#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

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

struct PlayerFOVState
{
    int worldFOV = 0;
    float viewX = 2.50f;
    float viewY = 2.00f;
    float viewZ = -2.00f;
    bool enabled = false;
    bool customOffsets = false;
};

// One authoritative state per player. Both VIP menu changes and debug
// commands update this same state, so an old VIP cookie cannot fight with a
// newer !fovcam/!fovweapon value during weapon switches or respawns.
static PlayerFOVState g_PlayerFOV[64];

PLUGIN_EXPOSE(VIPFovWeapon, g_VIPFovWeapon);

// Runtime schema offsets. Nothing here is hard-coded to a particular CS2 build.
static int g_iDesiredFOVOffset = -1;
static int g_iPlayerPawnHandleOffset = -1;
static int g_iViewmodelFOVOffset = -1;
static int g_iViewmodelOffsetXOffset = -1;
static int g_iViewmodelOffsetYOffset = -1;
static int g_iViewmodelOffsetZOffset = -1;
static int g_iCameraServicesOffset = -1;
static int g_iCameraFOVOffset = -1;
static int g_iCameraFOVStartOffset = -1;
static int g_iCameraFOVRateOffset = -1;

// The client ignores the networked m_flViewmodelFOV for the local weapon on
// current CS2 builds, but it DOES honor m_flViewmodelOffsetX/Y/Z. The default
// CS2 "far" preset is 2.5 / 2.0 / -2.0. For FOV above 90 we push Y beyond the
// normal client cvar clamp, which moves the viewmodel farther away and keeps
// it visually proportional to the wider world FOV.
static constexpr float kBaseWorldFOV = 90.0f;
static constexpr float kBaseViewX = 2.50f;
static constexpr float kBaseViewY = 2.00f;
static constexpr float kBaseViewZ = -2.00f;

#ifdef _WIN32
static constexpr const char* kServerModule = "server.dll";
#else
static constexpr const char* kServerModule = "libserver.so";
#endif

// Resolve a field directly from a declared server-side schema class.
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
    // This is the same field used by Pisex VIP_FOV.
    g_iDesiredFOVOffset =
        FindServerOffset("CBasePlayerController", "m_iDesiredFOV");

    g_iPlayerPawnHandleOffset =
        FindServerOffset("CCSPlayerController", "m_hPlayerPawn");

    // This field was verified in the supplied Linux libserver.so and is a
    // server-side networked field on CCSPlayerPawn.
    g_iViewmodelFOVOffset =
        FindServerOffset("CCSPlayerPawn", "m_flViewmodelFOV");
    g_iViewmodelOffsetXOffset =
        FindServerOffset("CCSPlayerPawn", "m_flViewmodelOffsetX");
    g_iViewmodelOffsetYOffset =
        FindServerOffset("CCSPlayerPawn", "m_flViewmodelOffsetY");
    g_iViewmodelOffsetZOffset =
        FindServerOffset("CCSPlayerPawn", "m_flViewmodelOffsetZ");

    // Camera services live on the CBasePlayerPawn base class; the concrete
    // Counter-Strike service object is CCSPlayerBase_CameraServices.
    g_iCameraServicesOffset =
        FindServerOffset("CBasePlayerPawn", "m_pCameraServices");
    g_iCameraFOVOffset =
        FindServerOffset("CCSPlayerBase_CameraServices", "m_iFOV");
    g_iCameraFOVStartOffset =
        FindServerOffset("CCSPlayerBase_CameraServices", "m_iFOVStart");
    g_iCameraFOVRateOffset =
        FindServerOffset("CCSPlayerBase_CameraServices", "m_flFOVRate");

    ConColorMsg(
        Color(80, 220, 120, 255),
        "[VIP-FOVWeapon] offsets: Desired=0x%X Pawn=0x%X VMFOV=0x%X VMXYZ=[0x%X 0x%X 0x%X] CameraPtr=0x%X Camera=[FOV:0x%X Start:0x%X Rate:0x%X]\n",
        g_iDesiredFOVOffset,
        g_iPlayerPawnHandleOffset,
        g_iViewmodelFOVOffset,
        g_iViewmodelOffsetXOffset,
        g_iViewmodelOffsetYOffset,
        g_iViewmodelOffsetZOffset,
        g_iCameraServicesOffset,
        g_iCameraFOVOffset,
        g_iCameraFOVStartOffset,
        g_iCameraFOVRateOffset
    );
}

