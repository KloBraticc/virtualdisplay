# virtualdisplay

Creates a virtual 240Hz display on macOS and mirrors your main screen to it,
so games and other apps can render above the refresh rate of the physical
display. It powers the macOS virtual display in
[Voidstrap](https://github.com/KloBraticc/Voidstrap).

Forked from [Froststrap/virtualdisplay](https://github.com/Froststrap/virtualdisplay).

## How it works

Uses macOS's private `CGVirtualDisplay` API to create a virtual monitor at
your main screen's resolution with 60, 120 and 240Hz modes, then configures
the system to mirror your physical display to it via
`CGConfigureDisplayMirrorOfDisplay`.

It also moves the virtual display to origin `(0,0)` with
`CGConfigureDisplayOrigin`, which is how macOS designates the main display,
so apps vsync against the virtual display instead of the physical one. Both
steps are verified and retried until they stick, and rolled back on teardown.

Setup is driven by display reconfiguration events, with a 100ms check while
the display settles. Once the virtual display is mirrored and main, nothing
polls, and a later reconfiguration that breaks either step sets it up again.

## API

The crate builds as both a `cdylib` and an `rlib`, and exports four C
functions. `start_display`, `is_running` and `end_display` run on the main
dispatch queue, so the host process has to run a main run loop.

| Function | Returns |
| --- | --- |
| `init(name, width, height, rates, count)` | `0`, or `-1` when no refresh rates or more than 255 are passed. Pass `0` for width and height to match the main screen |
| `start_display()` | `0` once created or already running, `-1` if `init` was not called, a refresh rate is invalid, or macOS refused the display |
| `is_running()` | `1` while the display exists, otherwise `0` |
| `end_display()` | `0` after removing the display and restoring the original arrangement |

## Build

The crate takes its edition, license and dependency versions from a Cargo
workspace. Voidstrap provides that workspace in `src/VirtualDisplay`, which
includes this repository as a submodule:

```sh
git clone --recursive https://github.com/KloBraticc/Voidstrap
cd Voidstrap/src/VirtualDisplay
cargo build --release -p virtualdisplay
```

This requires macOS and Rust.

## License

[Mozilla Public License 2.0](LICENSE.txt)
