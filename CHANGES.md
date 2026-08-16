# Prediction7

- Added runtime Schema resolution for `CBasePlayerController::m_iDesiredFOV`.
- `m_iDesiredFOV` is published before applying native CameraServices FOV.
- Native `SetFOV` zero-reset detour from Resolver6 is kept unchanged.
- Desired FOV is repaired only if game code changes it.
- `off` clears DesiredFOV first, then returns CameraServices to native game FOV.
- Unique commands: `!fovpred7`, `!fovscan7`.
