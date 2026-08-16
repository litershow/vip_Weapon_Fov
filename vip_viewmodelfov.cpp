#include <stdio.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "vip_viewmodelfov.h"

VIPViewmodelFOV g_VIPViewmodelFOV;

IVIPApi* g_pVIPCore = nullptr;
IMenusApi* g_pMenus = nullptr;
IUtilsApi* g_pUtils = nullptr;

IVEngineServer2* engine = nullptr;
static ISchemaSystem* s_pSchemaSystem = nullptr;
CGameEntitySystem* g_pGameEntitySystem = nullptr;
CEntitySystem* g_pEntitySystem = nullptr;

std::vector<std::string> g_VM_FOV[64];
std::vector<std::string> g_VM_Y[64];

PLUGIN_EXPOSE(VIPViewmodelFOV, g_VIPViewmodelFOV);

static int g_iPlayerPawnHandleOffset = -1;
static int g_iViewmodelFOVOffset = -1;
static int g_iViewmodelOffsetYOffset = -1;

#ifdef _WIN32
static constexpr const char* kServerModule = "server.dll";
#else
static constexpr const char* kServerModule = "libserver.so";
#endif

// Minimal runtime schema resolver.
// This replaces SchemaEntity/schemasystem.cpp completely, so the plugin no
// longer drags in unrelated trace/damage/KeyValues3 code.
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
    g_iPlayerPawnHandleOffset =
        FindServerOffset("CCSPlayerController", "m_hPlayerPawn");

    g_iViewmodelFOVOffset =
        FindServerOffset("CCSPlayerPawn", "m_flViewmodelFOV");

    g_iViewmodelOffsetYOffset =
        FindServerOffset("CCSPlayerPawn", "m_flViewmodelOffsetY");

    ConColorMsg(
        Color(80, 220, 120, 255),
        "[VIP-ViewmodelFOV] offsets: PawnHandle=0x%X FOV=0x%X OffsetY=0x%X\n",
        g_iPlayerPawnHandleOffset,
        g_iViewmodelFOVOffset,
        g_iViewmodelOffsetYOffset
    );
}

// Avoid CEntitySystem::GetEntityIdentity() implementation from entitysystem.cpp.
// We only need the public entity list layout from the SDK header.
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

static CEntityInstance* GetPawn(int slot)
{
    if (g_iPlayerPawnHandleOffset < 0)
        ResolveOffsets();

    if (g_iPlayerPawnHandleOffset < 0)
        return nullptr;

    // Player controllers occupy entity indexes slot + 1.
    CEntityInstance* controller = EntityFromIndex(slot + 1);
    if (!controller)
        return nullptr;

    const CEntityHandle& pawnHandle =
        *reinterpret_cast<const CEntityHandle*>(
            reinterpret_cast<uintptr_t>(controller) +
            static_cast<uintptr_t>(g_iPlayerPawnHandleOffset)
        );

    return EntityFromHandle(pawnHandle);
}

static bool SetPawnFloat(
    int slot,
    int fieldOffset,
    const char* fieldName,
    float value)
{
    CEntityInstance* pawn = GetPawn(slot);

    if (!pawn || fieldOffset < 0 || !g_pUtils)
        return false;

    *reinterpret_cast<float*>(
        reinterpret_cast<uintptr_t>(pawn) +
        static_cast<uintptr_t>(fieldOffset)
    ) = value;

    // Utils already knows how to resolve the schema/network chain.
    g_pUtils->SetStateChanged(
        reinterpret_cast<CBaseEntity*>(pawn),
        "CCSPlayerPawn",
        fieldName
    );

    return true;
}

static bool SetViewmodelFOV(int slot, float value)
{
    if (g_iViewmodelFOVOffset < 0)
        ResolveOffsets();

    return SetPawnFloat(
        slot,
        g_iViewmodelFOVOffset,
        "m_flViewmodelFOV",
        value
    );
}

static bool SetViewmodelOffsetY(int slot, float value)
{
    if (g_iViewmodelOffsetYOffset < 0)
        ResolveOffsets();

    return SetPawnFloat(
        slot,
        g_iViewmodelOffsetYOffset,
        "m_flViewmodelOffsetY",
        value
    );
}