// Avoid depending on generated CCSPlayerController/CCSPlayerPawn accessors.
// The public entity list layout is enough for this module.
static CEntityInstance* EntityFromIndex(int index)
{
    if (!g_pEntitySystem || index < 0 || index >= MAX_TOTAL_ENTITIES - 1)
        return nullptr;

    CEntityIdentity* chunk =
        g_pEntitySystem->m_EntityList.m_pIdentityChunks[index / MAX_ENTITIES_IN_LIST];

    if (!chunk)
        return nullptr;

    CEntityIdentity* identity =
        &chunk[index % MAX_ENTITIES_IN_LIST];

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

    CEntityIdentity* identity =
        &chunk[index % MAX_ENTITIES_IN_LIST];

    if (identity->GetRefEHandle() != handle)
        return nullptr;

    return identity->m_pInstance;
}

static CEntityInstance* GetController(int slot)
{
    // Source 2 player controllers occupy entity indexes slot + 1.
    return EntityFromIndex(slot + 1);
}

static CEntityInstance* GetPawn(int slot)
{
    if (g_iPlayerPawnHandleOffset < 0)
        ResolveOffsets();

    if (g_iPlayerPawnHandleOffset < 0)
        return nullptr;

    CEntityInstance* controller = GetController(slot);
    if (!controller)
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

static bool SetCameraFOV(int slot, int value)
{
    if (value <= 0)
        return false;

    if (g_iCameraFOVOffset < 0 ||
        g_iCameraFOVStartOffset < 0 ||
        g_iCameraServicesOffset < 0)
    {
        ResolveOffsets();
    }

    CEntityInstance* pawn = GetPawn(slot);
    void* camera = GetCameraServices(slot);
    if (!pawn || !camera || g_iCameraFOVOffset < 0 || g_iCameraFOVStartOffset < 0)
        return false;

    *reinterpret_cast<uint32_t*>(
        reinterpret_cast<uintptr_t>(camera) + static_cast<uintptr_t>(g_iCameraFOVOffset)
    ) = static_cast<uint32_t>(value);

    *reinterpret_cast<uint32_t*>(
        reinterpret_cast<uintptr_t>(camera) + static_cast<uintptr_t>(g_iCameraFOVStartOffset)
    ) = static_cast<uint32_t>(value);

    if (g_iCameraFOVRateOffset >= 0)
    {
        *reinterpret_cast<float*>(
            reinterpret_cast<uintptr_t>(camera) + static_cast<uintptr_t>(g_iCameraFOVRateOffset)
        ) = 0.0f;
    }

    // Camera services are networked through the pawn. Mark the owning
    // networked service pointer dirty so the service state is serialized.
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

static int ReadCameraFOV(int slot)
{
    if (g_iCameraFOVOffset < 0)
        ResolveOffsets();

    void* camera = GetCameraServices(slot);
    if (!camera || g_iCameraFOVOffset < 0)
        return 0;

    return static_cast<int>(*reinterpret_cast<uint32_t*>(
        reinterpret_cast<uintptr_t>(camera) + static_cast<uintptr_t>(g_iCameraFOVOffset)
    ));
}

static int ReadCameraFOVStart(int slot)
{
    if (g_iCameraFOVStartOffset < 0)
        ResolveOffsets();

    void* camera = GetCameraServices(slot);
    if (!camera || g_iCameraFOVStartOffset < 0)
        return 0;

    return static_cast<int>(*reinterpret_cast<uint32_t*>(
        reinterpret_cast<uintptr_t>(camera) + static_cast<uintptr_t>(g_iCameraFOVStartOffset)
    ));
}

static float ReadPawnFloat(int slot, int offset)
{
    CEntityInstance* pawn = GetPawn(slot);
    if (!pawn || offset < 0)
        return 0.0f;

    return *reinterpret_cast<float*>(
        reinterpret_cast<uintptr_t>(pawn) + static_cast<uintptr_t>(offset)
    );
}

static bool SetViewmodelOffsets(int slot, float x, float y, float z)
{
    if (g_iViewmodelOffsetXOffset < 0 ||
        g_iViewmodelOffsetYOffset < 0 ||
        g_iViewmodelOffsetZOffset < 0)
    {
        ResolveOffsets();
    }

    CEntityInstance* pawn = GetPawn(slot);
    if (!pawn || !g_pUtils ||
        g_iViewmodelOffsetXOffset < 0 ||
        g_iViewmodelOffsetYOffset < 0 ||
        g_iViewmodelOffsetZOffset < 0)
    {
        return false;
    }

    *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(pawn) + g_iViewmodelOffsetXOffset) = x;
    *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(pawn) + g_iViewmodelOffsetYOffset) = y;
    *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(pawn) + g_iViewmodelOffsetZOffset) = z;

    g_pUtils->SetStateChanged(reinterpret_cast<CBaseEntity*>(pawn), "CCSPlayerPawn", "m_flViewmodelOffsetX");
    g_pUtils->SetStateChanged(reinterpret_cast<CBaseEntity*>(pawn), "CCSPlayerPawn", "m_flViewmodelOffsetY");
    g_pUtils->SetStateChanged(reinterpret_cast<CBaseEntity*>(pawn), "CCSPlayerPawn", "m_flViewmodelOffsetZ");
    return true;
}

