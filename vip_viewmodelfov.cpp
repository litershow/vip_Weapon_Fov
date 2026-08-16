#include <stdio.h>
#include <iostream>
#include <sstream>
#include <vector>
#include <string>
#include <cstdlib>

#include "vip_viewmodelfov.h"
#include "schemasystem/schemasystem.h"
#include "schemasystem.h"

VIPViewmodelFOV g_VIPViewmodelFOV;

IVIPApi* g_pVIPCore = nullptr;
IMenusApi* g_pMenus = nullptr;
IUtilsApi* g_pUtils = nullptr;

IVEngineServer2* engine = nullptr;
CGameEntitySystem* g_pGameEntitySystem = nullptr;
CEntitySystem* g_pEntitySystem = nullptr;

std::vector<std::string> g_VM_FOV[64];
std::vector<std::string> g_VM_Y[64];

PLUGIN_EXPOSE(VIPViewmodelFOV, g_VIPViewmodelFOV);

static int g_iViewmodelFOVOffset = -1;
static int g_iViewmodelOffsetYOffset = -1;

static void ResolveOffsets()
{
    g_iViewmodelFOVOffset =
        schema::GetServerOffset("CCSPlayerPawn", "m_flViewmodelFOV");

    g_iViewmodelOffsetYOffset =
        schema::GetServerOffset("CCSPlayerPawn", "m_flViewmodelOffsetY");

    ConColorMsg(
        Color(80, 220, 120, 255),
        "[VIP-ViewmodelFOV] resolved offsets: FOV=0x%X OffsetY=0x%X\n",
        g_iViewmodelFOVOffset,
        g_iViewmodelOffsetYOffset
    );
}

static CCSPlayerPawn* GetPawn(int iSlot)
{
    CCSPlayerController* controller = CCSPlayerController::FromSlot(iSlot);
    if (!controller)
        return nullptr;

    return controller->m_hPlayerPawn().Get();
}

static bool SetViewmodelFOV(int iSlot, float value)
{
    CCSPlayerPawn* pawn = GetPawn(iSlot);
    if (!pawn)
        return false;

    if (g_iViewmodelFOVOffset < 0)
        ResolveOffsets();

    if (g_iViewmodelFOVOffset < 0)
        return false;

    *reinterpret_cast<float*>(
        reinterpret_cast<uintptr_t>(pawn) + g_iViewmodelFOVOffset
    ) = value;

    g_pUtils->SetStateChanged(
        reinterpret_cast<CBaseEntity*>(pawn),
        "CCSPlayerPawn",
        "m_flViewmodelFOV"
    );

    return true;
}

static bool SetViewmodelOffsetY(int iSlot, float value)
{
    CCSPlayerPawn* pawn = GetPawn(iSlot);
    if (!pawn)
        return false;

    if (g_iViewmodelOffsetYOffset < 0)
        ResolveOffsets();

    if (g_iViewmodelOffsetYOffset < 0)
        return false;

    *reinterpret_cast<float*>(
        reinterpret_cast<uintptr_t>(pawn) + g_iViewmodelOffsetYOffset
    ) = value;

    g_pUtils->SetStateChanged(
        reinterpret_cast<CBaseEntity*>(pawn),
        "CCSPlayerPawn",
        "m_flViewmodelOffsetY"
    );

    return true;
}

