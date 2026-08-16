# Changes

- Preserved VIP feature name `FOV` and cookie `FOV_Value`.
- Added runtime SchemaSystem lookup for `CBasePlayerController::m_iDesiredFOV`.
- Added runtime SchemaSystem lookup for `CCSPlayerPawn::m_flViewmodelFOV`.
- Selecting/spawning now applies camera FOV and weapon/viewmodel FOV together.
- Added proportional world->viewmodel FOV mapping.
- Added `!fovweapon` and `!fovweaponinfo` test commands.
- No hard-coded CS2 offsets are used.