static bool SetWorldFOV(int slot, int value)
{
    if (g_iDesiredFOVOffset < 0)
        ResolveOffsets();

    if (g_iDesiredFOVOffset < 0 || !g_pUtils)
        return false;

    CEntityInstance* controller = GetController(slot);
    if (!controller)
        return false;

    *reinterpret_cast<int*>(
        reinterpret_cast<uintptr_t>(controller) +
        static_cast<uintptr_t>(g_iDesiredFOVOffset)
    ) = value;

    g_pUtils->SetStateChanged(
        reinterpret_cast<CBaseEntity*>(controller),
        "CBasePlayerController",
        "m_iDesiredFOV"
    );

    return true;
}

static bool SetViewmodelFOV(int slot, float value)
{
    if (g_iViewmodelFOVOffset < 0)
        ResolveOffsets();

    if (g_iViewmodelFOVOffset < 0 || !g_pUtils)
        return false;

    CEntityInstance* pawn = GetPawn(slot);
    if (!pawn)
        return false;

    *reinterpret_cast<float*>(
        reinterpret_cast<uintptr_t>(pawn) +
        static_cast<uintptr_t>(g_iViewmodelFOVOffset)
    ) = value;

    g_pUtils->SetStateChanged(
        reinterpret_cast<CBaseEntity*>(pawn),
        "CCSPlayerPawn",
        "m_flViewmodelFOV"
    );

    return true;
}

static int ReadWorldFOV(int slot)
{
    if (g_iDesiredFOVOffset < 0)
        ResolveOffsets();

    CEntityInstance* controller = GetController(slot);
    if (!controller || g_iDesiredFOVOffset < 0)
        return 0;

    return *reinterpret_cast<int*>(
        reinterpret_cast<uintptr_t>(controller) +
        static_cast<uintptr_t>(g_iDesiredFOVOffset)
    );
}

static float ReadViewmodelFOV(int slot)
{
    if (g_iViewmodelFOVOffset < 0)
        ResolveOffsets();

    CEntityInstance* pawn = GetPawn(slot);
    if (!pawn || g_iViewmodelFOVOffset < 0)
        return 0.0f;

    return *reinterpret_cast<float*>(
        reinterpret_cast<uintptr_t>(pawn) +
        static_cast<uintptr_t>(g_iViewmodelFOVOffset)
    );
}

static void AutoOffsetsForFOV(int worldFOV, float& x, float& y, float& z)
{
    const float extra = std::max(0.0f, static_cast<float>(worldFOV) - kBaseWorldFOV);

    // Tuned as a practical server-side replacement for client viewmodel FOV.
    //  90 -> 2.50 / 2.00 / -2.00
    // 120 -> 2.50 / 5.60 / -2.60
    // 150 -> 2.50 / 9.20 / -3.20
    x = kBaseViewX;
    y = kBaseViewY + extra * 0.12f;
    z = kBaseViewZ - extra * 0.02f;
}

static void SaveFOVCookieIfVIP(int slot, int worldFOV)
{
    if (!g_pVIPCore || slot < 0 || slot >= 64 || worldFOV <= 0)
        return;

    if (!g_pVIPCore->VIP_IsClientVIP(slot))
        return;

    const std::string value = std::to_string(worldFOV);
    g_pVIPCore->VIP_SetClientCookie(slot, "FOV_Value", value.c_str());
}

