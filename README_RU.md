# VIP FOVWeapon Prediction7

Эта версия проверяет гипотезу, которую подсказал успешный Resolver6:

- native `CameraServices::SetFOV` detour уже работает (`detour=YES`);
- reset `target=0` реально блокируется (`resetsBlocked` растёт);
- но при смене оружия клиент всё равно на один кадр показывает стандартный FOV.

Prediction7 держит **две сетевые величины одновременно**:

1. `CBasePlayerController::m_iDesiredFOV = выбранный FOV` — постоянный fallback для локального prediction;
2. `CCSPlayerBase_CameraServices::m_iFOV/m_iFOVStart = выбранный FOV` через настоящий native `SetFOV` — тот механизм, который меняет и камеру, и оружие.

Когда CS2 пытается сделать native `SetFOV(..., 0, ...)`, detour по-прежнему заменяет reset на выбранный FOV. Идея нового слоя: если клиент сам локально предсказывает `FOV=0` во время deploy, его fallback уже должен быть не 90, а `m_iDesiredFOV` игрока.

## Тестовые команды

```
!fovpred7 120
!fovscan7
!fovpred7 off
```

После `!fovpred7 120` ожидается:

```
[FOV7] target=120 desired=120 cam=120/120 detour=YES ...
```

Переключите нож/пистолет/автомат несколько раз и снова выполните `!fovscan7`.

Если визуальный рывок исчез — это нужная комбинация для финального VIP-модуля.

Если `desired=120`, `cam=120/120`, `resetsBlocked` растёт, но рывок всё равно остаётся, то он уже почти наверняка формируется целиком в клиентском deploy/view prediction до использования этих серверных network vars.

## Важно

Оставьте только один `vip_viewmodelfov.so` и один соответствующий `.vdf`. Полностью перезапустите процесс сервера перед тестом.
