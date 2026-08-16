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
#endif

#include "vip_fovweapon.h"

VIPFovWeapon g_VIPFovWeapon;

IVIPApi* g_pVIPCore = nullptr;
IMenusApi* g_pMenus = nullptr;
IUtilsApi* g_pUtils = nullptr;

IVEngineServer2* engine = nullptr;
static ISchemaSystem* s_pSchemaSystem = nullptr;
CGameEntitySystem* g_pGameEntitySystem = nullptr;
CEntitySystem* g_pEntitySystem = nullptr;

std::vector<std::string> g_FOV[64];

PLUGIN_EXPOSE(VIPFovWeapon, g_VIPFovWeapon);

// -----------------------------------------------------------------------------
// Runtime schema offsets. No build-specific field offsets are hard-coded.
// -----------------------------------------------------------------------------
static int g_iDesiredFOVOffset = -1;          // diagnostics only; never written
static int g_iPlayerPawnHandleOffset = -1;
static int g_iCameraServicesOffset = -1;
static int g_iCameraFOVOffset = -1;
static int g_iCameraFOVStartOffset = -1;
static int g_iCameraFOVTimeOffset = -1;
static int g_iCameraFOVRateOffset = -1;
static int g_iCameraZoomOwnerOffset = -1;

#ifdef _WIN32
static constexpr const char* kServerModule = "server.dll";
#else
static constexpr const char* kServerModule = "libserver.so";
#endif

// The user-provided Linux libserver.so contains a native CameraServices SetFOV
// routine. We locate it by code shape + runtime schema offsets, NOT by a fixed RVA.
// Recovered SysV ABI for the supplied build:
//   bool SetFOV(cameraServices, ownerPawn, targetFOV, startFOV, rate)
using NativeSetFOVFn = bool (*)(void*, CEntityInstance*, int, int, float);
static NativeSetFOVFn g_pNativeSetFOV = nullptr;
static uintptr_t g_ServerBase = 0;
static uintptr_t g_NativeSetFOVRVA = 0;
static int g_NativeSetFOVMatches = 0;

// Per-player target. 0 = no override / let the game use normal FOV.
static std::array<int, 64> g_TargetFOV{};
static std::array<uint32_t, 64> g_BurstGeneration{};
static std::array<uint64_t, 64> g_AutoFixCount{};
static CTimer* g_pWatchdogTimer = nullptr;

// -----------------------------------------------------------------------------
// Schema helpers
// -----------------------------------------------------------------------------
static int FindServerOffset(const char* className, const char* fieldName)
{
    if (!s_pSchemaSystem || !className || !fieldName)
        return -1;

    CSchemaSystemTypeScope* scope =
        s_pSchemaSystem->FindTypeScopeForModule(kServerModule);

    if (!scope)
        return -1;

    SchemaClassInfoData_t* classInfo =
        scope->FindDeclaredClass(className).Get();

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
    g_iDesiredFOVOffset =
        FindServerOffset("CBasePlayerController", "m_iDesiredFOV");

    g_iPlayerPawnHandleOffset =
        FindServerOffset("CCSPlayerController", "m_hPlayerPawn");

    g_iCameraServicesOffset =
        FindServerOffset("CBasePlayerPawn", "m_pCameraServices");

    g_iCameraFOVOffset =
        FindServerOffset("CCSPlayerBase_CameraServices", "m_iFOV");
    g_iCameraFOVStartOffset =
        FindServerOffset("CCSPlayerBase_CameraServices", "m_iFOVStart");
    g_iCameraFOVTimeOffset =
        FindServerOffset("CCSPlayerBase_CameraServices", "m_flFOVTime");
    g_iCameraFOVRateOffset =
        FindServerOffset("CCSPlayerBase_CameraServices", "m_flFOVRate");
    g_iCameraZoomOwnerOffset =
        FindServerOffset("CCSPlayerBase_CameraServices", "m_hZoomOwner");

    ConColorMsg(
        Color(80, 220, 120, 255),
        "[VIP-FOVNative] schema: Pawn=0x%X CameraPtr=0x%X Camera=[FOV:0x%X Start:0x%X Time:0x%X Rate:0x%X Owner:0x%X] Desired(read-only)=0x%X\n",
        g_iPlayerPawnHandleOffset,
        g_iCameraServicesOffset,
        g_iCameraFOVOffset,
        g_iCameraFOVStartOffset,
        g_iCameraFOVTimeOffset,
        g_iCameraFOVRateOffset,
        g_iCameraZoomOwnerOffset,
        g_iDesiredFOVOffset
    );
}

