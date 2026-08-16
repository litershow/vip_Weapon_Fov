#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "vip_fovweapon.h"

VIPFovWeapon g_VIPFovWeapon;

IVIPApi* g_pVIPCore = nullptr;
IMenusApi* g_pMenus = nullptr;
IUtilsApi* g_pUtils = nullptr;

IVEngineServer2* engine = nullptr;
static ISource2Server* s_pFovSource2Server = nullptr;
static ISchemaSystem* s_pSchemaSystem = nullptr;
CGameEntitySystem* g_pGameEntitySystem = nullptr;
CEntitySystem* g_pEntitySystem = nullptr;

std::vector<std::string> g_FOV[64];

PLUGIN_EXPOSE(VIPFovWeapon, g_VIPFovWeapon);

SH_DECL_HOOK3_void(IServerGameDLL, GameFrame, SH_NOATTRIB, 0, bool, bool, bool);

// -----------------------------------------------------------------------------
// Runtime schema offsets. No player/camera field offset is hard-coded.
// -----------------------------------------------------------------------------
static int g_iPlayerPawnHandleOffset = -1;
static int g_iCameraServicesOffset = -1;
static int g_iCameraFOVOffset = -1;
static int g_iCameraFOVStartOffset = -1;
static int g_iCameraFOVRateOffset = -1;
static int g_iCameraZoomOwnerOffset = -1;

#ifdef _WIN32
static constexpr const char* kServerModule = "server.dll";
#else
static constexpr const char* kServerModule = "libserver.so";
#endif

// 0 = no override. The detour only replaces native SetFOV target=0 for the
// matching live pawn; non-zero game zoom FOVs are never modified.
static std::array<int, 64> g_TargetFOV{};
static std::array<CEntityInstance*, 64> g_CachedPawn{};
static std::array<uint64_t, 64> g_InterceptedResets{};
static std::array<uint64_t, 64> g_EmergencyRepairs{};

// Recovered SysV ABI for the supplied Linux libserver.so:
//   bool CCSPlayerBase_CameraServices::SetFOV(ownerPawn, target, start, rate)
// represented as a free function with explicit this pointer.
using NativeSetFOVFn = bool (*)(void*, CEntityInstance*, int, int, float);
static NativeSetFOVFn g_pNativeSetFOVEntry = nullptr;    // patched entry point
static NativeSetFOVFn g_pOriginalSetFOV = nullptr;       // trampoline
static uintptr_t g_ServerBase = 0;
static uintptr_t g_NativeSetFOVRVA = 0;
static int g_NativeSetFOVMatches = 0;
static bool g_DetourInstalled = false;
static bool g_EntryWasPreHooked = false;
static const char* g_EntryState = "unresolved";
static const char* g_ResolverMethod = "none";
static int g_ServerModuleCandidates = 0;
static int g_BodyAnchorMatches = 0;
static int g_ResetCallSites = 0;
static int g_ResetUniqueTargets = 0;
static std::string g_SelectedModulePath;
static std::string g_EntryBytesHex;

#ifndef _WIN32
static constexpr size_t kDetourPatchSize = 17; // whole prologue instructions
static std::array<uint8_t, kDetourPatchSize> g_OriginalPrologue{};
static void* g_TrampolineMemory = nullptr;
static size_t g_TrampolineSize = 0;
#endif

// -----------------------------------------------------------------------------
// Schema / entity helpers
// -----------------------------------------------------------------------------
static int FindServerOffset(const char* className, const char* fieldName)
{
    if (!s_pSchemaSystem || !className || !fieldName)
        return -1;

    CSchemaSystemTypeScope* scope = s_pSchemaSystem->FindTypeScopeForModule(kServerModule);
    if (!scope)
        return -1;

    SchemaClassInfoData_t* classInfo = scope->FindDeclaredClass(className).Get();
    if (!classInfo || !classInfo->m_pFields)
        return -1;

    for (int i = 0; i < classInfo->m_nFieldCount; ++i)
    {
        SchemaClassFieldData_t& field = classInfo->m_pFields[i];
        if (field.m_pszName && std::strcmp(field.m_pszName, fieldName) == 0)
            return field.m_nSingleInheritanceOffset;
    }

    return -1;
}

static void ResolveOffsets()
{
    g_iPlayerPawnHandleOffset = FindServerOffset("CCSPlayerController", "m_hPlayerPawn");
    g_iCameraServicesOffset = FindServerOffset("CBasePlayerPawn", "m_pCameraServices");
    g_iCameraFOVOffset = FindServerOffset("CCSPlayerBase_CameraServices", "m_iFOV");
    g_iCameraFOVStartOffset = FindServerOffset("CCSPlayerBase_CameraServices", "m_iFOVStart");
    g_iCameraFOVRateOffset = FindServerOffset("CCSPlayerBase_CameraServices", "m_flFOVRate");
    g_iCameraZoomOwnerOffset = FindServerOffset("CCSPlayerBase_CameraServices", "m_hZoomOwner");

    ConColorMsg(
        Color(80, 220, 120, 255),
        "[VIP-FOVResolver6] schema: Pawn=0x%X CameraPtr=0x%X FOV=0x%X Start=0x%X Rate=0x%X Owner=0x%X\n",
        g_iPlayerPawnHandleOffset,
        g_iCameraServicesOffset,
        g_iCameraFOVOffset,
        g_iCameraFOVStartOffset,
        g_iCameraFOVRateOffset,
        g_iCameraZoomOwnerOffset
    );
}

static CEntityInstance* EntityFromIndex(int index)
{
    if (!g_pEntitySystem || index < 0 || index >= MAX_TOTAL_ENTITIES - 1)
        return nullptr;

    CEntityIdentity* chunk =
        g_pEntitySystem->m_EntityList.m_pIdentityChunks[index / MAX_ENTITIES_IN_LIST];
    if (!chunk)
        return nullptr;

    CEntityIdentity* identity = &chunk[index % MAX_ENTITIES_IN_LIST];
    if (identity->GetEntityIndex().Get() != index)
        return nullptr;

    return identity->m_pInstance;
}

