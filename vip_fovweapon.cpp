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
        "[VIP-FOVDetour] schema: Pawn=0x%X CameraPtr=0x%X FOV=0x%X Start=0x%X Rate=0x%X Owner=0x%X\n",
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

    auto* out = reinterpret_cast<ServerModuleScanInfo*>(opaque);
    out->base = static_cast<uintptr_t>(info->dlpi_addr);
    out->path = info->dlpi_name ? info->dlpi_name : "";

    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i)
    {
        const ElfW(Phdr)& phdr = info->dlpi_phdr[i];
        if (phdr.p_type != PT_LOAD || (phdr.p_flags & PF_X) == 0 || phdr.p_memsz == 0)
            continue;

        const uintptr_t begin = out->base + static_cast<uintptr_t>(phdr.p_vaddr);
        out->executable.push_back({begin, begin + static_cast<uintptr_t>(phdr.p_memsz)});
    }

    return 1;
}

static uint32_t ReadU32Unaligned(const uint8_t* p)
{
    uint32_t value = 0;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

static bool MatchesNativeSetFOVShape(const uint8_t* p, const uint8_t* end)
{
    if (!p || !end || p + 61 > end)
        return false;

    // Exact instruction shape from the supplied libserver.so, but branch
    // displacements are wildcarded and schema field displacements are checked
    // against runtime SchemaSystem values.
    static constexpr uint8_t prefix[] = {
        0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56,
        0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC,
        0x58, 0xF3, 0x0F, 0x11, 0x45, 0x8C, 0x48, 0x85,
        0xF6, 0x0F, 0x84
    };

    if (std::memcmp(p, prefix, sizeof(prefix)) != 0)
        return false;

    static constexpr uint8_t middle[] = {
        0x49, 0x89, 0xFF,
        0x49, 0x89, 0xF6,
        0x89, 0xD3,
        0x41, 0x89, 0xCC,
        0x3B, 0x97
    };

    if (std::memcmp(p + 31, middle, sizeof(middle)) != 0)
        return false;

    if (g_iCameraFOVOffset < 0 ||
        ReadU32Unaligned(p + 44) != static_cast<uint32_t>(g_iCameraFOVOffset))
        return false;

    if (p[48] != 0x0F || p[49] != 0x84 ||
        p[54] != 0x41 || p[55] != 0x8B || p[56] != 0x8F)
        return false;

    if (g_iCameraZoomOwnerOffset < 0 ||
        ReadU32Unaligned(p + 57) != static_cast<uint32_t>(g_iCameraZoomOwnerOffset))
        return false;

    return true;
}

static bool ResolveNativeSetFOV()
{
    g_pNativeSetFOVEntry = nullptr;
    g_ServerBase = 0;
    g_NativeSetFOVRVA = 0;
    g_NativeSetFOVMatches = 0;

    ServerModuleScanInfo module;
    dl_iterate_phdr(FindServerModuleCallback, &module);

    if (module.base == 0 || module.executable.empty())
    {
        ConColorMsg(Color(255, 80, 80, 255),
            "[VIP-FOVDetour] loaded server module/executable range not found.\n");
        return false;
    }

    uintptr_t onlyMatch = 0;
    for (const ExecutableRange& range : module.executable)
    {
        const auto* begin = reinterpret_cast<const uint8_t*>(range.begin);
        const auto* end = reinterpret_cast<const uint8_t*>(range.end);

        for (const uint8_t* p = begin; p + 61 <= end; ++p)
        {
            if (!MatchesNativeSetFOVShape(p, end))
                continue;

            ++g_NativeSetFOVMatches;
            onlyMatch = reinterpret_cast<uintptr_t>(p);
        }
    }

    g_ServerBase = module.base;

    if (g_NativeSetFOVMatches != 1 || onlyMatch == 0)
    {
        ConColorMsg(Color(255, 150, 50, 255),
            "[VIP-FOVDetour] SetFOV signature matches=%d in %s; detour DISABLED.\n",
            g_NativeSetFOVMatches,
            module.path.c_str());
        return false;
    }

    g_NativeSetFOVRVA = onlyMatch - module.base;
    g_pNativeSetFOVEntry = reinterpret_cast<NativeSetFOVFn>(onlyMatch);

    ConColorMsg(Color(80, 220, 120, 255),
        "[VIP-FOVDetour] native SetFOV found: %s + 0x%lX.\n",
        module.path.c_str(),
        static_cast<unsigned long>(g_NativeSetFOVRVA));
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

    // 17 copied bytes + 12-byte absolute jump back, rounded to one page.
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
            "[VIP-FOVDetour] mmap trampoline failed; detour DISABLED.\n");
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

    if (!MakeCodeWritable(entry, kDetourPatchSize, PROT_READ | PROT_WRITE | PROT_EXEC))
    {
        munmap(g_TrampolineMemory, g_TrampolineSize);
        g_TrampolineMemory = nullptr;
        g_TrampolineSize = 0;
        g_pOriginalSetFOV = nullptr;
        ConColorMsg(Color(255, 80, 80, 255),
            "[VIP-FOVDetour] mprotect libserver text failed; detour DISABLED.\n");
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
        "[VIP-FOVDetour] SetFOV detour INSTALLED at libserver.so+0x%lX.\n",
        static_cast<unsigned long>(g_NativeSetFOVRVA));
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
        "[VIP-FOVDetour] SetFOV detour removed.\n");
}
#else
static bool ResolveNativeSetFOV()
{
    ConColorMsg(Color(255, 150, 50, 255),
        "[VIP-FOVDetour] native detour is Linux-only.\n");
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
        g_pUtils->PrintToChat(slot, "[FOV4] override OFF; native game FOV restored");
        return true;
    }

    const int value = std::strtol(token.c_str(), nullptr, 10);
    if (value < 60 || value > 179)
    {
        g_pUtils->PrintToChat(slot, "[FOV4] Usage: !fovhook 120 (60..179) or !fovhook off");
        return true;
    }

    if (!g_DetourInstalled)
    {
        g_pUtils->PrintToChat(
            slot,
            "[FOV4] detour unavailable: matches=%d rva=0x%lX. See server console",
            g_NativeSetFOVMatches,
            static_cast<unsigned long>(g_NativeSetFOVRVA));
        return true;
    }

    if (!SetTargetFOV(slot, value))
    {
        g_pUtils->PrintToChat(slot, "[FOV4] native SetFOV failed; check !fovhookdiag");
        return true;
    }

    g_pUtils->PrintToChat(
        slot,
        "[FOV4] target=%d camera=%d/%d detour=ON (zero-reset blocked)",
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
        "[FOV4] target=%d cam=%d/%d detour=%s resetsBlocked=%llu emergency=%llu",
        g_TargetFOV[slot],
        ReadCameraFOV(slot),
        ReadCameraFOVStart(slot),
        g_DetourInstalled ? "YES" : "NO",
        static_cast<unsigned long long>(g_InterceptedResets[slot]),
        static_cast<unsigned long long>(g_EmergencyRepairs[slot]));

    g_pUtils->PrintToChat(
        slot,
        "[FOV4] native matches=%d rva=0x%lX pawn=%s schema camPtr=0x%X FOV=0x%X Start=0x%X",
        g_NativeSetFOVMatches,
        static_cast<unsigned long>(g_NativeSetFOVRVA),
        g_CachedPawn[slot] ? "OK" : "NULL",
        g_iCameraServicesOffset,
        g_iCameraFOVOffset,
        g_iCameraFOVStartOffset);

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
            "[VIP-FOVDetour] required schema fields missing; detour DISABLED.\n");
        return;
    }

    if (!InstallNativeSetFOVDetour())
    {
        ConColorMsg(Color(255, 80, 80, 255),
            "[VIP-FOVDetour] hook was NOT installed. !fovhook will refuse to enable.\n");
    }
}