// -----------------------------------------------------------------------------
// Entity helpers
// -----------------------------------------------------------------------------
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
            static_cast<uintptr_t>(g_iPlayerPawnHandleOffset)
        );

    return EntityFromHandle(pawnHandle);
}

static void* GetCameraServices(int slot)
{
    if (g_iCameraServicesOffset < 0)
        ResolveOffsets();

    CEntityInstance* pawn = GetPawn(slot);
    if (!pawn || g_iCameraServicesOffset < 0)
        return nullptr;

    return *reinterpret_cast<void**>(
        reinterpret_cast<uintptr_t>(pawn) +
        static_cast<uintptr_t>(g_iCameraServicesOffset)
    );
}

static int ReadIntAt(void* object, int offset)
{
    if (!object || offset < 0)
        return 0;
    return *reinterpret_cast<int*>(
        reinterpret_cast<uintptr_t>(object) + static_cast<uintptr_t>(offset)
    );
}

static float ReadFloatAt(void* object, int offset)
{
    if (!object || offset < 0)
        return 0.0f;
    return *reinterpret_cast<float*>(
        reinterpret_cast<uintptr_t>(object) + static_cast<uintptr_t>(offset)
    );
}

static int ReadCameraFOV(int slot)
{
    return ReadIntAt(GetCameraServices(slot), g_iCameraFOVOffset);
}

static int ReadCameraFOVStart(int slot)
{
    return ReadIntAt(GetCameraServices(slot), g_iCameraFOVStartOffset);
}

static float ReadCameraFOVRate(int slot)
{
    return ReadFloatAt(GetCameraServices(slot), g_iCameraFOVRateOffset);
}

static float ReadCameraFOVTime(int slot)
{
    return ReadFloatAt(GetCameraServices(slot), g_iCameraFOVTimeOffset);
}

static int ReadDesiredFOV(int slot)
{
    CEntityInstance* controller = GetController(slot);
    if (!controller || g_iDesiredFOVOffset < 0)
        return 0;
    return ReadIntAt(controller, g_iDesiredFOVOffset);
}

// -----------------------------------------------------------------------------
// Native libserver SetFOV resolver (Linux)
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
};

static bool EndsWithServerSo(const char* path)
{
    if (!path || !path[0])
        return false;

    const char* slash = std::strrchr(path, '/');
    const char* name = slash ? slash + 1 : path;
    return std::strcmp(name, "libserver.so") == 0;
}

static int FindServerModuleCallback(
    struct dl_phdr_info* info,
    size_t,
    void* opaque)
{
    if (!info || !opaque || !EndsWithServerSo(info->dlpi_name))
        return 0;

    auto* out = reinterpret_cast<ServerModuleScanInfo*>(opaque);
    out->base = static_cast<uintptr_t>(info->dlpi_addr);

    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i)
    {
        const ElfW(Phdr)& phdr = info->dlpi_phdr[i];
        if (phdr.p_type != PT_LOAD || (phdr.p_flags & PF_X) == 0 || phdr.p_memsz == 0)
            continue;

        const uintptr_t begin = out->base + static_cast<uintptr_t>(phdr.p_vaddr);
        out->executable.push_back({begin, begin + static_cast<uintptr_t>(phdr.p_memsz)});
    }

    return 1; // libserver found; stop iteration
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

    // Stable function prologue up through the first conditional branch.
    static constexpr uint8_t prefix[] = {
        0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56,
        0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC,
        0x58, 0xF3, 0x0F, 0x11, 0x45, 0x8C, 0x48, 0x85,
        0xF6, 0x0F, 0x84
    };

    if (std::memcmp(p, prefix, sizeof(prefix)) != 0)
        return false;

    // Branch displacement p[27..30] is intentionally ignored.
    static constexpr uint8_t middle[] = {
        0x49, 0x89, 0xFF,       // mov r15,rdi
        0x49, 0x89, 0xF6,       // mov r14,rsi
        0x89, 0xD3,             // mov ebx,edx
        0x41, 0x89, 0xCC,       // mov r12d,ecx
        0x3B, 0x97              // cmp edx,[rdi+disp32]
    };

    if (std::memcmp(p + 31, middle, sizeof(middle)) != 0)
        return false;

    if (g_iCameraFOVOffset < 0 ||
        ReadU32Unaligned(p + 44) != static_cast<uint32_t>(g_iCameraFOVOffset))
    {
        return false;
    }

    // Ignore second branch displacement, then validate zoom-owner access.
    if (p[48] != 0x0F || p[49] != 0x84 ||
        p[54] != 0x41 || p[55] != 0x8B || p[56] != 0x8F)
    {
        return false;
    }

    if (g_iCameraZoomOwnerOffset < 0 ||
        ReadU32Unaligned(p + 57) != static_cast<uint32_t>(g_iCameraZoomOwnerOffset))
    {
        return false;
    }

    return true;
}