static CEntityInstance* EntityFromHandle(const CEntityHandle& handle)
{
    if (!g_pEntitySystem || !handle.IsValid())
        return nullptr;

    const int index = handle.GetEntryIndex();
    if (index < 0 || index >= MAX_TOTAL_ENTITIES - 1)
        return nullptr;

    CEntityIdentity* chunk =
        g_pEntitySystem->m_EntityList.m_pIdentityChunks[index / MAX_ENTITIES_IN_LIST];
    if (!chunk)
        return nullptr;

    CEntityIdentity* identity = &chunk[index % MAX_ENTITIES_IN_LIST];
    if (identity->GetRefEHandle() != handle)
        return nullptr;

    return identity->m_pInstance;
}

static CEntityInstance* GetController(int slot)
{
    if (slot < 0 || slot >= 64)
        return nullptr;
    return EntityFromIndex(slot + 1);
}

static CEntityInstance* GetPawn(int slot)
{
    if (g_iPlayerPawnHandleOffset < 0)
        ResolveOffsets();

    CEntityInstance* controller = GetController(slot);
    if (!controller || g_iPlayerPawnHandleOffset < 0)
        return nullptr;

    const CEntityHandle& pawnHandle =
        *reinterpret_cast<const CEntityHandle*>(
            reinterpret_cast<uintptr_t>(controller) +
            static_cast<uintptr_t>(g_iPlayerPawnHandleOffset));

    return EntityFromHandle(pawnHandle);
}

static void* GetCameraServicesFromPawn(CEntityInstance* pawn)
{
    if (!pawn || g_iCameraServicesOffset < 0)
        return nullptr;

    return *reinterpret_cast<void**>(
        reinterpret_cast<uintptr_t>(pawn) +
        static_cast<uintptr_t>(g_iCameraServicesOffset));
}

static void* GetCameraServices(int slot)
{
    return GetCameraServicesFromPawn(GetPawn(slot));
}

static int ReadCameraFOV(int slot)
{
    void* camera = GetCameraServices(slot);
    if (!camera || g_iCameraFOVOffset < 0)
        return 0;

    return *reinterpret_cast<int*>(
        reinterpret_cast<uintptr_t>(camera) +
        static_cast<uintptr_t>(g_iCameraFOVOffset));
}

static int ReadCameraFOVStart(int slot)
{
    void* camera = GetCameraServices(slot);
    if (!camera || g_iCameraFOVStartOffset < 0)
        return 0;

    return *reinterpret_cast<int*>(
        reinterpret_cast<uintptr_t>(camera) +
        static_cast<uintptr_t>(g_iCameraFOVStartOffset));
}

static int FindSlotForPawn(CEntityInstance* pawn)
{
    if (!pawn)
        return -1;

    for (int slot = 0; slot < 64; ++slot)
    {
        if (g_TargetFOV[slot] > 0 && g_CachedPawn[slot] == pawn)
            return slot;
    }

    return -1;
}

// -----------------------------------------------------------------------------
// Native SetFOV resolver + Linux inline detour
// -----------------------------------------------------------------------------
#ifndef _WIN32
struct ExecutableRange
{
    uintptr_t begin = 0;
    uintptr_t end = 0;
};

struct ServerModuleScanInfo
{
    uintptr_t base = 0;
    std::vector<ExecutableRange> executable;
    std::string path;
};

struct ServerModuleCollection
{
    std::vector<ServerModuleScanInfo> modules;
};

static bool IsServerModulePath(const char* path)
{
    if (!path || !path[0])
        return false;

    const char* slash = std::strrchr(path, '/');
    const char* name = slash ? slash + 1 : path;
    return std::strcmp(name, "libserver.so") == 0 ||
           std::strcmp(name, "server.so") == 0;
}

static int FindServerModuleCallback(struct dl_phdr_info* info, size_t, void* opaque)
{
    if (!info || !opaque || !IsServerModulePath(info->dlpi_name))
        return 0;

    ServerModuleScanInfo module;
    module.base = static_cast<uintptr_t>(info->dlpi_addr);
    module.path = info->dlpi_name ? info->dlpi_name : "";

    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i)
    {
        const ElfW(Phdr)& phdr = info->dlpi_phdr[i];
        if (phdr.p_type != PT_LOAD || (phdr.p_flags & PF_X) == 0 || phdr.p_memsz == 0)
            continue;

        const uintptr_t begin = module.base + static_cast<uintptr_t>(phdr.p_vaddr);
        module.executable.push_back({begin, begin + static_cast<uintptr_t>(phdr.p_memsz)});
    }

    if (!module.executable.empty())
        reinterpret_cast<ServerModuleCollection*>(opaque)->modules.push_back(std::move(module));

    // Do not stop at the first basename match. A dedicated server may have
    // more than one module whose basename is server.so/libserver.so.
    return 0;
}

