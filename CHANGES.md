# Changes - v2.1 safe client command

- Удалён `point_clientcommand`.
- Удалены `CreateEntityByName("point_clientcommand")` и `DispatchSpawn`.
- Исправлен SIGSEGV на старте сервера, вызванный опасным entity-spawn путём предыдущей версии.
- `!clientfov` / `!clientvm` теперь отправляются через `IVEngineServer2::ClientCommand`.
- `#include <eiface.h>` добавлен явно для актуального объявления `IVEngineServer2`/`CPlayerSlot`.
