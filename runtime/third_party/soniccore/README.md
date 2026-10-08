# SonicCore (vendored)

Sonic Adventure DX's Sonic as a reusable C++17 module (MIT, see `LICENSE`). Only the
library sources (`src/soniccore`) are vendored here; the standalone test level, the GL
demo renderer and the `sonic_extract` tool live in the SonicCore project itself.

It contains no Sega data. At runtime it reads the player's own Sonic Adventure DX files:
`CHRMODELS_orig.dll` (or `CHRMODELS.dll`), `SONIC.PVM`, `SON_EFF.PVM` and two sound banks.

Local addition for WiiCompiled: `CharacterModel::actionSkeleton/skeleton/actionLocals/drawLocals`
(compose custom poses from several actions), used by `runtime/src/sonic/sonic_render.cpp`.