static uint32_t ReadU32Unaligned(const uint8_t* p)
{
    uint32_t value = 0;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

static bool MatchesNativeSetFOVBodyAnchor(const uint8_t* p, const uint8_t* end)
{
    if (!p || !end || p + 19 > end ||
        g_iCameraFOVOffset < 0 || g_iCameraZoomOwnerOffset < 0)
        return false;

    // Anchor begins at the native function's cmp m_iFOV, target instruction.
    // We deliberately do NOT inspect the function prologue: another plugin may
    // already have replaced the entry bytes with a detour while this body is
    // still intact.
    if (p[0] != 0x3B || p[1] != 0x97 ||
        ReadU32Unaligned(p + 2) != static_cast<uint32_t>(g_iCameraFOVOffset))
        return false;

    // je rel32
    if (p[6] != 0x0F || p[7] != 0x84)
        return false;

    // mov ecx, [r15 + m_hZoomOwner]
    if (p[12] != 0x41 || p[13] != 0x8B || p[14] != 0x8F ||
        ReadU32Unaligned(p + 15) != static_cast<uint32_t>(g_iCameraZoomOwnerOffset))
        return false;

    return true;
}

static bool LooksLikeCleanSetFOVEntry(const uint8_t* p)
{
    if (!p)
        return false;

    static constexpr uint8_t prologue[] = {
        0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56,
        0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC
    };
    return std::memcmp(p, prologue, sizeof(prologue)) == 0;
}

static bool DecodeExistingEntryJump(const uint8_t* entry, uintptr_t& destination, size_t& jumpSize)
{
    destination = 0;
    jumpSize = 0;
    if (!entry)
        return false;

    // mov rax, imm64 ; jmp rax
    if (entry[0] == 0x48 && entry[1] == 0xB8 && entry[10] == 0xFF && entry[11] == 0xE0)
    {
        std::memcpy(&destination, entry + 2, sizeof(destination));
        jumpSize = 12;
        return destination != 0;
    }

    // jmp rel32
    if (entry[0] == 0xE9)
    {
        int32_t rel = 0;
        std::memcpy(&rel, entry + 1, sizeof(rel));
        destination = reinterpret_cast<uintptr_t>(entry + 5) + static_cast<int64_t>(rel);
        jumpSize = 5;
        return destination != 0;
    }

    // mov r11, imm64 ; jmp r11 (common alternative detour stub)
    if (entry[0] == 0x49 && entry[1] == 0xBB && entry[10] == 0x41 &&
        entry[11] == 0xFF && entry[12] == 0xE3)
    {
        std::memcpy(&destination, entry + 2, sizeof(destination));
        jumpSize = 13;
        return destination != 0;
    }

    return false;
}

static bool AddressInExecutableModule(const ServerModuleScanInfo& module, uintptr_t address)
{
    for (const ExecutableRange& range : module.executable)
    {
        if (address >= range.begin && address < range.end)
            return true;
    }
    return false;
}

static bool WindowHasBytes(const uint8_t* begin, const uint8_t* end,
                           const uint8_t* needle, size_t needleSize)
{
    if (!begin || !end || !needle || needleSize == 0 || begin >= end)
        return false;

    for (const uint8_t* p = begin; p + needleSize <= end; ++p)
    {
        if (std::memcmp(p, needle, needleSize) == 0)
            return true;
    }
    return false;
}

static void AddUniqueAddress(std::vector<uintptr_t>& values, uintptr_t value)
{
    if (value != 0 && std::find(values.begin(), values.end(), value) == values.end())
        values.push_back(value);
}

static void ScanResetCallTargets(const ServerModuleScanInfo& module,
                                 std::vector<uintptr_t>& uniqueTargets,
                                 int& callSiteCount)
{
    if (g_iCameraServicesOffset < 0)
        return;

    static constexpr uint8_t kZeroEDX[] = {0x31, 0xD2};
    static constexpr uint8_t kZeroECX[] = {0x31, 0xC9};
    static constexpr uint8_t kZeroXMM0A[] = {0x66, 0x0F, 0xEF, 0xC0}; // pxor xmm0,xmm0
    static constexpr uint8_t kZeroXMM0B[] = {0x0F, 0x57, 0xC0};       // xorps xmm0,xmm0

    for (const ExecutableRange& range : module.executable)
    {
        const auto* begin = reinterpret_cast<const uint8_t*>(range.begin);
        const auto* end = reinterpret_cast<const uint8_t*>(range.end);
        if (end <= begin + 96)
            continue;

        for (const uint8_t* p = begin + 64; p + 16 <= end; ++p)
        {
            // mov rdi, [<pawn-register> + m_pCameraServices]
            // 48/49 8B /r with mod=disp32 and destination register RDI.
            const uint8_t rex = p[0];
            if ((rex != 0x48 && rex != 0x49) || p[1] != 0x8B)
                continue;

            const uint8_t modrm = p[2];
            if ((modrm & 0xF8) != 0xB8 || (modrm & 0x07) == 0x04)
                continue; // destination != rdi, or SIB form we intentionally skip

            if (ReadU32Unaligned(p + 3) != static_cast<uint32_t>(g_iCameraServicesOffset))
                continue;

            // Immediately after the camera load the supplied build moves the
            // pawn into RSI (second SetFOV argument), then performs call rel32.
            const uint8_t* q = p + 7;
            if ((q[0] != 0x48 && q[0] != 0x4C) || q[1] != 0x89 ||
                (q[2] & 0xC7) != 0xC6 || q[3] != 0xE8)
                continue;

            const uint8_t* windowBegin = p - 64;
            const bool zeroEDX = WindowHasBytes(windowBegin, p, kZeroEDX, sizeof(kZeroEDX));
            const bool zeroECX = WindowHasBytes(windowBegin, p, kZeroECX, sizeof(kZeroECX));
            const bool zeroXMM0 =
                WindowHasBytes(windowBegin, p, kZeroXMM0A, sizeof(kZeroXMM0A)) ||
                WindowHasBytes(windowBegin, p, kZeroXMM0B, sizeof(kZeroXMM0B));

            if (!zeroEDX || !zeroECX || !zeroXMM0)
                continue;

            int32_t rel = 0;
            std::memcpy(&rel, q + 4, sizeof(rel));
            const uintptr_t callAddress = reinterpret_cast<uintptr_t>(q + 3);
            const uintptr_t target = callAddress + 5 + static_cast<int64_t>(rel);
            if (!AddressInExecutableModule(module, target))
                continue;

            ++callSiteCount;
            AddUniqueAddress(uniqueTargets, target);
        }
    }
}

static std::string HexEntryBytes(const uint8_t* entry, size_t count)
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    if (!entry || count == 0)
        return out;
    out.reserve(count * 2);
    for (size_t i = 0; i < count; ++i)
    {
        out.push_back(kHex[(entry[i] >> 4) & 0xF]);
        out.push_back(kHex[entry[i] & 0xF]);
    }
    return out;
}

