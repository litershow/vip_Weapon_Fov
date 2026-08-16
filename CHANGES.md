# v5 chain-detour

- Replaced fragile prologue signature with a body anchor based on runtime Schema offsets.
- Added detection/chaining for an already-detoured SetFOV entry.
- Refuses unknown entry patches instead of overwriting them.
- Added unique test commands `!fovchain` / `!fovchaininfo`.
