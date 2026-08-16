# VIP FOVWeapon Resolver6 (Linux CS2)

Эта версия не полагается только на старую сигнатуру SetFOV.

Она пытается найти штатный вызов сброса:
`SetFOV(cameraServices, pawn, 0, 0, 0)`
по runtime Schema offset `CBasePlayerPawn::m_pCameraServices`, затем декодирует
реальный адрес SetFOV из инструкции `call rel32`.

## Тест

Оставьте только один `vip_viewmodelfov.so`, полностью перезапустите сервер.

В игре:

- `!fovscan6` — диагностика резолвера
- `!fovv6 120` — включить FOV 120
- `!fovv6 off` — выключить и вернуть игровой FOV

Нормальная диагностика:

`resolver=reset-call` или `resolver=body+reset-call`
`resetSites>=1 targets=1`
`entry=clean` или `entry=prehooked-chainable`
`detour=YES`

Если target найден, но entry неизвестен, `!fovscan6` покажет первые 16 байт entry.
Плагин в таком случае НЕ ставит hook, чтобы не крашить сервер.