static float ReadPawnFloat(CEntityInstance* pawn, int offset)
{
    if (!pawn || offset < 0)
        return 0.0f;

    return *reinterpret_cast<float*>(
        reinterpret_cast<uintptr_t>(pawn) +
        static_cast<uintptr_t>(offset)
    );
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
        if (!token.empty())
            output.push_back(token);
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

static bool CommandVMFOV(int slot, const char* content)
{
    const std::string token = LastToken(content);

    if (token.empty() || token == "mm_vmfov" || token == "!vmfov")
    {
        g_pUtils->PrintToChat(slot, "[VM] Usage: !vmfov 68");
        return true;
    }

    const float value = std::strtof(token.c_str(), nullptr);

    if (!SetViewmodelFOV(slot, value))
    {
        g_pUtils->PrintToChat(slot, "[VM] Failed to update viewmodel FOV.");
        return true;
    }

    g_pUtils->PrintToChat(slot, "[VM] m_flViewmodelFOV = %.2f", value);
    return true;
}

static bool CommandVMY(int slot, const char* content)
{
    const std::string token = LastToken(content);

    if (token.empty() || token == "mm_vmoffsety" || token == "!vmoffsety")
    {
        g_pUtils->PrintToChat(slot, "[VM] Usage: !vmoffsety 2");
        return true;
    }

    const float value = std::strtof(token.c_str(), nullptr);

    if (!SetViewmodelOffsetY(slot, value))
    {
        g_pUtils->PrintToChat(slot, "[VM] Failed to update viewmodel OffsetY.");
        return true;
    }

    g_pUtils->PrintToChat(slot, "[VM] m_flViewmodelOffsetY = %.2f", value);
    return true;
}

static bool CommandVMInfo(int slot, const char*)
{
    CEntityInstance* pawn = GetPawn(slot);

    if (!pawn)
    {
        g_pUtils->PrintToChat(slot, "[VM] Pawn not found.");
        return true;
    }

    if (g_iViewmodelFOVOffset < 0 ||
        g_iViewmodelOffsetYOffset < 0 ||
        g_iPlayerPawnHandleOffset < 0)
    {
        ResolveOffsets();
    }

    const float fov =
        ReadPawnFloat(pawn, g_iViewmodelFOVOffset);

    const float y =
        ReadPawnFloat(pawn, g_iViewmodelOffsetYOffset);

    g_pUtils->PrintToChat(
        slot,
        "[VM] FOV=%.2f Y=%.2f offsets=[pawn:0x%X fov:0x%X y:0x%X]",
        fov,
        y,
        g_iPlayerPawnHandleOffset,
        g_iViewmodelFOVOffset,
        g_iViewmodelOffsetYOffset
    );

    return true;
}

bool VIPViewmodelFOV::Load(
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

bool VIPViewmodelFOV::Unload(char* error, size_t maxlen)
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

static void VIP_OnClientLoaded_VM(int slot, bool isVIP)
{
    g_VM_FOV[slot].clear();
    g_VM_Y[slot].clear();

    if (!isVIP || !g_pVIPCore)
        return;

    SplitList(
        g_pVIPCore->VIP_GetClientFeatureString(slot, "ViewmodelFOV"),
        g_VM_FOV[slot]
    );

    SplitList(
        g_pVIPCore->VIP_GetClientFeatureString(slot, "ViewmodelOffsetY"),
        g_VM_Y[slot]
    );
}

static void VIP_OnPlayerSpawn_VM(int slot, int team, bool isVIP)
{
    if (!isVIP || !g_pVIPCore)
        return;

    if (!g_VM_FOV[slot].empty())
    {
        const char* cookie =
            g_pVIPCore->VIP_GetClientCookie(slot, "ViewmodelFOV_Value");

        const float value =
            (cookie && cookie[0]) ? std::strtof(cookie, nullptr) : 68.0f;

        SetViewmodelFOV(slot, value);
    }

    if (!g_VM_Y[slot].empty())
    {
        const char* cookie =
            g_pVIPCore->VIP_GetClientCookie(slot, "ViewmodelOffsetY_Value");

        const float value =
            (cookie && cookie[0]) ? std::strtof(cookie, nullptr) : 2.0f;

        SetViewmodelOffsetY(slot, value);
    }
}

static bool OpenFOVMenu(int slot, const char*)
{
    Menu menu;
    g_pMenus->SetTitleMenu(menu, "Viewmodel FOV");

    for (const auto& value : g_VM_FOV[slot])
        g_pMenus->AddItemMenu(menu, value.c_str(), value.c_str());

    g_pMenus->SetExitMenu(menu, true);
    g_pMenus->SetBackMenu(menu, true);

    g_pMenus->SetCallback(
        menu,
        [](const char* back, const char*, int item, int slot)
        {
            if (item < static_cast<int>(g_VM_FOV[slot].size()))
            {
                const float value = std::strtof(back, nullptr);
                SetViewmodelFOV(slot, value);

                g_pVIPCore->VIP_SetClientCookie(
                    slot,
                    "ViewmodelFOV_Value",
                    strdup(back)
                );

                OpenFOVMenu(slot, "ViewmodelFOV");
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

static bool OpenYMenu(int slot, const char*)
{
    Menu menu;
    g_pMenus->SetTitleMenu(menu, "Viewmodel Offset Y");

    for (const auto& value : g_VM_Y[slot])
        g_pMenus->AddItemMenu(menu, value.c_str(), value.c_str());

    g_pMenus->SetExitMenu(menu, true);
    g_pMenus->SetBackMenu(menu, true);

    g_pMenus->SetCallback(
        menu,
        [](const char* back, const char*, int item, int slot)
        {
            if (item < static_cast<int>(g_VM_Y[slot].size()))
            {
                const float value = std::strtof(back, nullptr);
                SetViewmodelOffsetY(slot, value);

                g_pVIPCore->VIP_SetClientCookie(
                    slot,
                    "ViewmodelOffsetY_Value",
                    strdup(back)
                );

                OpenYMenu(slot, "ViewmodelOffsetY");
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

void VIPViewmodelFOV::AllPluginsLoaded()
{
    int ret = 0;

    g_pUtils = reinterpret_cast<IUtilsApi*>(
        g_SMAPI->MetaFactory(Utils_INTERFACE, &ret, nullptr)
    );

    if (ret == META_IFACE_FAILED || !g_pUtils)
    {
        ConColorMsg(
            Color(255, 0, 0, 255),
            "[VIP-ViewmodelFOV] Utils API not found.\n"
        );
        return;
    }

    g_pMenus = reinterpret_cast<IMenusApi*>(
        g_SMAPI->MetaFactory(Menus_INTERFACE, &ret, nullptr)
    );

    g_pVIPCore = reinterpret_cast<IVIPApi*>(
        g_SMAPI->MetaFactory(VIP_INTERFACE, &ret, nullptr)
    );

    g_pUtils->RegCommand(
        g_PLID,
        {"mm_vmfov"},
        {"!vmfov"},
        CommandVMFOV
    );

    g_pUtils->RegCommand(
        g_PLID,
        {"mm_vmoffsety"},
        {"!vmoffsety"},
        CommandVMY
    );

    g_pUtils->RegCommand(
        g_PLID,
        {"mm_vminfo"},
        {"!vminfo"},
        CommandVMInfo
    );

    g_pUtils->StartupServer(g_PLID, OnStartupServer);

    if (g_pVIPCore && g_pMenus)
    {
        g_pVIPCore->VIP_OnClientLoaded(VIP_OnClientLoaded_VM);
        g_pVIPCore->VIP_OnPlayerSpawn(VIP_OnPlayerSpawn_VM);

        g_pVIPCore->VIP_RegisterFeature(
            "ViewmodelFOV",
            VIP_STRING,
            SELECTABLE,
            OpenFOVMenu
        );

        g_pVIPCore->VIP_RegisterFeature(
            "ViewmodelOffsetY",
            VIP_STRING,
            SELECTABLE,
            OpenYMenu
        );
    }

    ConColorMsg(
        Color(80, 220, 120, 255),
        "[VIP-ViewmodelFOV] loaded WITHOUT SchemaEntity runtime. Commands: !vmfov !vmoffsety !vminfo\n"
    );
}

const char* VIPViewmodelFOV::GetLicense() { return "Public"; }
const char* VIPViewmodelFOV::GetVersion() { return "0.2-test"; }
const char* VIPViewmodelFOV::GetDate() { return __DATE__; }
const char* VIPViewmodelFOV::GetLogTag() { return "[VIP-ViewmodelFOV]"; }
const char* VIPViewmodelFOV::GetAuthor() { return "OpenAI adaptation for testing"; }

const char* VIPViewmodelFOV::GetDescription()
{
    return "Tests replicated CCSPlayerPawn viewmodel FOV and Y offset.";
}

const char* VIPViewmodelFOV::GetName()
{
    return "[VIP] Viewmodel FOV Test";
}

const char* VIPViewmodelFOV::GetURL()
{
    return "";
}
