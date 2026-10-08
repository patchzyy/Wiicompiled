# Sonic as a playable driver

WiiCompiled can put **Sonic from Sonic Adventure DX** into Mario Kart Wii as a playable
character. He takes over one roster slot (Luigi by default) and replaces that driver
everywhere the game shows them:

- **In races and menus**: Sonic's DX model in a driving pose (seated, legs forward,
  both hands on the wheel). He is scaled to the driver he replaces, follows the kart
  and leans with the driver's body. In menus and on the podium, where the driver
  stands, he stands with his idle animation.
- **Roster icons**: every icon of the slot (`tt_<name>_64x64.tpl`, `st_<name>_32x32.tpl`
  and the other sizes) is a portrait rendered from Sonic's model, framed like the
  Mario Kart Wii icons: head and shoulders, 3/4 view, dark outline, transparent
  background.
- **Name**: the character's name message (`Common.bmg` message 9000 + slot) reads
  "Sonic" (ソニック in Japanese, 소닉 in Korean) in every language.

The slot keeps its stats, weight class, vehicles, unlocks and save data, and its voice
clips.

## Nothing is shipped

No Nintendo or Sega data is in this repository. Sonic is built at runtime from your own
Sonic Adventure DX files: his model, animations and textures come from
`system\CHRMODELS_orig.dll` (or `CHRMODELS.dll`) and `system\SONIC.PVM`, read by
[SonicCore](../runtime/third_party/soniccore) (MIT). The icons are rendered from that
model on your machine. Patched Mario Kart Wii archives are written to your cache folder
(`<app data>/Cache/sonic`), never next to your game files, and never leave your machine.

## Setup

1. Have the Steam or 2004 PC version of **Sonic Adventure DX** installed.
2. In `Config.toml` (next to the program, or in the app data folder), set:

```toml
[sonic]
enabled = true
replaces = "luigi"
assets = "D:\\Games\\Sonic Adventure DX"
```

`assets` is the SADX folder that contains `system\`. A folder made by SonicCore's
`sonic_extract` tool (`SonicAssets`) works too. Without `assets`, a `SonicAssets` folder
next to `Config.toml`, next to the program or in the app data folder is used.

3. Start the game. The log shows a line like this (the numbers depend on your files):

```
[sonic] Sonic replaces Luigi: 108 driver model(s) in 36 file(s), 23 UI archive(s) patched (850 ms)
```

The first start after a change takes a moment while the archives are patched; later
starts reuse the cache.

## Options

| key        | default   | meaning |
|------------|-----------|---------|
| `enabled`  | `true`    | Turn Sonic on or off. Without SADX files nothing changes either way. |
| `replaces` | `"luigi"` | The roster slot Sonic takes. English name (`"King Boo"`), file abbreviation (`"kt"`) or icon name (`"teresa"`). |
| `assets`   | (search)  | Your Sonic Adventure DX folder, or a SonicAssets folder. Relative paths are relative to `Config.toml`. |
| `model`    | `true`    | Draw Sonic instead of the driver's 3D model. |
| `icons`    | `true`    | Replace the slot's roster icons. |
| `name`     | `true`    | Replace the slot's name. |
| `scale`    | `1.0`     | Extra size multiplier for Sonic's 3D model. |
| `debug`    | `false`   | Log every model the draw hook sees (address, fingerprint, first bone names) and every patched file. |

Any roster character can be replaced. Medium characters (Luigi, Mario, Peach, Daisy,
Yoshi, Birdo, Diddy Kong, Dry Bones) fit Sonic's size best.

## How it works

Two parts, both in `runtime/src/sonic/`:

**Disc layer** (`sonic_disc_patch.cpp`, run from `DVDInit` after the disc and any
Riivolution/Retro Rewind overlays are registered):

- every `Scene/UI/*.szs` archive (and `Race/Common.szs`) is decompressed (Yaz0), opened
  (U8), and the slot's icon textures and name message are replaced; the archive is
  rebuilt, recompressed and registered in place of the original, exactly like an
  overlay file. Archives that do not mention the slot are left alone. Because the
  overlays are applied first, mods that replace the UI are patched too.
- the slot's driver models (`driver_model.brres` in `Race/Kart/*-<abbr>*.szs`, and
  the slot's models in `Scene/Model/` and `Demo/`) are fingerprinted: a hash of each
  MDL0's vertex position data, which nw4r never rewrites when it binds a model.
- results are cached under `<app data>/Cache/sonic`, keyed by each source file's path,
  size and modification time.

**Draw layer** (`sonic_draw_hook.cpp`): a native wrapper around
`nw4r::g3d::DrawResMdlDirectly` (0x80069000), registered with
`REGISTER_NATIVE_FUNCTION_AS` so the original translated function still draws every
other model. When the model being drawn has one of the fingerprints, the wrapper:

1. reads the driver's bone matrices from the view matrix array the game passes in and
   works out, in the driver's root bone frame, whether the driver is seated (feet well
   in front of the head) or standing, which way it faces, how big it is and how far it
   leans (`sonic_place.cpp`);
2. poses Sonic accordingly (`sonic_render.cpp`): the driving pose is built from two
   SADX animations, the body of the "holding on" seat pose (`SONIC_ACTIONS` 89) with the
   legs of the "sitting, legs forward" pose (108), mirrored so both hands are on the
   wheel and both legs reach the pedals;
3. bakes lighting into vertex colours and draws Sonic with GX immediate mode in the
   opaque pass (textures go to aurora as linear RGBA, `GX_TF_RGBA8_PC`);
4. restores the HLE vertex descriptor state, invalidates the texture binding cache for
   the texture map it used and calls `nw4r::g3d::G3DState::Invalidate` so g3d reloads
   its GX state for the next model.

The draw hook never touches models it does not recognise, and the disc layer never
fails the boot: if anything goes wrong the game keeps its own files.

## Limitations

- Sonic replaces a slot; he is not a 25th roster entry (that would need new layout
  files, character tables and online handling).
- Voice clips and the character's sound effects stay the replaced driver's.
- Kart colours and emblems that depend on the character stay the replaced driver's.
- The driving pose is static (no steering or trick animation); the lean follows the
  driver's upper body sideways.
- Online play: other players see the original character.

## Troubleshooting

- **"Sonic Adventure DX files not found"**: check `assets`. It must be the folder that
  contains `system\CHRMODELS_orig.dll` (or `CHRMODELS.dll`) and `system\SONIC.PVM`.
- **"no driver models found"**: the slot's `Race/Kart` files were not found under that
  abbreviation. Turn on `debug = true` and send the log.
- **Icons or name not replaced**: with `debug = true` the log lists every patched file
  and texture.
- **Sonic too big or too small**: adjust `scale`.
- To force a rebuild, delete `<app data>/Cache/sonic`.
