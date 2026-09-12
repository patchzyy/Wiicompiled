# MetalFX spatial upscaling: renderer integration and tests

Aurora can now upscale the completed game image with MetalFX before aspect-fit
presentation and ImGui composition. It is opt-in and requires macOS 13+, a
supported Metal device, and Dawn IOSurface/shared-event support. Other backends
and builds without the MetalFX SDK use a stub and the existing presentation path.
The MetalFX framework is weak-linked; the game's deployment target is unchanged.

## Trying the renderer integration

Use F10 → Graphics → MetalFX spatial upscaling. Then use the existing
Resolution control to render below the output viewport's size.
Both source dimensions must be smaller than the output dimensions. Equal-size
rendering, supersampling, and unsupported source formats bypass MetalFX. In
particular, Auto (window size) generally offers no upscaling opportunity.

The F10 toggle is saved in `Config.toml` as
`video.metalfx_spatial_upscaling`. It uses these thread-safe Aurora entry points:

- `aurora_set_metalfx_spatial(bool)` requests a change at the next sealed frame.
- `aurora_get_metalfx_spatial()` returns the requested setting.
- `aurora_is_metalfx_spatial_supported()` reports device/build support.
- `aurora_get_metalfx_status()` distinguishes Disabled, Unsupported,
  Not Upscaling, Active, and Error. A busy resize-retirement pool temporarily
  bypasses upscaling and retries on a later frame. Other upscaler errors log a
  reason and use normal presentation until a disabled frame resets the error.

The game's HUD is part of the source image and is upscaled. ImGui/F10/FPS overlays
are composed afterward at output resolution. Existing source-frame captures
still capture the original source image. The interpolation snapshot call sites
all use the same upscaling hook; game-specific interpolation remains untested.

## GPU path and ownership

1. Request Dawn's `SharedTextureMemoryIOSurface` and `SharedFenceMTLSharedEvent`
   features when the Metal adapter supports both. Use that Dawn device's native
   `MTLDevice`, not a separately selected default device.
2. Cache three upscaling slots with IOSurface-backed input and output textures,
   a spatial scaler, a private MetalFX output, and shared-event dependencies.
   Check texture formats, dimensions, usages, and device size limits on creation.
3. Begin Dawn input access, copy the completed game image at its source size,
   and submit the scene plus copy. End input access and wait for Dawn's
   `commandsScheduledFuture` before submitting dependent native Metal work.
4. On the native queue, wait for input rendering and any prior Dawn consumption
   of the shared output. Encode MetalFX into its required **private** output
   texture, then GPU-blit the result into the output IOSurface and signal an event.
5. After native scheduling, begin Dawn output access with that event/value.
   Composite the upscaled image into the existing content viewport, retaining
   letterboxing, then draw ImGui. Submit and end output access. Reuse observes
   both Dawn-to-Metal and Metal-to-Dawn event dependencies.
6. GPU completion callbacks retain resources after a cache entry is replaced,
   disabled, or shut down. At most six resource sets may exist (three current
   plus three retiring); rapid resizing cannot allocate an unbounded queue.

CPU scheduling waits remain, but there are no CPU image transfers or per-frame
GPU-completion waits in the upscaling path. There is one source-size GPU copy
and one full-output GPU blit. Their cost must be measured before promising a
performance gain. The input copy follows the existing perceptual/unorm sampling
path. sRGB texture formats bypass MetalFX to avoid implicit color conversion.

## Standalone GPU regression test

The test builds the actual `lib/webgpu/metalfx.mm` implementation. Point
`Dawn_DIR` at the package used by an existing Aurora build:

```sh
cmake -S aurora-main/tests/metalfx_interop -B build-metalfx-interop \
  -DDawn_DIR="/absolute/path/to/dawn_prebuilt-src/lib/cmake/Dawn" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-metalfx-interop
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
  ctest --test-dir build-metalfx-interop --output-on-failure -V
```

The GPU test returns 77 (CTest **Skipped**) when no Metal adapter, required
sharing features, or spatial scaler is available. A skip is not evidence of
interoperability. Sandboxed processes may need GPU access. CTest imposes a
60-second timeout. The separate stub test needs no GPU.

Tested on Apple M3, macOS 26.5.1, using Aurora's existing Dawn package
(`v20260603.191052`). Metal API and GPU validation were enabled:

| Formats | Input | Output | Frames per format |
| --- | --- | --- | --- |
| RGBA8Unorm, BGRA8Unorm | 64 × 48 | 128 × 96 | 24 |
| RGBA8Unorm, BGRA8Unorm | 320 × 180 | 480 × 270 | 24 |
| RGBA8Unorm, BGRA8Unorm | 960 × 540 | 1920 × 1080 | 24 |

All 144 frames and 2,304 interior pixel samples passed. Red changes per frame;
green and blue distinguish left/right and top/bottom. All channels are checked
within five 8-bit levels, catching stale images, orientation/channel mistakes,
and missing output. Tests cover 1.5× and 2× scaling, padded readback rows, slot
reuse, dropping wrappers before readback completion, invalid dimensions/sRGB
formats, the six-set allocation bound, and the unavailable-backend stub.

Readback is only the test oracle and is absent from the game upscaling path.
These samples do not measure reconstruction quality at edges or race performance.

## Windowed presentation smoke test

This optional target exercises Aurora's actual frame submission and presentation
with a synthetic source and an ImGui overlay. It requires no Wii game data and
creates an automatically closing test window. Add the option to an existing
from-source runtime build (the normal dependency/provider options still apply):

```sh
cmake -S runtime -B build-macos -DCMAKE_BUILD_TYPE=Release \
  -DAURORA_BUILD_METALFX_SMOKE_TEST=ON
cmake --build build-macos --target metalfx_presentation_smoke
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
  ./build-macos/aurora-build/metalfx_presentation_smoke
```

On the same M3, all 84 frames passed with Metal API/GPU validation: disabled,
enabled, window resize, 4:3/16:9 aspect changes, native-size bypass, disable, and
re-enable. Assertions check renderer status and errors; this is not a pixel-level
verification of the window image. The core build and macOS 12 deployment-target
availability compilation also passed. Full Mario Kart gameplay, race performance,
visual quality, Intel Macs, other Apple GPUs, older macOS runtime versions, and
non-macOS full builds remain untested.

References: [Apple MetalFX](https://developer.apple.com/documentation/metalfx),
[spatial scaler requirements](https://developer.apple.com/documentation/metalfx/mtlfxspatialscaler),
and the installed Dawn `MetalBackend.h` / `webgpu_cpp.h` APIs.
