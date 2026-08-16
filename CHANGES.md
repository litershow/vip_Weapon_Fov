# Resolver6

- Enumerates every loaded `server.so` / `libserver.so` instead of stopping on the first basename match.
- New primary resolver: finds the game's own zero-FOV reset call using runtime Schema `m_pCameraServices`, then decodes the native SetFOV target from `E8 rel32`.
- Old SetFOV body anchor retained as an independent cross-check/fallback.
- Refuses to hook when independent resolvers disagree.
- Diagnostic command reports module count, body matches, reset callsite count, unique targets, RVA, entry state and first 16 target bytes.
- Unique test commands: `!fovv6`, `!fovscan6`.