static bool ApplyPlayerFOVState(int slot)
{
    if (slot < 0 || slot >= 64)
        return false;

    PlayerFOVState& state = g_PlayerFOV[slot];
    if (!state.enabled || state.worldFOV <= 0)
        return false;

    if (!state.customOffsets)
        AutoOffsetsForFOV(state.worldFOV, state.viewX, state.viewY, state.viewZ);

    const bool desiredOk = SetWorldFOV(slot, state.worldFOV);
    const bool cameraOk = SetCameraFOV(slot, state.worldFOV);
    const bool offsetsOk = SetViewmodelOffsets(slot, state.viewX, state.viewY, state.viewZ);

    if (!desiredOk || !cameraOk || !offsetsOk)
    {
        ConColorMsg(
            Color(255, 180, 50, 255),
            "[VIP-FOVWeapon] apply failed slot %d: desired=%d camera=%d offsets=%d FOV=%d XYZ=%.2f/%.2f/%.2f\n",
            slot,
            desiredOk ? 1 : 0,
            cameraOk ? 1 : 0,
            offsetsOk ? 1 : 0,
            state.worldFOV,
            state.viewX,
            state.viewY,
            state.viewZ
        );
    }

    return desiredOk && cameraOk && offsetsOk;
}

static bool SetPlayerFOVTarget(int slot, int worldFOV, bool saveCookie)
{
    if (slot < 0 || slot >= 64 || worldFOV <= 0)
        return false;

    PlayerFOVState& state = g_PlayerFOV[slot];
    state.worldFOV = worldFOV;
    state.enabled = true;

    if (!state.customOffsets)
        AutoOffsetsForFOV(worldFOV, state.viewX, state.viewY, state.viewZ);

    if (saveCookie)
        SaveFOVCookieIfVIP(slot, worldFOV);

    return ApplyPlayerFOVState(slot);
}

static void ReapplyBurstFrame(int slot, int remaining)
{
    if (!g_pUtils || slot < 0 || slot >= 64 || remaining <= 0)
        return;

    g_pUtils->NextFrame([slot, remaining]()
    {
        if (slot < 0 || slot >= 64 || !g_PlayerFOV[slot].enabled)
            return;

        ApplyPlayerFOVState(slot);

        if (remaining > 1)
            ReapplyBurstFrame(slot, remaining - 1);
    });
}

static void ReapplyPlayerFOV(int slot)
{
    if (slot < 0 || slot >= 64 || !g_PlayerFOV[slot].enabled)
        return;

    // Weapon changes can rewrite camera/viewmodel state over several frames.
    // A short 6-frame burst is still event-driven (not every tick forever),
    // while being much harder for the engine's weapon setup to overwrite.
    ApplyPlayerFOVState(slot);
    ReapplyBurstFrame(slot, 6);
}

static void OnWeaponFOVResetEvent(const char*, IGameEvent* event, bool)
{
    if (!event)
        return;

    const int slot = event->GetPlayerSlot("userid").Get();
    if (slot < 0 || slot >= 64 || !g_PlayerFOV[slot].enabled)
        return;

    ReapplyPlayerFOV(slot);
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
        // Trim basic spaces around comma-separated values.
        const std::size_t first = token.find_first_not_of(" \t\r\n");
        const std::size_t last = token.find_last_not_of(" \t\r\n");

        if (first != std::string::npos)
            output.push_back(token.substr(first, last - first + 1));
    }
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

static std::vector<std::string> Tokens(const char* text)
{
    std::vector<std::string> out;
    if (!text)
        return out;

    std::stringstream ss(text);
    std::string token;
    while (ss >> token)
        out.push_back(token);
    return out;
}

// Optional debug/test command: !fovweapon 110
static bool CommandFOVWeapon(int slot, const char* content)
{
    // Debug command intentionally bypasses VIP access checks.
    // The actual selectable VIP feature/menu remains VIP-only via VIP Core.
    if (!g_pUtils)
        return true;

    const std::string token = LastToken(content);

    if (token.empty() || token == "mm_fovweapon" || token == "!fovweapon")
    {
        g_pUtils->PrintToChat(slot, "[FOV] Usage: !fovweapon 110");
        return true;
    }

    const int value = std::strtol(token.c_str(), nullptr, 10);
    if (value <= 0)
    {
        g_pUtils->PrintToChat(slot, "[FOV] Invalid value.");
        return true;
    }

    if (!SetPlayerFOVTarget(slot, value, true))
    {
        g_pUtils->PrintToChat(slot, "[FOV] Failed. Check server console offsets.");
        return true;
    }

    ReapplyPlayerFOV(slot);
    g_pUtils->PrintToChat(
        slot,
        "[FOV] world=%d camera=%d/%d weaponXYZ=%.2f %.2f %.2f",
        ReadWorldFOV(slot),
        ReadCameraFOV(slot),
        ReadCameraFOVStart(slot),
        g_PlayerFOV[slot].viewX,
        g_PlayerFOV[slot].viewY,
        g_PlayerFOV[slot].viewZ
    );
    return true;
}

