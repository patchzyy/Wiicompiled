# Sonic as a playable racer

WiiCompiled can add **Sonic from Sonic Adventure DX** to Mario Kart Wii as a new
playable character who **races on foot**, with his own SADX physics, against the other
racers. Every other character stays as it is.

- **Character select**: a new Sonic button in the grid, with a portrait rendered from his
  model, his name ("Sonic"; ソニック in Japanese, 소닉 in Korean) and Sonic standing in the
  3D preview while he is highlighted.
- **Races**: Sonic runs, jumps, spin dashes and homing-attacks with Sonic Adventure DX's
  own physics (ported in [SonicCore](../runtime/third_party/soniccore)) on the course's
  real collision. Laps, positions, items, the camera, Lakitu, cannons and the finish all
  work as for a kart.

Nothing from Nintendo or Sega is shipped: Sonic is built at runtime from your own SADX
files (see below).

## Setup

1. Have the Steam or 2004 PC version of **Sonic Adventure DX** installed.
2. In `Config.toml` (next to the program, or in the app data folder), set:

```toml
[sonic]
enabled = true
assets = "D:\\Games\\Sonic Adventure DX"
```

`assets` is the SADX folder that contains `system\`. A folder made by SonicCore's
`sonic_extract` tool (`SonicAssets`) works too. Without `assets`, a `SonicAssets` folder
next to `Config.toml`, next to the program or in the app data folder is used.

3. Start the game. The log shows lines like these:

```
[sonic] Sonic loaded from D:\Games\Sonic Adventure DX; he plays as Mario under the hood
[sonic] Sonic (as Mario): 120 Mario model(s) in 40 file(s), 23 UI archive(s) patched (900 ms)
[sonic] Sonic button added to the character select grid at (...)
```

## Controls (in a race)

| Mario Kart control | Sonic |
|---|---|
| Accelerate (A / 2) | Run forward |
| Stick / tilt | Steer (relative to where he is running) |
| Drift / hop (R / B) | Jump; hold for a higher jump; press again in the air for a homing attack (towards a nearby racer) or an air dash |
| Brake (B / 1) | Spin dash: hold to charge, let go to launch. Holding it through the countdown or while landing charges as soon as he touches the ground |
| Trick (d-pad / shake) | Jump |
| Item | Use the item, as usual |

Grass, sand and dirt don't slow him: he is on foot. Boost panels and mushrooms push him
to about a quarter above his top speed.

## Speed

Sonic is about as tall as Mario (1 SADX unit = 9 Mario Kart units at `scale = 1`).

| `physics` | top speed on flat ground | notes |
|---|---|---|
| `"downhill"` (default) | ~94 Mario Kart units/frame | SADX, but his flat-ground speed cap is raised to the speed SADX gives him running down a steep (30°) slope. Everything else is SADX's. |
| `"sadx"` | ~40 | exactly SADX |
| `"kart"` | ~86 | speed and acceleration matched to the karts |

For comparison: the fastest 150cc kart does about 86, a bike in a wheelie about 98, a
mushroom 115+, and the game caps normal speed at 120. With SADX acceleration
(`acceleration = 1`) Sonic needs about 7 s to reach top speed by running, or a third of
a second of spin dash charging.

## Collision

With `collision = "model"` (default) Sonic's body is what collides: a set of spheres,
one per part of his model (head, torso, arms, legs, shoes, quills, or the spin ball),
recomputed every frame so they follow his animation.

- **Karts** bump into his body and he into theirs (their real hitboxes from the game's
  data); karts push him away and he pushes them.
- **Items** (shells, bananas, bombs...) hit him when they touch his body.
- **Course hazards** (Goombas, Thwomps, Chain Chomps...) still use the game's own check
  against the (tiny) kart at his feet, which is close to his body for most hazards.

Under the hood the game still has a kart for him; it is shrunk to `kart_scale` and hidden,
and only follows him. With `collision = "kart"` that kart keeps its full size and does
all the colliding instead (a fallback).

## Options

| key            | default      | meaning |
|----------------|--------------|---------|
| `enabled`      | `true`       | Turn Sonic on or off. Without SADX files nothing changes either way. |
| `assets`       | (search)     | Your Sonic Adventure DX folder, or a SonicAssets folder. Relative paths are relative to `Config.toml`. |
| `base`         | `"mario"`    | The character Sonic plays as under the hood (weight class, item odds, kart sizes, online). English name, file abbreviation or icon name. Players who pick that character themselves are not affected. |
| `physics`      | `"downhill"` | `"downhill"`, `"sadx"` or `"kart"` (see Speed). |
| `speed`        | `1.0`        | Multiplies his speeds. |
| `acceleration` | `1.0`        | Multiplies how hard he pushes off the ground (1 = SADX). |
| `scale`        | `1.0`        | Sonic's size (1 = about Mario's height). Speeds scale with it. |
| `select`       | `"button"`   | `"button"`: the Sonic button. `"base"`: every local player who picks the base character races as Sonic (a fallback for testing). |
| `collision`    | `"model"`    | `"model"` or `"kart"` (see Collision). |
| `kart_scale`   | `0.1`        | Size of the hidden stand-in kart with `collision = "model"`. |
| `model`        | `true`       | Show Sonic in the menus' 3D views (over the base character's model). |
| `icons`        | `true`       | Sonic's portrait in the roster. |
| `name`         | `true`       | The "Sonic" name message. |
| `debug`        | `false`      | Log the grid, race players, item and bump decisions, and every patched file. |

## Nothing is shipped

No Nintendo or Sega data is in this repository. Sonic's model, animations, textures and
physics values come from `system\CHRMODELS_orig.dll` (or `CHRMODELS.dll`),
`system\SONIC.PVM` and the game executable, read by SonicCore (MIT). The portrait is
rendered on your machine. Patched Mario Kart Wii archives are written to your cache
folder (`<app data>/Cache/sonic`), never next to your game files.

## How it works

All in `runtime/src/sonic/`:

- **`sonic_disc_patch.cpp`** (from `DVDInit`, after overlays): patches the UI archives
  (the roster's unused `tt_hammer_*` picture, shown by pane `cha_21_hammer`, becomes
  Sonic's portrait; message 9025 "Sonic" is added to every `Common.bmg`), and
  fingerprints the base character's driver models.
- **`sonic_roster.cpp`**: after `CtrlMenuCharacterSelect::Load`, a 27th `ButtonDriver`
  is built with the game's own `LoadButton` for the base character's index (the button
  array pointer is shifted for that one call), moved to a free grid cell and appended to
  the grid's control group. Clicking it is, to the game, clicking the base character;
  the hooks remember which player used Sonic's button. While the game builds that
  button, its name and its preview, the icon and name lookups
  (`GetCharacterIconPaneName`, `GetCharacterMessageId`) answer with Sonic's.
- **`sonic_race_hook.cpp`**: wraps `Kart::Manager::Update`. For each Sonic player it
  reads the kart (respawn, cannon, finish, boosts, item hits) and the controller, runs
  `SonicRacer`, and writes Sonic's position, rotation and speed back into the kart. It
  also shrinks the kart (`Kart::Movement::UpdateScale`), resolves kart bumps against
  Sonic's body, and answers `Item::Obj::CheckKartCollision` for him.
- **`sonic_racer.cpp`**: SonicCore's SADX physics on the course's KCL collision
  (`sonic_course.cpp`, read from the course archive the game loaded), the physics modes,
  and Sonic's collision body.
- **`sonic_draw_hook.cpp`**: wraps `nw4r::g3d::DrawResMdlDirectly`. In races it skips
  the base character's driver model at Sonic's kart and draws Sonic there, once per
  camera, through the current camera matrix; in the menus it draws Sonic over the base
  character's models while Sonic is picked.

The disc layer never fails the boot, and every hook falls back to the game's own
function when it does not recognise what it is given.

## Limitations

- CPUs never play as Sonic; only local players.
- Online, other players see the base character.
- Sonic's sound effects and voice are not played yet; his kart's engine sound is still
  heard.
- Course hazards use the stand-in kart, not his model (see Collision).
- Results screens and the award ceremony show Sonic only if no other player uses the
  base character.

## Troubleshooting

Turn on `debug = true` and send the log; it lists the grid buttons, the race players,
every item/bump decision near Sonic and every patched file.

- **No Sonic button**: look for `character select:` lines in the log. As a fallback, set
  `select = "base"` and pick the base character.
- **Sonic stands still in races**: look for `course collision` (he needs the course's
  collision) and `race:` lines.
- **"Sonic Adventure DX files not found"**: check `assets`. It must be the folder that
  contains `system\CHRMODELS_orig.dll` (or `CHRMODELS.dll`) and `system\SONIC.PVM`.
- **Items pass through him or hit from too far**: try `collision = "kart"` and send the
  log.
- To force a rebuild of the patched archives, delete `<app data>/Cache/sonic`.