static bool ResolveNativeSetFOV()
{
    g_pNativeSetFOV = nullptr;
    g_ServerBase = 0;
    g_NativeSetFOVRVA = 0;
    g_NativeSetFOVMatches = 0;

    if (g_iCameraFOVOffset < 0 || g_iCameraZoomOwnerOffset < 0)
        ResolveOffsets();

    if (g_iCameraFOVOffset < 0 || g_iCameraZoomOwnerOffset < 0)
        return false;

    ServerModuleScanInfo module;
    dl_iterate_phdr(FindServerModuleCallback, &module);

    if (module.base == 0 || module.executable.empty())
    {
        ConColorMsg(Color(255, 80, 80, 255),
            "[VIP-FOVNative] could not locate loaded libserver.so executable segment.\n");
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
            "[VIP-FOVNative] native SetFOV signature matches=%d; native path disabled.\n",
            g_NativeSetFOVMatches);
        return false;
    }

    g_NativeSetFOVRVA = onlyMatch - module.base;
    g_pNativeSetFOV = reinterpret_cast<NativeSetFOVFn>(onlyMatch);

    ConColorMsg(Color(80, 220, 120, 255),
        "[VIP-FOVNative] native CameraServices SetFOV found: libserver.so+0x%lX (1 unique match).\n",
        static_cast<unsigned long>(g_NativeSetFOVRVA));

    return true;
}
#else
static bool ResolveNativeSetFOV()
{
    ConColorMsg(Color(255, 150, 50, 255),
        "[VIP-FOVNative] native SetFOV scanner in this build is Linux-only.\n");
    return false;
}
#endif

// -----------------------------------------------------------------------------
// FOV application
// -----------------------------------------------------------------------------
static bool ApplyRawCameraFOVFallback(int slot, int value)
{
    // Fallback is intentionally the exact CameraServices-only mechanism from
    // the diagnostic build that the user confirmed visually worked. It does
    // NOT touch m_iDesiredFOV or any viewmodel field.
    CEntityInstance* pawn = GetPawn(slot);
    void* camera = GetCameraServices(slot);

    if (!pawn || !camera || g_iCameraFOVOffset < 0 || g_iCameraFOVStartOffset < 0)
        return false;

    *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(camera) + g_iCameraFOVOffset) = value;
    *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(camera) + g_iCameraFOVStartOffset) = value;

    if (g_iCameraFOVRateOffset >= 0)
    {
        *reinterpret_cast<float*>(
            reinterpret_cast<uintptr_t>(camera) + g_iCameraFOVRateOffset) = 0.0f;
    }

    if (g_pUtils)
    {
        g_pUtils->SetStateChanged(
            reinterpret_cast<CBaseEntity*>(pawn),
            "CBasePlayerPawn",
            "m_pCameraServices"
        );
    }

    return true;
}

static bool ApplyCameraFOVNow(int slot, int value)
{
    if (slot < 0 || slot >= 64 || value <= 0)
        return false;

    CEntityInstance* pawn = GetPawn(slot);
    void* camera = GetCameraServices(slot);
    if (!pawn || !camera)
        return false;

    if (g_pNativeSetFOV)
    {
        // startFOV=value deliberately reproduces the diagnostic camera=120/120
        // state that was observed to pull back both camera and weapon.
        return g_pNativeSetFOV(camera, pawn, value, value, 0.0f);
    }

    return ApplyRawCameraFOVFallback(slot, value);
}

