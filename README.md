# virtualdisplay

Makes a fake 240Hz screen on macOS and mirrors your real one onto it, so Roblox (or anything else) can run past your display's refresh rate. This is what [Voidstrap](https://github.com/KloBraticc/Voidstrap) uses on macOS.

Forked from [Froststrap/virtualdisplay](https://github.com/Froststrap/virtualdisplay).

## How it works

It uses the private `CGVirtualDisplay` API to make a screen the same size as yours with 60, 120 and 240Hz modes, mirrors your real screen onto it, then moves it to `(0,0)` so macOS treats it as the main display. Apps sync to that one instead of your real screen.

If macOS undoes any of that it just sets it back up again. When you close it everything goes back to how it was.

## Using it

There are 4 C functions:

- `init(name, width, height, rates, count)` sets the name, size and refresh rates. Pass 0 for width and height to use your screen's size.
- `start_display()` makes the screen.
- `is_running()` returns 1 if it's up.
- `end_display()` removes it and puts your displays back.

Everything except `init` runs on the main queue, so your app needs a main run loop.

## Building

It builds inside Voidstrap's workspace:

```sh
git clone --recursive https://github.com/KloBraticc/Voidstrap
cd Voidstrap/src/VirtualDisplay
cargo build --release -p virtualdisplay
```

You need a Mac and Rust.

## License

[MPL 2.0](LICENSE.txt)
