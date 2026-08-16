#ifndef _INCLUDE_VIP_VIEWMODELFOV_PLUGIN_H_
#define _INCLUDE_VIP_VIEWMODELFOV_PLUGIN_H_

#include <ISmmPlugin.h>
#include <igameevents.h>
#include <iserver.h>
#include <entity2/entitysystem.h>
#include <entity2/entityinstance.h>
#include <schemasystem/schemasystem.h>
#include <steam/steamclientpublic.h>

class CBaseEntity;
class CBaseModelEntity;
class CCSGameRules;
class CTimer;

#include "include/vip.h"
#include "include/menus.h"

class VIPViewmodelFOV : public ISmmPlugin, public IMetamodListener
{
public:
    bool Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late);
    bool Unload(char* error, size_t maxlen);
    void AllPluginsLoaded();

public:
    const char* GetAuthor();
    const char* GetName();
    const char* GetDescription();
    const char* GetURL();
    const char* GetLicense();
    const char* GetVersion();
    const char* GetDate();
    const char* GetLogTag();
};

extern VIPViewmodelFOV g_VIPViewmodelFOV;

PLUGIN_GLOBALVARS();

#endif
