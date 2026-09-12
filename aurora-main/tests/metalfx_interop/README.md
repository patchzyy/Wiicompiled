# MetalFX / Dawn interoperability probe

This standalone macOS test proves the first step of MetalFX spatial upscaling
integration. It does not enable upscaling in the game or change its settings,
renderer, dependencies, or normal build. It requires macOS 13+, an Xcode SDK
containing MetalFX, and Aurora's existing Dawn package.

## Build and run

From the repository root, point `Dawn_DIR` at the package used by an existing
Aurora build (the path may also point into another checkout):

```sh
cmake -S aurora-main/tests/metalfx_interop -B build-metalfx-interop \
  -DDawn_DIR="/absolute/path/to/dawn_prebuilt-src/lib/cmake/Dawn" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-metalfx-interop
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
  ctest --test-dir build-metalfx-interop --output-on-failure -V
```

The test returns 77 (CTest **Skipped**) when no Metal adapter is available,
the adapter lacks the required Dawn sharing features, or MetalFX spatial
scaling is unsupported. A skipped test is not evidence of interoperability.
Sandboxed processes may not see the GPU; run with GPU access for validation.
CTest imposes a 60-second timeout.

## GPU handoff

1. Obtain the native `MTLDevice` from the selected Dawn device. Request
   `SharedTextureMemoryIOSurface` and `SharedFenceMTLSharedEvent` at device
   creation; do not select an unrelated default Metal device.
2. Create IOSurface-backed input and output textures, each imported into Dawn
   with `ImportSharedTextureMemory`. Check dimensions, format, and usages.
3. Begin Dawn input access, render a changing asymmetric pattern, submit,
   then `EndAccess`. Export its Metal shared-event fences and wait for the
   `commandsScheduledFuture` before submitting a dependent Metal command buffer.
4. On the native Metal queue, wait for input rendering and any previous Dawn
   use of the output. Encode MetalFX from the input IOSurface into a **private**
   texture. Blit that result into the output IOSurface and signal a shared event.
5. Commit and wait for Metal scheduling, then begin Dawn output access with
   that event/value. Consume the result with a Dawn texture-to-buffer copy and
   end access, retaining the returned dependencies for output reuse.
6. Reuse three slots for 24 frames per case. Each slot's next Dawn input write
   waits for its previous Metal consumption. Each next Metal output write waits
   for its previous Dawn consumption. Scalers and image allocations are cached
   per slot; readback buffers and uniforms are test-only allocations per frame.

There are no CPU image copies between render, upscale, and Dawn consumption.
CPU scheduling waits remain; they do not wait for GPU frame completion. Final
buffer mapping and completion checks are the test oracle, after all frames have
been submitted, and are not part of the proposed game path.

## Verified result

Tested on Apple M3, macOS 26.5.1, using the existing Aurora Dawn package
(`v20260603.191052` in the upstream build configuration). Both Metal API and
Metal GPU validation were enabled. All six cases passed without reported Dawn
errors or Metal command-buffer failures:

| Formats | Input | Output | Frames per format |
| --- | --- | --- | --- |
| RGBA8Unorm, BGRA8Unorm | 64 × 48 | 128 × 96 | 24 |
| RGBA8Unorm, BGRA8Unorm | 320 × 180 | 480 × 270 | 24 |
| RGBA8Unorm, BGRA8Unorm | 960 × 540 | 1920 × 1080 | 24 |

This checks 144 frames and 2,304 interior pixel samples. Red varies per frame;
green and blue distinguish left/right and top/bottom quadrants. All four
channels are checked within five 8-bit levels. This detects stale images,
orientation/channel mistakes, and missing output. It exercises 1.5× and 2×
scaling, padded readback rows, resource reuse, and resource recreation between
cases. It does not assess reconstruction quality near edges or game performance.

## Consequences for integration

- The current Dawn package supports the required route on the tested M3;
  this prototype needs no Dawn upgrade or custom native backend patch.
- MetalFX requires a private output texture. The IOSurface return path therefore
  costs one full-output GPU blit. The existing game image would also need a GPU
  copy/render into the shared input unless its producer targets that image
  directly. Measure those costs before making performance claims.
- Use the completed game image before aspect-fit presentation and ImGui
  composition. Upscale to the content viewport size, then preserve letterboxing
  and draw the settings overlay at output resolution. The game's own HUD is
  already part of the source image.
- The probe uses non-sRGB texture formats containing perceptual color values.
  Integration must audit the game's copy/gamma path to avoid double conversion.
- Keep each in-flight slot's textures, scalers, fences, and values alive through
  GPU use. Production needs bounded resource retirement on resize/toggle and
  device-loss handling; this finite probe drains work before destroying a case.
- Require OS, device, and Dawn feature checks in the game, with weak framework
  linkage/runtime availability and fallback to existing presentation. Intel
  Macs, other Apple GPUs, older supported macOS versions, live resizing, game
  frame interpolation, and actual race performance remain untested.

References: [Apple MetalFX](https://developer.apple.com/documentation/metalfx),
[spatial scaler requirements](https://developer.apple.com/documentation/metalfx/mtlfxspatialscaler),
and the installed Dawn `MetalBackend.h` / `webgpu_cpp.h` APIs.