static bool ResolveNativeSetFOV()
{
    g_pNativeSetFOVEntry = nullptr;
    g_ServerBase = 0;
    g_NativeSetFOVRVA = 0;
    g_NativeSetFOVMatches = 0;
    g_EntryWasPreHooked = false;
    g_EntryState = "unresolved";
    g_ResolverMethod = "none";
    g_ServerModuleCandidates = 0;
    g_BodyAnchorMatches = 0;
    g_ResetCallSites = 0;
    g_ResetUniqueTargets = 0;
    g_SelectedModulePath.clear();
    g_EntryBytesHex.clear();

    ServerModuleCollection collection;
    dl_iterate_phdr(FindServerModuleCallback, &collection);
    g_ServerModuleCandidates = static_cast<int>(collection.modules.size());

    if (collection.modules.empty())
    {
        ConColorMsg(Color(255, 80, 80, 255),
            "[VIP-FOVResolver6] no loaded server.so/libserver.so executable module found.\n");
        return false;
    }

    std::vector<uintptr_t> bodyEntries;
    std::vector<uintptr_t> resetTargets;

    for (const ServerModuleScanInfo& module : collection.modules)
    {
        // Resolver A: old high-confidence body anchor. Useful when unchanged.
        for (const ExecutableRange& range : module.executable)
        {
            const auto* begin = reinterpret_cast<const uint8_t*>(range.begin);
            const auto* end = reinterpret_cast<const uint8_t*>(range.end);

            for (const uint8_t* p = begin; p + 19 <= end; ++p)
            {
                if (!MatchesNativeSetFOVBodyAnchor(p, end))
                    continue;

                ++g_BodyAnchorMatches;
                constexpr ptrdiff_t kAnchorFromEntry = 0x2A;
                if (p < begin + kAnchorFromEntry)
                    continue;

                AddUniqueAddress(bodyEntries,
                    reinterpret_cast<uintptr_t>(p - kAnchorFromEntry));
            }
        }

        // Resolver B: locate the game's own reset call SetFOV(camera,pawn,0,0,0)
        // by m_pCameraServices schema displacement, then decode E8 rel32 target.
        ScanResetCallTargets(module, resetTargets, g_ResetCallSites);
    }

    g_ResetUniqueTargets = static_cast<int>(resetTargets.size());

    uintptr_t chosen = 0;
    if (bodyEntries.size() == 1 && resetTargets.size() == 1)
    {
        if (bodyEntries[0] != resetTargets[0])
        {
            g_ResolverMethod = "conflict";
            ConColorMsg(Color(255, 80, 80, 255),
                "[VIP-FOVResolver6] resolver conflict: body=0x%lX resetTarget=0x%lX; hook disabled.\n",
                static_cast<unsigned long>(bodyEntries[0]),
                static_cast<unsigned long>(resetTargets[0]));
            return false;
        }
        chosen = bodyEntries[0];
        g_ResolverMethod = "body+reset-call";
    }
    else if (resetTargets.size() == 1)
    {
        chosen = resetTargets[0];
        g_ResolverMethod = "reset-call";
    }
    else if (bodyEntries.size() == 1)
    {
        chosen = bodyEntries[0];
        g_ResolverMethod = "body";
    }
    else
    {
        g_NativeSetFOVMatches = static_cast<int>(bodyEntries.size());
        ConColorMsg(Color(255, 150, 50, 255),
            "[VIP-FOVResolver6] unresolved: modules=%d body=%zu resetSites=%d resetTargets=%zu.\n",
            g_ServerModuleCandidates,
            bodyEntries.size(),
            g_ResetCallSites,
            resetTargets.size());
        return false;
    }

    const ServerModuleScanInfo* selectedModule = nullptr;
    for (const ServerModuleScanInfo& module : collection.modules)
    {
        if (AddressInExecutableModule(module, chosen))
        {
            selectedModule = &module;
            break;
        }
    }

    if (!selectedModule)
    {
        g_ResolverMethod = "target-outside-module";
        return false;
    }

    g_ServerBase = selectedModule->base;
    g_SelectedModulePath = selectedModule->path;
    g_NativeSetFOVRVA = chosen - selectedModule->base;
    g_pNativeSetFOVEntry = reinterpret_cast<NativeSetFOVFn>(chosen);
    g_NativeSetFOVMatches = 1;

    const uint8_t* entry = reinterpret_cast<const uint8_t*>(chosen);
    g_EntryBytesHex = HexEntryBytes(entry, 16);

    uintptr_t chainedTarget = 0;
    size_t chainedJumpSize = 0;
    if (LooksLikeCleanSetFOVEntry(entry))
    {
        g_EntryWasPreHooked = false;
        g_EntryState = "clean";
    }
    else if (DecodeExistingEntryJump(entry, chainedTarget, chainedJumpSize))
    {
        g_EntryWasPreHooked = true;
        g_EntryState = "prehooked-chainable";
    }
    else
    {
        // We found the native target, but do not overwrite an unknown prologue.
        // !fovscan6 exposes the first 16 bytes so the resolver can be adapted
        // without risking a server crash.
        g_EntryState = "unknown-entry";
        ConColorMsg(Color(255, 150, 50, 255),
            "[VIP-FOVResolver6] target found by %s at +0x%lX but entry is unknown (%s); hook disabled.\n",
            g_ResolverMethod,
            static_cast<unsigned long>(g_NativeSetFOVRVA),
            g_EntryBytesHex.c_str());
        g_pNativeSetFOVEntry = nullptr;
        return false;
    }

    ConColorMsg(Color(80, 220, 120, 255),
        "[VIP-FOVResolver6] SetFOV resolved by %s: %s + 0x%lX entry=%s modules=%d body=%d resetSites=%d.\n",
        g_ResolverMethod,
        g_SelectedModulePath.c_str(),
        static_cast<unsigned long>(g_NativeSetFOVRVA),
        g_EntryState,
        g_ServerModuleCandidates,
        g_BodyAnchorMatches,
        g_ResetCallSites);
    return true;
}

