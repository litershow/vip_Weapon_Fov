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

PLUGIN_EXPOSE(VIPFovWeapon, g_VIPFovWeapon);

// Runtime schema offsets. Nothing here is hard-coded to a particular CS2 build.
static int g_iDesiredFOVOffset = -1;
static int g_iPlayerPawnHandleOffset = -1;
static int g_iViewmodelFOVOffset = -1;

// CS2 defaults used to make the weapon move visually together with world FOV.
// Example: world 110 => viewmodel ~83.11, world 120 => viewmodel ~90.67.
static constexpr float kBaseWorldFOV = 90.0f;
static constexpr float kBaseViewmodelFOV = 68.0f;
static constexpr float kMinViewmodelFOV = 1.0f;
static constexpr float kMaxViewmodelFOV = 179.0f;

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

    ConColorMsg(
        Color(80, 220, 120, 255),
        "[VIP-FOVWeapon] offsets: DesiredFOV=0x%X PawnHandle=0x%X ViewmodelFOV=0x%X\n",
        g_iDesiredFOVOffset,
        g_iPlayerPawnHandleOffset,
        g_iViewmodelFOVOffset
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

static float ViewmodelFOVForWorldFOV(int worldFOV)
{
    const float scaled =
        kBaseViewmodelFOV * (static_cast<float>(worldFOV) / kBaseWorldFOV);

    return std::clamp(scaled, kMinViewmodelFOV, kMaxViewmodelFOV);
}

// Apply both fields together. This is the important difference from stock
// VIP_FOV, which only changes CBasePlayerController::m_iDesiredFOV.
static bool ApplyCombinedFOV(int slot, int worldFOV)
{
    const float viewmodelFOV = ViewmodelFOVForWorldFOV(worldFOV);

    const bool worldOk = SetWorldFOV(slot, worldFOV);
    const bool weaponOk = SetViewmodelFOV(slot, viewmodelFOV);

    if (!worldOk || !weaponOk)
    {
        ConColorMsg(
            Color(255, 180, 50, 255),
            "[VIP-FOVWeapon] apply failed for slot %d: world=%d weapon=%d (FOV=%d VM=%.2f)\n",
            slot,
            worldOk ? 1 : 0,
            weaponOk ? 1 : 0,
            worldFOV,
            viewmodelFOV
        );
    }

    return worldOk && weaponOk;
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

    const float vm = ViewmodelFOVForWorldFOV(value);

    if (!ApplyCombinedFOV(slot, value))
    {
        g_pUtils->PrintToChat(slot, "[FOV] Failed. Check server console offsets.");
        return true;
    }

    g_pUtils->PrintToChat(slot, "[FOV] world=%d viewmodel=%.2f", value, vm);
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

    // Re-apply one frame after spawn so the new pawn definitely exists.
    g_pUtils->NextFrame([slot, worldFOV]()
    {
        ApplyCombinedFOV(slot, worldFOV);
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
                    ApplyCombinedFOV(slot, worldFOV);

                    g_pVIPCore->VIP_SetClientCookie(
                        slot,
                        "FOV_Value",
                        strdup(back)
                    );
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

    g_pUtils->StartupServer(g_PLID, OnStartupServer);

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
        "[VIP-FOVWeapon] loaded. FOV now changes camera + viewmodel. Commands: !fovweapon !fovweaponinfo\n"
    );
}

const char* VIPFovWeapon::GetLicense() { return "Public"; }
const char* VIPFovWeapon::GetVersion() { return "1.1"; }
const char* VIPFovWeapon::GetDate() { return __DATE__; }
const char* VIPFovWeapon::GetLogTag() { return "[VIP-FOVWeapon]"; }
const char* VIPFovWeapon::GetAuthor() { return "Pisex VIP_FOV + combined viewmodel adaptation"; }

const char* VIPFovWeapon::GetDescription()
{
    return "VIP FOV that changes both camera/world FOV and weapon/viewmodel FOV.";
}

const char* VIPFovWeapon::GetName()
{
    return "[VIP] FOV + Weapon";
}

const char* VIPFovWeapon::GetURL()
{
    return "https://github.com/Pisex/cs2-vip-modules";
}
