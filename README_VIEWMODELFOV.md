# VIP_ViewmodelFOV test module

This is based on the build layout of Pisex VIP_FOV, but tests the pawn fields
`m_flViewmodelFOV` and `m_flViewmodelOffsetY`.

Important: this is a TEST. It proves whether the CS2 client honors these
replicated server fields for the first-person viewmodel. If chat/`!vminfo`
shows the fields changing but the first-person weapon does not change,
then the final rendering is client-side and a server-only module cannot
force that visual effect.

## Test commands

In chat:
- `!vmfov 60`
- `!vmfov 68`
- `!vmfov 90`
- `!vmoffsety -2`
- `!vmoffsety 2`
- `!vmoffsety 6`
- `!vminfo`

Console forms:
- `mm_vmfov 68`
- `mm_vmoffsety 2`
- `mm_vminfo`

The module resolves the schema offsets at runtime with `SchemaEntity`, rather
than hard-coding offsets from a dump.

## Optional VIP configuration

In `addons/configs/vip/groups.ini`, in the desired group:

```
"ViewmodelFOV"      "60,68,80,90,100"
"ViewmodelOffsetY"  "-2,0,2,4,6"
```

## Build layout

You need this layout:

```
work/
├── SchemaEntity/
└── VIP_ViewmodelFOV/
    └── source/
```

This archive is the `VIP_ViewmodelFOV` folder.

Get SchemaEntity:

```
cd work
git clone https://github.com/Pisex/SchemaEntity.git SchemaEntity
```

You also need AMBuild 2, Metamod:Source sources, and the CS2 HL2SDK tree.

A convenient Linux/WSL layout is:

```
~/cs2build/
├── metamod-source/
├── hl2sdk-root/
│   └── hl2sdk-cs2/
├── SchemaEntity/
└── VIP_ViewmodelFOV/
```

Metamod's own build helper can fetch its SDK dependencies:

```
git clone --recurse-submodules https://github.com/alliedmodders/metamod-source.git
cd metamod-source
./support/checkout-deps.sh -s cs2
```

Then use the resulting CS2 SDK location as `--hl2sdk-root` (the parent folder
that contains `hl2sdk-cs2`).

Configure this module from its `source` directory, for example:

```
cd ~/cs2build/VIP_ViewmodelFOV/source

python3 configure.py \
  --mms_path=$HOME/cs2build/metamod-source \
  --hl2sdk-root=$HOME/cs2build \
  --hl2sdk-manifests=$PWD/hl2sdk-manifests \
  -s cs2 \
  --targets=x86_64 \
  --enable-optimize
```

Then:

```
ambuild obj
```

If configuration creates `obj` successfully, the built library will be under
the `obj` tree. `PackageScript` can package it using the same layout as the
original Pisex module.

## First test

Do NOT start with huge values.

1. Join server and spawn.
2. Run:
   `!vminfo`
3. Run:
   `!vmfov 60`
4. Check weapon.
5. Run:
   `!vmfov 68`
6. Check weapon.
7. Run:
   `!vmoffsety -2`
8. Run:
   `!vmoffsety 2`
9. Check weapon each time.
10. Run `!vminfo` and send the output.

If 60/68 or -2/2 work visually, then try 80/90 and 4/6.
If the server values change but the view does not, that is a strong result:
the server field is not the final first-person renderer control.