static bool HookedSetFOV(void* camera, CEntityInstance* owner, int target, int start, float rate)
{
    NativeSetFOVFn original = g_pOriginalSetFOV;
    if (!original)
        return false;

    const int slot = FindSlotForPawn(owner);
    if (slot >= 0 && target == 0)
    {
        const int desired = g_TargetFOV[slot];
        if (desired > 0)
        {
            ++g_InterceptedResets[slot];
            // Critical part: the game never writes/sends zero. Replace the
            // reset before the native function touches CameraServices.
            return original(camera, owner, desired, desired, 0.0f);
        }
    }

    return original(camera, owner, target, start, rate);
}

static void WriteAbsoluteJump(uint8_t* dst, uintptr_t target)
{
    // mov rax, imm64 ; jmp rax
    dst[0] = 0x48;
    dst[1] = 0xB8;
    std::memcpy(dst + 2, &target, sizeof(target));
    dst[10] = 0xFF;
    dst[11] = 0xE0;
}

static bool MakeCodeWritable(void* address, size_t length, int protection)
{
    const long pageSizeLong = sysconf(_SC_PAGESIZE);
    if (pageSizeLong <= 0)
        return false;

    const uintptr_t pageSize = static_cast<uintptr_t>(pageSizeLong);
    const uintptr_t begin = reinterpret_cast<uintptr_t>(address) & ~(pageSize - 1);
    const uintptr_t end = (reinterpret_cast<uintptr_t>(address) + length + pageSize - 1) & ~(pageSize - 1);
    return mprotect(reinterpret_cast<void*>(begin), end - begin, protection) == 0;
}

static bool InstallNativeSetFOVDetour()
{
    if (g_DetourInstalled)
        return true;

    if (!g_pNativeSetFOVEntry && !ResolveNativeSetFOV())
        return false;

    uint8_t* entry = reinterpret_cast<uint8_t*>(g_pNativeSetFOVEntry);
    std::memcpy(g_OriginalPrologue.data(), entry, kDetourPatchSize);

    uintptr_t chainedTarget = 0;
    size_t chainedJumpSize = 0;
    if (DecodeExistingEntryJump(entry, chainedTarget, chainedJumpSize))
    {
        // Another plugin already owns the native entry. Chain through its
        // destination instead of trying to execute its jump bytes in a trampoline.
        g_EntryWasPreHooked = true;
        g_EntryState = "prehooked-chainable";
        g_pOriginalSetFOV = reinterpret_cast<NativeSetFOVFn>(chainedTarget);
    }
    else if (LooksLikeCleanSetFOVEntry(entry))
    {
        g_EntryWasPreHooked = false;
        g_EntryState = "clean";

        const long pageSizeLong = sysconf(_SC_PAGESIZE);
        if (pageSizeLong <= 0)
            return false;

        g_TrampolineSize = static_cast<size_t>(pageSizeLong);
        g_TrampolineMemory = mmap(
            nullptr,
            g_TrampolineSize,
            PROT_READ | PROT_WRITE | PROT_EXEC,
            MAP_PRIVATE | MAP_ANONYMOUS,
            -1,
            0);

        if (g_TrampolineMemory == MAP_FAILED)
        {
            g_TrampolineMemory = nullptr;
            ConColorMsg(Color(255, 80, 80, 255),
                "[VIP-FOVResolver6] mmap trampoline failed; detour DISABLED.\n");
            return false;
        }

        auto* trampoline = reinterpret_cast<uint8_t*>(g_TrampolineMemory);
        std::memcpy(trampoline, g_OriginalPrologue.data(), kDetourPatchSize);
        WriteAbsoluteJump(
            trampoline + kDetourPatchSize,
            reinterpret_cast<uintptr_t>(entry + kDetourPatchSize));
        __builtin___clear_cache(
            reinterpret_cast<char*>(trampoline),
            reinterpret_cast<char*>(trampoline + kDetourPatchSize + 12));

        g_pOriginalSetFOV = reinterpret_cast<NativeSetFOVFn>(trampoline);
    }
    else
    {
        g_EntryState = "unknown-entry";
        ConColorMsg(Color(255, 80, 80, 255),
            "[VIP-FOVResolver6] SetFOV entry has an unknown patch; refusing to overwrite it.\n");
        return false;
    }

    if (!MakeCodeWritable(entry, kDetourPatchSize, PROT_READ | PROT_WRITE | PROT_EXEC))
    {
        if (g_TrampolineMemory)
        {
            munmap(g_TrampolineMemory, g_TrampolineSize);
            g_TrampolineMemory = nullptr;
            g_TrampolineSize = 0;
        }
        g_pOriginalSetFOV = nullptr;
        ConColorMsg(Color(255, 80, 80, 255),
            "[VIP-FOVResolver6] mprotect libserver text failed; detour DISABLED.\n");
        return false;
    }

    WriteAbsoluteJump(entry, reinterpret_cast<uintptr_t>(&HookedSetFOV));
    for (size_t i = 12; i < kDetourPatchSize; ++i)
        entry[i] = 0x90;

    __builtin___clear_cache(
        reinterpret_cast<char*>(entry),
        reinterpret_cast<char*>(entry + kDetourPatchSize));
    MakeCodeWritable(entry, kDetourPatchSize, PROT_READ | PROT_EXEC);

    g_DetourInstalled = true;
    ConColorMsg(Color(80, 220, 120, 255),
        "[VIP-FOVResolver6] SetFOV detour INSTALLED at libserver.so+0x%lX (%s).\n",
        static_cast<unsigned long>(g_NativeSetFOVRVA),
        g_EntryState);
    return true;
}