static bool CommandFOVCam(int slot, const char* content)
{
    if (!g_pUtils)
        return true;

    const std::string token = LastToken(content);

    if (token == "off" || token == "0")
    {
        if (slot >= 0 && slot < 64)
            g_PlayerFOV[slot] = PlayerFOVState{};
        g_pUtils->PrintToChat(slot, "[FOV] persistent FOV/weapon reapply disabled");
        return true;
    }

    const int value = std::strtol(token.c_str(), nullptr, 10);

    if (value <= 0)
    {
        g_pUtils->PrintToChat(slot, "[FOV] Usage: !fovcam 120 (or !fovcam off)");
        return true;
    }

    // Use the exact same state/cookie path as the VIP menu. This prevents an
    // old saved VIP value (for example 150) from restoring over !fovcam 120.
    if (!SetPlayerFOVTarget(slot, value, true))
    {
        g_pUtils->PrintToChat(slot, "[FOV] CameraServices write failed. See server console.");
        return true;
    }

    ReapplyPlayerFOV(slot);
    g_pUtils->PrintToChat(
        slot,
        "[FOV] camera=%d/%d world=%d weaponXYZ=%.2f %.2f %.2f",
        ReadCameraFOV(slot),
        ReadCameraFOVStart(slot),
        ReadWorldFOV(slot),
        g_PlayerFOV[slot].viewX,
        g_PlayerFOV[slot].viewY,
        g_PlayerFOV[slot].viewZ
    );
    return true;
}

static bool CommandFOVOffset(int slot, const char* content)
{
    if (!g_pUtils)
        return true;

    const auto tokens = Tokens(content);
    if (tokens.size() < 3)
    {
        g_pUtils->PrintToChat(slot, "[FOV] Usage: !fovoffset 10 0 0");
        return true;
    }

    // Arguments are always the final 3 whitespace-separated tokens.
    const float x = std::strtof(tokens[tokens.size() - 3].c_str(), nullptr);
    const float y = std::strtof(tokens[tokens.size() - 2].c_str(), nullptr);
    const float z = std::strtof(tokens[tokens.size() - 1].c_str(), nullptr);

    if (slot >= 0 && slot < 64)
    {
        g_PlayerFOV[slot].viewX = x;
        g_PlayerFOV[slot].viewY = y;
        g_PlayerFOV[slot].viewZ = z;
        g_PlayerFOV[slot].customOffsets = true;
    }

    if (!SetViewmodelOffsets(slot, x, y, z))
    {
        g_pUtils->PrintToChat(slot, "[FOV] Viewmodel offset write failed. See server console.");
        return true;
    }

    // Keep a custom XYZ alive across weapon switches too.
    if (slot >= 0 && slot < 64 && g_PlayerFOV[slot].worldFOV > 0)
        ReapplyPlayerFOV(slot);

    g_pUtils->PrintToChat(
        slot,
        "[FOV] offsets X=%.2f Y=%.2f Z=%.2f",
        ReadPawnFloat(slot, g_iViewmodelOffsetXOffset),
        ReadPawnFloat(slot, g_iViewmodelOffsetYOffset),
        ReadPawnFloat(slot, g_iViewmodelOffsetZOffset)
    );
    return true;
}

static bool CommandFOVAuto(int slot, const char*)
{
    if (!g_pUtils || slot < 0 || slot >= 64)
        return true;

    PlayerFOVState& state = g_PlayerFOV[slot];
    int fov = state.worldFOV > 0 ? state.worldFOV : ReadWorldFOV(slot);
    if (fov <= 0)
        fov = 90;

    state.worldFOV = fov;
    state.enabled = true;
    state.customOffsets = false;
    AutoOffsetsForFOV(fov, state.viewX, state.viewY, state.viewZ);
    ReapplyPlayerFOV(slot);

    g_pUtils->PrintToChat(
        slot,
        "[FOV] automatic weapon distance enabled: %.2f %.2f %.2f",
        state.viewX,
        state.viewY,
        state.viewZ
    );
    return true;
}

