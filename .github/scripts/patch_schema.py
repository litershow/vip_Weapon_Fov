from pathlib import Path
import os

schema_dir = Path(os.environ["SCHEMA_DIR"])
p = schema_dir / "globaltypes.h"
s = p.read_text(encoding="utf-8")

marker = '#include "soundflags.h"'
compat = r'''
// ---- vip_Weapon_Fov build compatibility ----
// These trace/hitbox helper types are referenced by unrelated
// SchemaEntity helpers but are absent from the current AM hl2sdk-cs2.
// The FOV module does not use these helper structures directly.
struct Ray_t
{
    unsigned char _opaque[64];
};

class CHitBox;

enum RayType_t : int
{
    RAY_TYPE_COMPAT = 0
};

enum HitGroup_t : int
{
    HITGROUP_COMPAT = 0
};
// ---- end compatibility ----
'''

if "RAY_TYPE_COMPAT" not in s:
    if marker not in s:
        raise SystemExit(f"Could not find insertion marker in {p}")
    s = s.replace(marker, marker + "\n" + compat)

p.write_text(s, encoding="utf-8")
print(f"Patched: {p}")