static void RemoveNativeSetFOVDetour()
{
    if (!g_DetourInstalled || !g_pNativeSetFOVEntry)
        return;

    uint8_t* entry = reinterpret_cast<uint8_t*>(g_pNativeSetFOVEntry);
    if (MakeCodeWritable(entry, kDetourPatchSize, PROT_READ | PROT_WRITE | PROT_EXEC))
    {
        std::memcpy(entry, g_OriginalPrologue.data(), kDetourPatchSize);
        __builtin___clear_cache(
            reinterpret_cast<char*>(entry),
            reinterpret_cast<char*>(entry + kDetourPatchSize));
        MakeCodeWritable(entry, kDetourPatchSize, PROT_READ | PROT_EXEC);
    }

    g_DetourInstalled = false;
    g_pOriginalSetFOV = nullptr;

    if (g_TrampolineMemory)
    {
        munmap(g_TrampolineMemory, g_TrampolineSize);
        g_TrampolineMemory = nullptr;
        g_TrampolineSize = 0;
    }

    ConColorMsg(Color(220, 220, 80, 255),
        "[VIP-FOVResolver6] SetFOV detour removed.\n");
}
#else
static bool ResolveNativeSetFOV()
{
    ConColorMsg(Color(255, 150, 50, 255),
        "[VIP-FOVResolver6] native detour is Linux-only.\n");
    return false;
}

static bool InstallNativeSetFOVDetour() { return false; }
static void RemoveNativeSetFOVDetour() {}
#endif

// -----------------------------------------------------------------------------
// FOV application. Calls the ORIGINAL native routine via trampoline.
// -----------------------------------------------------------------------------
static bool CallOriginalSetFOV(int slot, int target, int start, float rate)
{
    if (!g_DetourInstalled || !g_pOriginalSetFOV || slot < 0 || slot >= 64)
        return false;

    CEntityInstance* pawn = GetPawn(slot);
    if (!pawn)
        return false;

    g_CachedPawn[slot] = pawn;
    void* camera = GetCameraServicesFromPawn(pawn);
    if (!camera)
        return false;

    return g_pOriginalSetFOV(camera, pawn, target, start, rate);
}

static bool SetTargetFOV(int slot, int value)
{
    if (slot < 0 || slot >= 64 || value < 60 || value > 179 || !g_DetourInstalled)
        return false;

    g_TargetFOV[slot] = value;
    g_CachedPawn[slot] = GetPawn(slot);

    if (!CallOriginalSetFOV(slot, value, value, 0.0f))
    {
        g_TargetFOV[slot] = 0;
        return false;
    }

    return true;
}

static void ClearTargetFOV(int slot, bool restoreGameFOV)
{
    if (slot < 0 || slot >= 64)
        return;

    // Clear first so our detour allows the native target=0 call through.
    g_TargetFOV[slot] = 0;

    if (restoreGameFOV && g_DetourInstalled)
        CallOriginalSetFOV(slot, 0, 0, 0.0f);

    g_CachedPawn[slot] = nullptr;
}

// -----------------------------------------------------------------------------
// Commands / VIP menu
// -----------------------------------------------------------------------------
static std::string CleanCommandToken(std::string token)
{
    while (!token.empty() && (token.front() == '"' || token.front() == '\''))
        token.erase(token.begin());
    while (!token.empty() && (token.back() == '"' || token.back() == '\'' ||
                              token.back() == '\r' || token.back() == '\n'))
        token.pop_back();
    return token;
}

static std::string LastToken(const char* text)
{
    if (!text)
        return "";

    std::stringstream ss(text);
    std::string token;
    std::string last;
    while (ss >> token)
        last = token;
    return CleanCommandToken(last);
}

static void SplitList(const char* text, std::vector<std::string>& output)
{
    output.clear();
    if (!text || !text[0])
        return;

    std::stringstream stream(text);
    std::string token;
    while (std::getline(stream, token, ','))
    {
        const std::size_t first = token.find_first_not_of(" \t\r\n");
        const std::size_t last = token.find_last_not_of(" \t\r\n");
        if (first != std::string::npos)
            output.push_back(token.substr(first, last - first + 1));
    }
}

static bool CommandFOVHook(int slot, const char* content)
{
    if (!g_pUtils || slot < 0 || slot >= 64)
        return true;

    const std::string token = LastToken(content);
    if (token == "off" || token == "0")
    {
        ClearTargetFOV(slot, true);
        g_pUtils->PrintToChat(slot, "[FOV6] override OFF; native game FOV restored");
        return true;
    }

    const int value = std::strtol(token.c_str(), nullptr, 10);
    if (value < 60 || value > 179)
    {
        g_pUtils->PrintToChat(slot, "[FOV6] Usage: !fovv6 120 (60..179) or !fovv6 off");
        return true;
    }

    if (!g_DetourInstalled)
    {
        g_pUtils->PrintToChat(
            slot,
            "[FOV6] detour unavailable: resolver=%s body=%d resetSites=%d targets=%d; use !fovscan6",
            g_ResolverMethod,
            g_BodyAnchorMatches,
            g_ResetCallSites,
            g_ResetUniqueTargets);
        return true;
    }

    if (!SetTargetFOV(slot, value))
    {
        g_pUtils->PrintToChat(slot, "[FOV6] native SetFOV failed; check !fovscan6");
        return true;
    }

    g_pUtils->PrintToChat(
        slot,
        "[FOV6] target=%d camera=%d/%d detour=ON (zero-reset blocked)",
        g_TargetFOV[slot],
        ReadCameraFOV(slot),
        ReadCameraFOVStart(slot));
    return true;
}