static bool ResetCameraFOVNow(int slot)
{
    CEntityInstance* pawn = GetPawn(slot);
    void* camera = GetCameraServices(slot);
    if (!pawn || !camera)
        return false;

    if (g_pNativeSetFOV)
        return g_pNativeSetFOV(camera, pawn, 0, 0, 0.0f);

    return ApplyRawCameraFOVFallback(slot, 0);
}

static bool CameraNeedsRepair(int slot, int target)
{
    if (target <= 0)
        return false;

    return ReadCameraFOV(slot) != target || ReadCameraFOVStart(slot) != target;
}

static bool EnsureTargetFOV(int slot, bool countRepair)
{
    if (slot < 0 || slot >= 64)
        return false;

    const int target = g_TargetFOV[slot];
    if (target <= 0)
        return false;

    if (!CameraNeedsRepair(slot, target))
        return true;

    const bool ok = ApplyCameraFOVNow(slot, target);
    if (ok && countRepair)
        ++g_AutoFixCount[slot];
    return ok;
}

static void BurstFrame(int slot, uint32_t generation, int framesLeft)
{
    if (!g_pUtils || slot < 0 || slot >= 64 || framesLeft <= 0)
        return;

    if (generation != g_BurstGeneration[slot] || g_TargetFOV[slot] <= 0)
        return;

    EnsureTargetFOV(slot, true);

    g_pUtils->NextFrame([slot, generation, framesLeft]()
    {
        BurstFrame(slot, generation, framesLeft - 1);
    });
}

static void StartRepairBurst(int slot, int frames = 32)
{
    if (!g_pUtils || slot < 0 || slot >= 64 || g_TargetFOV[slot] <= 0)
        return;

    const uint32_t generation = ++g_BurstGeneration[slot];
    g_pUtils->NextFrame([slot, generation, frames]()
    {
        BurstFrame(slot, generation, frames);
    });
}

static float WatchdogTick()
{
    for (int slot = 0; slot < 64; ++slot)
    {
        if (g_TargetFOV[slot] <= 0)
            continue;

        EnsureTargetFOV(slot, true);
    }

    // Pisex Utils timers use the returned positive value as the next interval.
    return 0.05f;
}

static void StartWatchdog()
{
    if (!g_pUtils || g_pWatchdogTimer)
        return;

    g_pWatchdogTimer = g_pUtils->CreateTimer(0.05f, WatchdogTick);
}

static void StopWatchdog()
{
    if (!g_pUtils || !g_pWatchdogTimer)
        return;

    g_pUtils->RemoveTimer(g_pWatchdogTimer);
    g_pWatchdogTimer = nullptr;
}

static bool SetTargetFOV(int slot, int value, bool burst = true)
{
    if (slot < 0 || slot >= 64 || value <= 0)
        return false;

    g_TargetFOV[slot] = value;

    const bool ok = ApplyCameraFOVNow(slot, value);
    if (burst)
        StartRepairBurst(slot, 32);
    return ok;
}

static void ClearTargetFOV(int slot, bool resetGameFOV)
{
    if (slot < 0 || slot >= 64)
        return;

    g_TargetFOV[slot] = 0;
    ++g_BurstGeneration[slot];

    if (resetGameFOV)
        ResetCameraFOVNow(slot);
}

