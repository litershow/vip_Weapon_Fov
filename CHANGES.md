# Changes — 1.4 final

- one authoritative per-player FOV state;
- debug commands and VIP menu no longer fight over different targets;
- `!fovcam` updates the VIP `FOV_Value` cookie for VIP players;
- automatic persistent viewmodel XYZ compensation for wide FOV;
- manual `!fovoffset X Y Z` is persisted through weapon changes;
- `!fovauto` restores automatic weapon-distance calculation;
- six-frame event-driven reapply burst after `weapon_switch` / `item_equip`;
- SchemaSystem offsets remain runtime-resolved (no hard-coded 0x1504 etc.);
- output remains `vip_viewmodelfov.so`;
- documented requirement to remove all older/original FOV modules to avoid multiple plugins overwriting each other.