static bool CommandFOVDiag(int slot, const char*)
{
    if (!g_pUtils)
        return true;

    ResolveOffsets();

    g_pUtils->PrintToChat(
        slot,
        "[FOV] world=%d vmFOV=%.2f cam=%d/%d",
        ReadWorldFOV(slot),
        ReadViewmodelFOV(slot),
        ReadCameraFOV(slot),
        ReadCameraFOVStart(slot)
    );

    g_pUtils->PrintToChat(
        slot,
        "[FOV] vmXYZ=%.2f %.2f %.2f target=%d auto=%s",
        ReadPawnFloat(slot, g_iViewmodelOffsetXOffset),
        ReadPawnFloat(slot, g_iViewmodelOffsetYOffset),
        ReadPawnFloat(slot, g_iViewmodelOffsetZOffset),
        (slot >= 0 && slot < 64) ? g_PlayerFOV[slot].worldFOV : 0,
        (slot >= 0 && slot < 64 && !g_PlayerFOV[slot].customOffsets) ? "yes" : "no"
    );

    g_pUtils->PrintToChat(
        slot,
        "[FOV] schema desired=0x%X vm=0x%X camPtr=0x%X camFOV=0x%X",
        g_iDesiredFOVOffset,
        g_iViewmodelFOVOffset,
        g_iCameraServicesOffset,
        g_iCameraFOVOffset
    );

    return true;
}

static bool CommandFOVInfo(int slot, const char*)
{
    if (g_iDesiredFOVOffset < 0 ||
        g_iPlayerPawnHandleOffset < 0 ||
        g_iViewmodelFOVOffset < 0)
    {
        ResolveOffsets();
    }

    g_pUtils->PrintToChat(
        slot,
        "[FOV] world=%d viewmodel=%.2f offsets=[desired:0x%X pawn:0x%X vm:0x%X]",
        ReadWorldFOV(slot),
        ReadViewmodelFOV(slot),
        g_iDesiredFOVOffset,
        g_iPlayerPawnHandleOffset,
        g_iViewmodelFOVOffset
    );

    return true;
}

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
    if (g_pUtils)
        g_pUtils->ClearAllHooks(g_PLID);

    return true;
}

static void OnStartupServer()
{
    g_pGameEntitySystem = g_pUtils->GetCGameEntitySystem();
    g_pEntitySystem = g_pUtils->GetCEntitySystem();
    ResolveOffsets();
}

static void VIP_OnClientLoaded_FOVWeapon(int slot, bool isVIP)
{
    if (slot < 0 || slot >= 64)
        return;

    g_FOV[slot].clear();
    g_PlayerFOV[slot] = PlayerFOVState{};

    if (!isVIP || !g_pVIPCore)
        return;

    SplitList(
        g_pVIPCore->VIP_GetClientFeatureString(slot, "FOV"),
        g_FOV[slot]
    );
}

