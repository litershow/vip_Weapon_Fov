# VIP_FOVWeapon Chain Detour v5

Эта версия исправляет случай `native matches=0`, когда начало `CameraServices::SetFOV` уже изменено другим Metamod/CSS hook-ом.

## Что изменено

- Поиск `SetFOV` идёт по телу функции (`m_iFOV` + `m_hZoomOwner`), а не по первым байтам пролога.
- Если вход функции чистый — ставится обычный trampoline detour.
- Если вход уже содержит поддерживаемый detour (`jmp rel32`, `mov rax, imm64; jmp rax`, `mov r11, imm64; jmp r11`) — плагин ставится цепочкой и вызывает предыдущий hook.
- Если найден неизвестный патч — hook не ставится, чтобы не крашнуть сервер.
- Все schema offsets берутся runtime через SchemaSystem.

## Команды

- `!fovchain 120` — включить FOV 120.
- `!fovchain off` — отключить и вернуть стандартный FOV.
- `!fovchaininfo` — диагностика.

В диагностике важны:

- `detour=YES`
- `native matches=1`
- `entry=clean` или `entry=prehooked-chainable`
- `resetsBlocked` должен расти при тех переключениях оружия, где CS2 раньше сбрасывал FOV.

## Важно

Перед тестом полностью перезапустите сервер и оставьте только один `vip_viewmodelfov.so`/VDF этой версии. Старые экспериментальные FOV-модули отключите.