static float ReadPawnFloat(CCSPlayerPawn* pawn, int offset)
{
    if (!pawn || offset < 0)
        return 0.0f;

    return *reinterpret_cast<float*>(
        reinterpret_cast<uintptr_t>(pawn) + offset
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

static bool CommandVMFOV(int iSlot, const char* content)
{
    std::string token = LastToken(content);
    if (token.empty() || token == "mm_vmfov" || token == "!vmfov")
    {
        g_pUtils->PrintToChat(iSlot, "[VM] Usage: !vmfov 68");
        return true;
    }

    float value = std::strtof(token.c_str(), nullptr);

    if (!SetViewmodelFOV(iSlot, value))
    {
        g_pUtils->PrintToChat(iSlot, "[VM] Failed to update viewmodel FOV.");
        return true;
    }

    g_pUtils->PrintToChat(iSlot, "[VM] m_flViewmodelFOV = %.2f", value);
    return true;
}

static bool CommandVMY(int iSlot, const char* content)
{
    std::string token = LastToken(content);
    if (token.empty() || token == "mm_vmoffsety" || token == "!vmoffsety")
    {
        g_pUtils->PrintToChat(iSlot, "[VM] Usage: !vmoffsety 2");
        return true;
    }

    float value = std::strtof(token.c_str(), nullptr);

    if (!SetViewmodelOffsetY(iSlot, value))
    {
        g_pUtils->PrintToChat(iSlot, "[VM] Failed to update viewmodel OffsetY.");
        return true;
    }

    g_pUtils->PrintToChat(iSlot, "[VM] m_flViewmodelOffsetY = %.2f", value);
    return true;
}

static bool CommandVMInfo(int iSlot, const char*)
{
    CCSPlayerPawn* pawn = GetPawn(iSlot);
    if (!pawn)
    {
        g_pUtils->PrintToChat(iSlot, "[VM] Pawn not found.");
        return true;
    }

    if (g_iViewmodelFOVOffset < 0 || g_iViewmodelOffsetYOffset < 0)
        ResolveOffsets();

    float fov = ReadPawnFloat(pawn, g_iViewmodelFOVOffset);
    float y = ReadPawnFloat(pawn, g_iViewmodelOffsetYOffset);

    g_pUtils->PrintToChat(
        iSlot,
        "[VM] server fields: FOV=%.2f OffsetY=%.2f offsets=[0x%X,0x%X]",
        fov, y, g_iViewmodelFOVOffset, g_iViewmodelOffsetYOffset
    );

    return true;
}

bool VIPViewmodelFOV::Load(
    PluginId id,
    ISmmAPI *ismm,
    char *error,
    size_t maxlen,
    bool late)
{
    PLUGIN_SAVEVARS();

    GET_V_IFACE_ANY(
        GetEngineFactory,
        g_pSchemaSystem,
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

bool VIPViewmodelFOV::Unload(char *error, size_t maxlen)
{
    if (g_pUtils)
        g_pUtils->ClearAllHooks(g_PLID);

    return true;
}

void OnStartupServer()
{
    g_pGameEntitySystem = g_pUtils->GetCGameEntitySystem();
    g_pEntitySystem = g_pGameEntitySystem;
    ResolveOffsets();
}

void VIP_OnClientLoaded_VM(int iSlot, bool bIsVIP)
{
    g_VM_FOV[iSlot].clear();
    g_VM_Y[iSlot].clear();

    if (!bIsVIP)
        return;

    SplitList(
        g_pVIPCore->VIP_GetClientFeatureString(iSlot, "ViewmodelFOV"),
        g_VM_FOV[iSlot]
    );

    SplitList(
        g_pVIPCore->VIP_GetClientFeatureString(iSlot, "ViewmodelOffsetY"),
        g_VM_Y[iSlot]
    );
}

void VIP_OnPlayerSpawn_VM(int iSlot, int iTeam, bool bIsVIP)
{
    if (!bIsVIP)
        return;

    if (!g_VM_FOV[iSlot].empty())
    {
        const char* cookie =
            g_pVIPCore->VIP_GetClientCookie(iSlot, "ViewmodelFOV_Value");

        float value = (cookie && cookie[0])
            ? std::strtof(cookie, nullptr)
            : 68.0f;

        SetViewmodelFOV(iSlot, value);
    }

    if (!g_VM_Y[iSlot].empty())
    {
        const char* cookie =
            g_pVIPCore->VIP_GetClientCookie(iSlot, "ViewmodelOffsetY_Value");

        float value = (cookie && cookie[0])
            ? std::strtof(cookie, nullptr)
            : 2.0f;

        SetViewmodelOffsetY(iSlot, value);
    }
}

static bool OpenFOVMenu(int iSlot, const char*)
{
    Menu menu;
    g_pMenus->SetTitleMenu(menu, "Viewmodel FOV");

    for (const auto& value : g_VM_FOV[iSlot])
        g_pMenus->AddItemMenu(menu, value.c_str(), value.c_str());

    g_pMenus->SetExitMenu(menu, true);
    g_pMenus->SetBackMenu(menu, true);

    g_pMenus->SetCallback(
        menu,
        [](const char* back, const char*, int item, int iSlot)
        {
            if (item < static_cast<int>(g_VM_FOV[iSlot].size()))
            {
                float value = std::strtof(back, nullptr);
                SetViewmodelFOV(iSlot, value);
                g_pVIPCore->VIP_SetClientCookie(
                    iSlot,
                    "ViewmodelFOV_Value",
                    strdup(back)
                );
                OpenFOVMenu(iSlot, "ViewmodelFOV");
            }
            else
            {
                g_pVIPCore->VIP_OpenMenu(iSlot);
            }
        }
    );

    g_pMenus->DisplayPlayerMenu(menu, iSlot);
    return false;
}

static bool OpenYMenu(int iSlot, const char*)
{
    Menu menu;
    g_pMenus->SetTitleMenu(menu, "Viewmodel Offset Y");

    for (const auto& value : g_VM_Y[iSlot])
        g_pMenus->AddItemMenu(menu, value.c_str(), value.c_str());

    g_pMenus->SetExitMenu(menu, true);
    g_pMenus->SetBackMenu(menu, true);

    g_pMenus->SetCallback(
        menu,
        [](const char* back, const char*, int item, int iSlot)
        {
            if (item < static_cast<int>(g_VM_Y[iSlot].size()))
            {
                float value = std::strtof(back, nullptr);
                SetViewmodelOffsetY(iSlot, value);
                g_pVIPCore->VIP_SetClientCookie(
                    iSlot,
                    "ViewmodelOffsetY_Value",
                    strdup(back)
                );
                OpenYMenu(iSlot, "ViewmodelOffsetY");
            }
            else
            {
                g_pVIPCore->VIP_OpenMenu(iSlot);
            }
        }
    );

    g_pMenus->DisplayPlayerMenu(menu, iSlot);
    return false;
}

void VIPViewmodelFOV::AllPluginsLoaded()
{
    int ret = 0;

    g_pUtils = (IUtilsApi*)g_SMAPI->MetaFactory(
        Utils_INTERFACE,
        &ret,
        nullptr
    );

    if (ret == META_IFACE_FAILED || !g_pUtils)
    {
        ConColorMsg(
            Color(255, 0, 0, 255),
            "[VIP-ViewmodelFOV] Utils API not found.\n"
        );
        return;
    }

    g_pMenus = (IMenusApi*)g_SMAPI->MetaFactory(
        Menus_INTERFACE,
        &ret,
        nullptr
    );

    g_pVIPCore = (IVIPApi*)g_SMAPI->MetaFactory(
        VIP_INTERFACE,
        &ret,
        nullptr
    );

    // Test commands work even before you configure VIP groups.
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
        "[VIP-ViewmodelFOV] loaded. Commands: !vmfov !vmoffsety !vminfo\n"
    );
}

const char *VIPViewmodelFOV::GetLicense() { return "Public"; }
const char *VIPViewmodelFOV::GetVersion() { return "0.1-test"; }
const char *VIPViewmodelFOV::GetDate() { return __DATE__; }
const char *VIPViewmodelFOV::GetLogTag() { return "[VIP-ViewmodelFOV]"; }
const char *VIPViewmodelFOV::GetAuthor() { return "OpenAI adaptation for testing"; }
const char *VIPViewmodelFOV::GetDescription()
{
    return "Tests replicated CCSPlayerPawn viewmodel FOV and Y offset.";
}
const char *VIPViewmodelFOV::GetName() { return "[VIP] Viewmodel FOV Test"; }
const char *VIPViewmodelFOV::GetURL() { return ""; }