// -----------------------------------------------------------------------------
// Events / commands
// -----------------------------------------------------------------------------
static void OnWeaponFOVResetEvent(const char*, IGameEvent* event, bool)
{
    if (!event)
        return;

    const int slot = event->GetPlayerSlot("userid").Get();
    if (slot < 0 || slot >= 64 || g_TargetFOV[slot] <= 0)
        return;

    // Weapon deploy/reset can happen several frames after the event. The burst
    // checks each frame but calls SetFOV only if CS2 actually changed the value.
    StartRepairBurst(slot, 40);
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
    return last;
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

static bool CommandFOVCam(int slot, const char* content)
{
    if (!g_pUtils || slot < 0 || slot >= 64)
        return true;

    const std::string token = LastToken(content);
    if (token == "off" || token == "0")
    {
        ClearTargetFOV(slot, true);
        g_pUtils->PrintToChat(slot, "[FOV] override OFF; game FOV restored");
        return true;
    }

    const int value = std::strtol(token.c_str(), nullptr, 10);
    if (value < 60 || value > 179)
    {
        g_pUtils->PrintToChat(slot, "[FOV] Usage: !fovcam 120 (60..179) or !fovcam off");
        return true;
    }

    if (!SetTargetFOV(slot, value, true))
    {
        g_pUtils->PrintToChat(slot, "[FOV] SetFOV failed. Check server console / !fovdiag");
        return true;
    }

    g_pUtils->PrintToChat(
        slot,
        "[FOV] target=%d camera=%d/%d native=%s",
        value,
        ReadCameraFOV(slot),
        ReadCameraFOVStart(slot),
        g_pNativeSetFOV ? "yes" : "fallback"
    );
    return true;
}

static bool CommandFOVDiag(int slot, const char*)
{
    if (!g_pUtils || slot < 0 || slot >= 64)
        return true;

    void* camera = GetCameraServices(slot);

    g_pUtils->PrintToChat(
        slot,
        "[FOV] target=%d cam=%d/%d rate=%.3f time=%.3f fixes=%llu",
        g_TargetFOV[slot],
        ReadCameraFOV(slot),
        ReadCameraFOVStart(slot),
        ReadCameraFOVRate(slot),
        ReadCameraFOVTime(slot),
        static_cast<unsigned long long>(g_AutoFixCount[slot])
    );

    g_pUtils->PrintToChat(
        slot,
        "[FOV] native=%s matches=%d rva=0x%lX desired(read-only)=%d",
        g_pNativeSetFOV ? "YES" : "NO/fallback",
        g_NativeSetFOVMatches,
        static_cast<unsigned long>(g_NativeSetFOVRVA),
        ReadDesiredFOV(slot)
    );

    g_pUtils->PrintToChat(
        slot,
        "[FOV] cameraPtr=%s schema camPtr=0x%X FOV=0x%X Start=0x%X Owner=0x%X",
        camera ? "OK" : "NULL",
        g_iCameraServicesOffset,
        g_iCameraFOVOffset,
        g_iCameraFOVStartOffset,
        g_iCameraZoomOwnerOffset
    );

    return true;
}

// -----------------------------------------------------------------------------
// Metamod / VIP lifecycle
// -----------------------------------------------------------------------------
bool VIPFovWeapon::Load(
    PluginId id,
    ISmmAPI* ismm,
    char* error,
    size_t maxlen,
    bool late)
{
    PLUGIN_SAVEVARS();

    GET_V_IFACE_ANY(
        GetEngineFactory,
        s_pSchemaSystem,
        ISchemaSystem,
        SCHEMASYSTEM_INTERFACE_VERSION
    );

    GET_V_IFACE_CURRENT(
        GetEngineFactory,
        engine,
        IVEngineServer2,
        SOURCE2ENGINETOSERVER_INTERFACE_VERSION
    );

    g_SMAPI->AddListener(this, this);
    return true;
}

bool VIPFovWeapon::Unload(char* error, size_t maxlen)
{
    StopWatchdog();

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
    ResolveNativeSetFOV();
    StartWatchdog();
}

static void VIP_OnClientLoaded_FOVWeapon(int slot, bool isVIP)
{
    if (slot < 0 || slot >= 64)
        return;

    g_FOV[slot].clear();
    g_TargetFOV[slot] = 0;
    g_AutoFixCount[slot] = 0;
    ++g_BurstGeneration[slot];

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
    g_AutoFixCount[slot] = 0;
    ++g_BurstGeneration[slot];
}

static void VIP_OnPlayerSpawn_FOVWeapon(int slot, int, bool isVIP)
{
    if (slot < 0 || slot >= 64 || !isVIP || !g_pVIPCore || !g_pUtils)
        return;

    if (g_FOV[slot].empty())
        return;

    const char* cookie = g_pVIPCore->VIP_GetClientCookie(slot, "FOV_Value");
    const int selected = (cookie && cookie[0]) ? std::strtol(cookie, nullptr, 10) : 90;

    if (selected < 60 || selected > 179)
        return;

    g_TargetFOV[slot] = selected;

    // New pawn/services may not exist until the following frame. The burst is
    // deliberately long enough to cover spawn + initial weapon deploy resets.
    StartRepairBurst(slot, 64);
}

static bool OpenFOVMenu(int slot, const char*)
{
    if (slot < 0 || slot >= 64 || !g_pMenus || !g_pVIPCore)
        return false;

    Menu menu;

    const char* translated = g_pVIPCore->VIP_GetTranslate("FOV_Title");
    g_pMenus->SetTitleMenu(
        menu,
        (translated && translated[0]) ? translated : "Select FOV"
    );

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
                if (value >= 60 && value <= 179)
                {
                    SetTargetFOV(slot, value, true);
                    g_pVIPCore->VIP_SetClientCookie(slot, "FOV_Value", strdup(back));
                }

                OpenFOVMenu(slot, "FOV");
            }
            else
            {
                g_pVIPCore->VIP_OpenMenu(slot);
            }
        }
    );

    g_pMenus->DisplayPlayerMenu(menu, slot);
    return false;
}