void VIPFovWeapon::AllPluginsLoaded()
{
    int ret = 0;

    g_pUtils = reinterpret_cast<IUtilsApi*>(g_SMAPI->MetaFactory(Utils_INTERFACE, &ret, nullptr));
    if (ret == META_IFACE_FAILED || !g_pUtils)
    {
        ConColorMsg(Color(255, 0, 0, 255), "[VIP-FOVDetour] Utils API not found.\n");
        return;
    }

    g_pMenus = reinterpret_cast<IMenusApi*>(g_SMAPI->MetaFactory(Menus_INTERFACE, &ret, nullptr));
    if (ret == META_IFACE_FAILED || !g_pMenus)
    {
        ConColorMsg(Color(255, 0, 0, 255), "[VIP-FOVDetour] Menus API not found.\n");
        return;
    }

    g_pVIPCore = reinterpret_cast<IVIPApi*>(g_SMAPI->MetaFactory(VIP_INTERFACE, &ret, nullptr));
    if (ret == META_IFACE_FAILED || !g_pVIPCore)
    {
        ConColorMsg(Color(255, 0, 0, 255), "[VIP-FOVDetour] VIP Core not found.\n");
        return;
    }

    // Unique names: do not collide with any of the old experimental builds.
    g_pUtils->RegCommand(g_PLID, {"mm_fovhook"}, {"!fovhook"}, CommandFOVHook);
    g_pUtils->RegCommand(g_PLID, {"mm_fovhookdiag"}, {"!fovhookdiag"}, CommandFOVDiag);

    g_pUtils->StartupServer(g_PLID, OnStartupServer);

    g_pVIPCore->VIP_OnClientLoaded(VIP_OnClientLoaded_FOVWeapon);
    g_pVIPCore->VIP_OnClientDisconnect(VIP_OnClientDisconnect_FOVWeapon);
    g_pVIPCore->VIP_OnPlayerSpawn(VIP_OnPlayerSpawn_FOVWeapon);
    g_pVIPCore->VIP_RegisterFeature("FOV", VIP_STRING, SELECTABLE, OpenFOVMenu);

    ConColorMsg(Color(80, 220, 120, 255),
        "[VIP-FOVDetour] loaded. Test only: !fovhook 120 / !fovhookdiag / !fovhook off\n");
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
const char* VIPFovWeapon::GetVersion() { return "4.0-native-detour"; }
const char* VIPFovWeapon::GetDate() { return __DATE__; }
const char* VIPFovWeapon::GetLogTag() { return "[VIP-FOVDetour]"; }
const char* VIPFovWeapon::GetAuthor() { return "Pisex VIP_FOV adaptation + native CameraServices SetFOV detour"; }
const char* VIPFovWeapon::GetDescription() { return "VIP FOV that blocks native zero-FOV resets before they reach the client."; }
const char* VIPFovWeapon::GetName() { return "[VIP] FOV + Weapon Native Detour"; }
const char* VIPFovWeapon::GetURL() { return "https://github.com/Pisex/cs2-vip-modules"; }