static bool CommandFOVDiag(int slot, const char*)
{
    if (!g_pUtils || slot < 0 || slot >= 64)
        return true;

    g_CachedPawn[slot] = GetPawn(slot);

    g_pUtils->PrintToChat(
        slot,
        "[FOV6] target=%d cam=%d/%d detour=%s resetsBlocked=%llu emergency=%llu",
        g_TargetFOV[slot],
        ReadCameraFOV(slot),
        ReadCameraFOVStart(slot),
        g_DetourInstalled ? "YES" : "NO",
        static_cast<unsigned long long>(g_InterceptedResets[slot]),
        static_cast<unsigned long long>(g_EmergencyRepairs[slot]));

    g_pUtils->PrintToChat(
        slot,
        "[FOV6] resolver=%s modules=%d body=%d resetSites=%d targets=%d",
        g_ResolverMethod,
        g_ServerModuleCandidates,
        g_BodyAnchorMatches,
        g_ResetCallSites,
        g_ResetUniqueTargets);

    g_pUtils->PrintToChat(
        slot,
        "[FOV6] native rva=0x%lX entry=%s pawn=%s bytes=%s",
        static_cast<unsigned long>(g_NativeSetFOVRVA),
        g_EntryState,
        g_CachedPawn[slot] ? "OK" : "NULL",
        g_EntryBytesHex.empty() ? "-" : g_EntryBytesHex.c_str());

    g_pUtils->PrintToChat(
        slot,
        "[FOV6] schema camPtr=0x%X FOV=0x%X Start=0x%X Owner=0x%X",
        g_iCameraServicesOffset,
        g_iCameraFOVOffset,
        g_iCameraFOVStartOffset,
        g_iCameraZoomOwnerOffset);

    return true;
}

static bool OpenFOVMenu(int slot, const char*);

static void VIP_OnClientLoaded_FOVWeapon(int slot, bool isVIP)
{
    if (slot < 0 || slot >= 64)
        return;

    g_FOV[slot].clear();
    g_TargetFOV[slot] = 0;
    g_CachedPawn[slot] = nullptr;
    g_InterceptedResets[slot] = 0;
    g_EmergencyRepairs[slot] = 0;

    if (!isVIP || !g_pVIPCore)
        return;

    SplitList(g_pVIPCore->VIP_GetClientFeatureString(slot, "FOV"), g_FOV[slot]);
}

static void VIP_OnClientDisconnect_FOVWeapon(int slot, bool)
{
    if (slot < 0 || slot >= 64)
        return;

    g_FOV[slot].clear();
    g_TargetFOV[slot] = 0;
    g_CachedPawn[slot] = nullptr;
}

static void VIP_OnPlayerSpawn_FOVWeapon(int slot, int, bool isVIP)
{
    if (slot < 0 || slot >= 64 || !isVIP || !g_pVIPCore || g_FOV[slot].empty())
        return;

    const char* cookie = g_pVIPCore->VIP_GetClientCookie(slot, "FOV_Value");
    const int selected = (cookie && cookie[0]) ? std::strtol(cookie, nullptr, 10) : 90;
    if (selected < 60 || selected > 179)
        return;

    g_TargetFOV[slot] = selected;
    // Pawn is refreshed and the target is applied by the next post GameFrame.
    g_CachedPawn[slot] = nullptr;
}

static bool OpenFOVMenu(int slot, const char*)
{
    if (slot < 0 || slot >= 64 || !g_pMenus || !g_pVIPCore)
        return false;

    Menu menu;
    const char* translated = g_pVIPCore->VIP_GetTranslate("FOV_Title");
    g_pMenus->SetTitleMenu(menu, (translated && translated[0]) ? translated : "Select FOV");

    for (const auto& value : g_FOV[slot])
        g_pMenus->AddItemMenu(menu, value.c_str(), value.c_str());

    g_pMenus->SetExitMenu(menu, true);
    g_pMenus->SetBackMenu(menu, true);
    g_pMenus->SetCallback(
        menu,
        [](const char* back, const char*, int item, int slot)
        {
            if (slot < 0 || slot >= 64)
                return;

            if (item < static_cast<int>(g_FOV[slot].size()))
            {
                const int value = std::strtol(back, nullptr, 10);
                if (value >= 60 && value <= 179 && SetTargetFOV(slot, value))
                    g_pVIPCore->VIP_SetClientCookie(slot, "FOV_Value", strdup(back));

                OpenFOVMenu(slot, "FOV");
            }
            else
            {
                g_pVIPCore->VIP_OpenMenu(slot);
            }
        });

    g_pMenus->DisplayPlayerMenu(menu, slot);
    return false;
}

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------
bool VIPFovWeapon::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
    PLUGIN_SAVEVARS();

    GET_V_IFACE_ANY(GetEngineFactory, s_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetServerFactory, s_pFovSource2Server, ISource2Server, SOURCE2SERVER_INTERFACE_VERSION);

    if (s_pFovSource2Server)
    {
        SH_ADD_HOOK(
            IServerGameDLL,
            GameFrame,
            s_pFovSource2Server,
            SH_MEMBER(this, &VIPFovWeapon::GameFrame),
            true);
    }

    g_SMAPI->AddListener(this, this);
    return true;
}