void VIPFovWeapon::AllPluginsLoaded()
{
    int ret = 0;

    g_pUtils = reinterpret_cast<IUtilsApi*>(
        g_SMAPI->MetaFactory(Utils_INTERFACE, &ret, nullptr));
    if (ret == META_IFACE_FAILED || !g_pUtils)
    {
        ConColorMsg(Color(255, 0, 0, 255), "[VIP-FOVNative] Utils API not found.\n");
        return;
    }

    g_pMenus = reinterpret_cast<IMenusApi*>(
        g_SMAPI->MetaFactory(Menus_INTERFACE, &ret, nullptr));
    if (ret == META_IFACE_FAILED || !g_pMenus)
    {
        ConColorMsg(Color(255, 0, 0, 255), "[VIP-FOVNative] Menus API not found.\n");
        return;
    }

    g_pVIPCore = reinterpret_cast<IVIPApi*>(
        g_SMAPI->MetaFactory(VIP_INTERFACE, &ret, nullptr));
    if (ret == META_IFACE_FAILED || !g_pVIPCore)
    {
        ConColorMsg(Color(255, 0, 0, 255), "[VIP-FOVNative] VIP Core not found.\n");
        return;
    }

    // Keep the one debug command the user already verified, but it now uses
    // the native SetFOV path and becomes persistent.
    g_pUtils->RegCommand(g_PLID, {"mm_fovcam"}, {"!fovcam", "!realfov"}, CommandFOVCam);
    g_pUtils->RegCommand(g_PLID, {"mm_fovdiag"}, {"!fovdiag", "!fovinfo"}, CommandFOVDiag);

    g_pUtils->StartupServer(g_PLID, OnStartupServer);
    g_pUtils->HookEvent(g_PLID, "weapon_switch", OnWeaponFOVResetEvent);
    g_pUtils->HookEvent(g_PLID, "item_equip", OnWeaponFOVResetEvent);

    g_pVIPCore->VIP_OnClientLoaded(VIP_OnClientLoaded_FOVWeapon);
    g_pVIPCore->VIP_OnClientDisconnect(VIP_OnClientDisconnect_FOVWeapon);
    g_pVIPCore->VIP_OnPlayerSpawn(VIP_OnPlayerSpawn_FOVWeapon);

    g_pVIPCore->VIP_RegisterFeature("FOV", VIP_STRING, SELECTABLE, OpenFOVMenu);

    ConColorMsg(
        Color(80, 220, 120, 255),
        "[VIP-FOVNative] loaded. CameraServices-only native FOV; no desiredFOV/viewmodel/client-command writes. Commands: !fovcam !fovdiag\n"
    );
}

const char* VIPFovWeapon::GetLicense() { return "Public"; }
const char* VIPFovWeapon::GetVersion() { return "2.0-native"; }
const char* VIPFovWeapon::GetDate() { return __DATE__; }
const char* VIPFovWeapon::GetLogTag() { return "[VIP-FOVNative]"; }
const char* VIPFovWeapon::GetAuthor() { return "Pisex VIP_FOV adaptation + native CameraServices SetFOV"; }
const char* VIPFovWeapon::GetDescription() { return "Persistent VIP FOV through native libserver CameraServices SetFOV."; }
const char* VIPFovWeapon::GetName() { return "[VIP] Native FOV + Weapon"; }
const char* VIPFovWeapon::GetURL() { return "https://github.com/Pisex/cs2-vip-modules"; }
