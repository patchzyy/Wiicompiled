# Optional native Linux Wii Remote support

`MKW_ENABLE_XWIIMOTE` is OFF by default. Enabling it adds a libxwiimote transport
for kernel-managed original Nintendo RVL-CNT-01 remotes (`gen10`). SDL HIDAPI
remains available. Keep the Linux `hid_wiimote` driver loaded; this backend uses
its input interfaces rather than taking ownership of the Bluetooth HID channels.

The backend does not supply pairing software, a Bluetooth daemon, a virtual HID
device, or input injection. Pair and connect a genuine remote normally first.
The game's existing optional pairing UI is unchanged; testing below never uses it.
No device addresses or host-specific paths are built into the implementation.

## Dependencies and building

In addition to the normal Linux runtime build requirements, install libxwiimote
headers and pkg-config (Ubuntu: `libxwiimote-dev` and `pkg-config`). CMake requires
`libxwiimote >= 2` and reports the detected version. This implementation was
validated with API version `2`, Ubuntu package `2-4.1build1`. See
[THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md) for licensing details.

Follow the normal translation/product build instructions, adding:

```sh
-DMKW_ENABLE_XWIIMOTE=ON
```

Native-enabled builds compile the runtime from source, avoiding an incompatible
precompiled runtime archive. The option participates in its content fingerprint.
Official pinned third-party dependencies (including upstream's Dawn distribution)
are still obtained through the normal build. No experimental objects are needed.
The option is rejected on non-Linux systems. With it disabled, neither pkg-config
nor libxwiimote is required by this feature, and native transport sources are not
included in the production runtime archive.

## Input, outputs and guest protocol

Each connected kernel device is offered to an available Wii player slot, sharing
the existing four-slot limit with SDL. Bluetooth addresses read from kernel
HID_UNIQ identify devices for duplicate suppression and remembered slots; they
are not pairing credentials. A returning native remote waits for its previous
guest link to go down before reusing that slot. Discovery order determines the
initial assignment; this feature does not add a player-assignment UI.

Core buttons keep the Wii report bit layout and the game's native sideways
orientation. Acceleration events become mode-0x31-compatible samples, including
the low bits interleaved with button fields. Fast button press/release transitions
are delivered separately instead of being merged into a motion sample.

Kernel events cannot answer WPAD initialization by themselves. The local protocol
adapter supplies status (0x20), memory responses (0x21), acknowledgements (0x22),
reporting modes, nominal calibration blocks/checksums, and bounded volatile
configuration storage. Command replies take priority over continuous input samples;
a command-reply overflow fails the native connection instead of silently losing
initialization replies. Guest link setup resets protocol state but retains the
current physical button/acceleration state.

Rumble uses libxwiimote force feedback when writable input access is available.
LED state is emulated for the guest and separately written through libxwiimote.
An LED sysfs EACCES is logged as a host permissions failure; it does not become an
invented Wii protocol error that prevents WPAD initialization. On release, modified
LEDs are restored where permitted and rumble started by this backend is stopped.
Native shutdown releases kernel interfaces without powering off the OS connection.

The existing SDL mask, queue-overflow and reconnect behavior are retained. The
native transport selects its own Wii-byte-order button mask and reply-queue policy;
correcting unrelated SDL behavior is outside this contribution.

## Compatibility limits

- Original RVL-CNT-01/gen10 only; RVL-CNT-01-TR/Remote Plus are rejected.
- Linux x86_64 was validated locally. AArch64 is permitted by the existing build
  but has not been validated for this backend. Windows/macOS retain SDL; their
  hosted CI jobs have not been executed as part of this local preparation.
- Requires both readable core and accelerometer interfaces. Writable force-feedback
  access and LED sysfs permissions are independent. No chmod, ACL or udev change is
  performed automatically; diagnose the exact denied operation.
- Calibration is nominal (zero 512, one g 616), not the remote's factory EEPROM.
  Local volatile memory is generated state, not a physical EEPROM dump.
- Extensions and MotionPlus are not exposed; extension probes return protocol
  errors and extension-present remains clear. IR reports contain no tracked dots;
  speaker data is discarded. Physical initialization stays owned by the kernel.
- Four guest slots are supported by the manager and covered for capacity offline;
  four-controller physical gameplay is not established by these tests.
- The source implementation has prior operator-confirmed genuine-remote gameplay
  in local base-game and Retro Rewind builds. This extracted contribution branch
  has been validated offline, without a new game or hardware run.

## Reproducible offline checks

A platform-only build needs no graphics dependencies or game data and exercises
protocol/HLE tests. Omit `MKW_ENABLE_XWIIMOTE` to verify the default-disabled path:

```sh
cmake -S runtime -B build-platform -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DMKW_BUILD_PRODUCTS=OFF -DMKW_PLATFORM_TESTS_ONLY=ON
cmake --build build-platform --parallel 3
ctest --test-dir build-platform --output-on-failure
```

The full source-runtime and production controller-manager checks need the normal
Linux graphics/audio development packages and pinned Aurora submodules, but no
translated game sources, ISO, mod assets or Bluetooth device:

```sh
git submodule update --init --recursive
cmake -S runtime -B build-native -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DMKW_BUILD_PRODUCTS=OFF -DMKW_ENABLE_XWIIMOTE=ON \
  -DMKW_BUILD_WIIMOTE_RUNTIME_PROBE=ON \
  -DMKW_NATIVE_PREBUILT_EXPORT_DIR=build-native-export
cmake --build build-native --parallel 3 --target \
  mkw_runtime mkw_wiimote_runtime_probe mkw_wiimote_runtime_probe_sdl \
  mkw_wiimote_native_protocol_tests mkw_wiimote_xwiimote_tests \
  mkw_wiimote_native_manager_tests mkw_bluetooth_controller_tests
ctest --test-dir build-native -R 'wiimote|bluetooth_controller' --output-on-failure
```

Repeat in a separate build directory with `MKW_ENABLE_XWIIMOTE=OFF`, omitting the
native adapter/manager and secondary SDL probe targets. The ordinary production
probe then verifies SDL-only compilation and linkage. CI adds a Linux native-enabled
job; existing default-disabled Windows/macOS/Linux jobs retain their configuration.

The ABI fake exercises actual adapter code with synthetic events: every core
button, three-axis packing, quick taps, status and initialization acknowledgements,
permission failures, output cleanup, invalid identity/type, and interface removal.
The production manager test uses independent synthetic devices for separate button
and motion streams, overlay handling, duplicate suppression, slot reconnection and
capacity. HLE tests drive the guest Bluetooth initialization exchange. No CTest
command performs live discovery, pairing, physical input reads or output writes.

## Optional manual hardware probe

This is separate from CTest and requires deliberate operator action. Run the
probe as the normal desktop user, after connecting a genuine remote and granting
that user the appropriate input-device access through normal system policy.
Create `portable.txt` beside the executable first; without it the probe refuses a
live run. It writes only that portable build's configuration and disables its
automatic pairing, runs for twenty seconds, and prints button/motion counters.
`--describe`, `--expect-sdl` and `--expect-xwiimote` exit before initialization.
Do not treat a successful probe as proof of in-game steering or factory-accurate
calibration. `MKW_XWIIMOTE_TRACE=1` enables guest-facing report diagnostics; review
logs for device identities before sharing them.