bool VIPFovWeapon::Unload(char* error, size_t maxlen)
{
    // Restore active users before removing the detour.
    for (int slot = 0; slot < 64; ++slot)
    {
        if (g_TargetFOV[slot] > 0)
            ClearTargetFOV(slot, true);
    }

    RemoveNativeSetFOVDetour();

    if (s_pFovSource2Server)
    {
        SH_REMOVE_HOOK(
            IServerGameDLL,
            GameFrame,
            s_pFovSource2Server,
            SH_MEMBER(this, &VIPFovWeapon::GameFrame),
            true);
    }

    if (g_pUtils)
        g_pUtils->ClearAllHooks(g_PLID);

    return true;
}

static void OnStartupServer()
{
    if (!g_pUtils)
        return;

    g_pGameEntitySystem = g_pUtils->GetCGameEntitySystem();
    g_pEntitySystem = g_pUtils->GetCEntitySystem();

    ResolveOffsets();

    const bool schemaOK =
        g_iPlayerPawnHandleOffset >= 0 &&
        g_iCameraServicesOffset >= 0 &&
        g_iCameraFOVOffset >= 0 &&
        g_iCameraFOVStartOffset >= 0 &&
        g_iCameraZoomOwnerOffset >= 0;

    if (!schemaOK)
    {
        ConColorMsg(Color(255, 80, 80, 255),
            "[VIP-FOVResolver6] required schema fields missing; detour DISABLED.\n");
        return;
    }

    if (!InstallNativeSetFOVDetour())
    {
        ConColorMsg(Color(255, 80, 80, 255),
            "[VIP-FOVResolver6] hook was NOT installed. !fovv6 will refuse to enable.\n");
    }
}

void VIPFovWeapon::AllPluginsLoaded()
{
    int ret = 0;

    g_pUtils = reinterpret_cast<IUtilsApi*>(g_SMAPI->MetaFactory(Utils_INTERFACE, &ret, nullptr));
    if (ret == META_IFACE_FAILED || !g_pUtils)
    {
        ConColorMsg(Color(255, 0, 0, 255), "[VIP-FOVResolver6] Utils API not found.\n");
        return;
    }

    g_pMenus = reinterpret_cast<IMenusApi*>(g_SMAPI->MetaFactory(Menus_INTERFACE, &ret, nullptr));
    if (ret == META_IFACE_FAILED || !g_pMenus)
    {
        ConColorMsg(Color(255, 0, 0, 255), "[VIP-FOVResolver6] Menus API not found.\n");
        return;
    }

    g_pVIPCore = reinterpret_cast<IVIPApi*>(g_SMAPI->MetaFactory(VIP_INTERFACE, &ret, nullptr));
    if (ret == META_IFACE_FAILED || !g_pVIPCore)
    {
        ConColorMsg(Color(255, 0, 0, 255), "[VIP-FOVResolver6] VIP Core not found.\n");
        return;
    }

    // Unique names: do not collide with any of the old experimental builds.
    g_pUtils->RegCommand(g_PLID, {"mm_fovv6"}, {"!fovv6"}, CommandFOVHook);
    g_pUtils->RegCommand(g_PLID, {"mm_fovscan6"}, {"!fovscan6"}, CommandFOVDiag);

    g_pUtils->StartupServer(g_PLID, OnStartupServer);

    g_pVIPCore->VIP_OnClientLoaded(VIP_OnClientLoaded_FOVWeapon);
    g_pVIPCore->VIP_OnClientDisconnect(VIP_OnClientDisconnect_FOVWeapon);
    g_pVIPCore->VIP_OnPlayerSpawn(VIP_OnPlayerSpawn_FOVWeapon);
    g_pVIPCore->VIP_RegisterFeature("FOV", VIP_STRING, SELECTABLE, OpenFOVMenu);

    ConColorMsg(Color(80, 220, 120, 255),
        "[VIP-FOVResolver6] loaded. Test only: !fovv6 120 / !fovscan6 / !fovv6 off\n");
}

void VIPFovWeapon::GameFrame(bool simulating, bool, bool)
{
    if (!simulating || !g_DetourInstalled)
        return;

    for (int slot = 0; slot < 64; ++slot)
    {
        const int target = g_TargetFOV[slot];
        if (target <= 0)
            continue;

        CEntityInstance* pawn = GetPawn(slot);
        if (!pawn)
        {
            g_CachedPawn[slot] = nullptr;
            continue;
        }

        const bool pawnChanged = g_CachedPawn[slot] != pawn;
        g_CachedPawn[slot] = pawn;

        // New spawn/new pawn: apply once. Normally no per-frame write occurs.
        if (pawnChanged)
        {
            CallOriginalSetFOV(slot, target, target, 0.0f);
            continue;
        }

        // Emergency only: if some code path writes the fields directly instead
        // of calling SetFOV, repair it here. With the native detour working this
        // counter should remain zero during ordinary weapon switching.
        if (ReadCameraFOV(slot) == 0 && ReadCameraFOVStart(slot) == 0)
        {
            if (CallOriginalSetFOV(slot, target, target, 0.0f))
                ++g_EmergencyRepairs[slot];
        }
    }
}

const char* VIPFovWeapon::GetLicense() { return "Public"; }
const char* VIPFovWeapon::GetVersion() { return "6.0-reset-call-resolver"; }
const char* VIPFovWeapon::GetDate() { return __DATE__; }
const char* VIPFovWeapon::GetLogTag() { return "[VIP-FOVResolver6]"; }
const char* VIPFovWeapon::GetAuthor() { return "Pisex VIP_FOV adaptation + native CameraServices SetFOV reset-call resolver"; }
const char* VIPFovWeapon::GetDescription() { return "VIP FOV that resolves native SetFOV from the game reset call and blocks zero-FOV resets before they reach the client."; }
const char* VIPFovWeapon::GetName() { return "[VIP] FOV + Weapon Resolver6"; }
const char* VIPFovWeapon::GetURL() { return "https://github.com/Pisex/cs2-vip-modules"; }