static void VIP_OnPlayerSpawn_FOVWeapon(int slot, int team, bool isVIP)
{
    if (slot < 0 || slot >= 64 || !isVIP || !g_pVIPCore || !g_pUtils)
        return;

    if (g_FOV[slot].empty())
        return;

    const char* cookie =
        g_pVIPCore->VIP_GetClientCookie(slot, "FOV_Value");

    const int worldFOV =
        (cookie && cookie[0]) ? std::strtol(cookie, nullptr, 10) : 90;

    // Load the saved VIP value into the same authoritative state used by
    // commands/menu, then re-apply after the new pawn exists.
    PlayerFOVState& state = g_PlayerFOV[slot];
    state.worldFOV = worldFOV;
    state.enabled = true;
    state.customOffsets = false;
    AutoOffsetsForFOV(worldFOV, state.viewX, state.viewY, state.viewZ);

    g_pUtils->NextFrame([slot]()
    {
        ReapplyPlayerFOV(slot);
    });
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
                const int worldFOV = std::strtol(back, nullptr, 10);

                if (worldFOV > 0)
                {
                    // Menu selection intentionally resets manual XYZ tuning
                    // back to automatic weapon distance for the chosen FOV.
                    g_PlayerFOV[slot].customOffsets = false;
                    SetPlayerFOVTarget(slot, worldFOV, true);
                    ReapplyPlayerFOV(slot);
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
        g_SMAPI->MetaFactory(Utils_INTERFACE, &ret, nullptr)
    );

    if (ret == META_IFACE_FAILED || !g_pUtils)
    {
        ConColorMsg(
            Color(255, 0, 0, 255),
            "[VIP-FOVWeapon] Utils API not found.\n"
        );
        return;
    }

    g_pMenus = reinterpret_cast<IMenusApi*>(
        g_SMAPI->MetaFactory(Menus_INTERFACE, &ret, nullptr)
    );

    if (ret == META_IFACE_FAILED || !g_pMenus)
    {
        ConColorMsg(
            Color(255, 0, 0, 255),
            "[VIP-FOVWeapon] Menus API not found.\n"
        );
        return;
    }

    g_pVIPCore = reinterpret_cast<IVIPApi*>(
        g_SMAPI->MetaFactory(VIP_INTERFACE, &ret, nullptr)
    );

    if (ret == META_IFACE_FAILED || !g_pVIPCore)
    {
        ConColorMsg(
            Color(255, 0, 0, 255),
            "[VIP-FOVWeapon] VIP Core not found.\n"
        );
        return;
    }

    // Debug commands are useful for checking whether the current CS2 build
    // still exposes/replicates both schema fields.
    g_pUtils->RegCommand(
        g_PLID,
        {"mm_fovweapon"},
        {"!fovweapon"},
        CommandFOVWeapon
    );

    g_pUtils->RegCommand(
        g_PLID,
        {"mm_fovweapon_info"},
        {"!fovweaponinfo"},
        CommandFOVInfo
    );

    g_pUtils->RegCommand(
        g_PLID,
        {"mm_fovcam"},
        {"!fovcam"},
        CommandFOVCam
    );

    g_pUtils->RegCommand(
        g_PLID,
        {"mm_fovoffset"},
        {"!fovoffset"},
        CommandFOVOffset
    );

    g_pUtils->RegCommand(
        g_PLID,
        {"mm_fovauto"},
        {"!fovauto"},
        CommandFOVAuto
    );

    g_pUtils->RegCommand(
        g_PLID,
        {"mm_fovdiag"},
        {"!fovdiag", "!fovinfo"},
        CommandFOVDiag
    );

    g_pUtils->StartupServer(g_PLID, OnStartupServer);

    // Both event names are present in the supplied libserver.so. Different
    // weapon transitions can use either path, so hook both.
    g_pUtils->HookEvent(g_PLID, "weapon_switch", OnWeaponFOVResetEvent);
    g_pUtils->HookEvent(g_PLID, "item_equip", OnWeaponFOVResetEvent);

    g_pVIPCore->VIP_OnClientLoaded(VIP_OnClientLoaded_FOVWeapon);
    g_pVIPCore->VIP_OnPlayerSpawn(VIP_OnPlayerSpawn_FOVWeapon);

    // Keep the original VIP_FOV feature/cookie names, so groups.ini can stay:
    // "FOV" "90,100,110,120"
    g_pVIPCore->VIP_RegisterFeature(
        "FOV",
        VIP_STRING,
        SELECTABLE,
        OpenFOVMenu
    );

    ConColorMsg(
        Color(80, 220, 120, 255),
        "[VIP-FOVWeapon] v1.4 loaded: world FOV + automatic persistent weapon distance. Commands: !fovcam !fovweapon !fovoffset !fovauto !fovdiag\n"
    );
}

const char* VIPFovWeapon::GetLicense() { return "Public"; }
const char* VIPFovWeapon::GetVersion() { return "1.4-final"; }
const char* VIPFovWeapon::GetDate() { return __DATE__; }
const char* VIPFovWeapon::GetLogTag() { return "[VIP-FOVWeapon]"; }
const char* VIPFovWeapon::GetAuthor() { return "Pisex VIP_FOV + combined viewmodel adaptation"; }

const char* VIPFovWeapon::GetDescription()
{
    return "VIP world FOV plus persistent automatic weapon distance, restored after weapon switches.";
}

const char* VIPFovWeapon::GetName()
{
    return "[VIP] FOV + Weapon";
}

const char* VIPFovWeapon::GetURL()
{
    return "https://github.com/Pisex/cs2-vip-modules";
}
