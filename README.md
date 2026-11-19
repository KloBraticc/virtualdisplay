# virtualdisplay

Makes a fake high refresh rate screen on macOS (240 Hz by default) and mirrors your real one onto it, so Roblox (or anything else) can run past your display's refresh rate. This is what [Voidstrap](https://github.com/KloBraticc/Voidstrap) uses on macOS.

Forked from [Froststrap/virtualdisplay](https://github.com/Froststrap/virtualdisplay).

## How it works

It uses the private `CGVirtualDisplay` API to make a screen the same size as yours with 60, 120 and 240 Hz modes by default, mirrors your real screen onto it, then moves it to `(0,0)` so macOS treats it as the main display. Apps sync to that one instead of your real screen.

If macOS undoes any of that it just sets it back up again. When you close it everything goes back to how it was.

## Using it

The library is plain C, declared in [`src/virtualdisplay.h`](src/virtualdisplay.h):

- `vd_configure(name, width, height, rates, count)` sets the name, size and refresh rates. Pass 0 for width and height to use your screen's size. Returns 0, or -1 when a value is out of range.
- `vd_start()` makes the screen.
- `vd_running()` returns 1 while it is up, and 0 once it stops or macOS removes it.
- `vd_stop()` removes it and puts your displays back.

Every call runs on the main thread, so your app needs a main run loop. Display changes only last while your process runs, so a crash never leaves the arrangement moved.

## Building

```sh
clang -c -std=c11 -O2 -fblocks src/virtualdisplay.c -o virtualdisplay.o
```

Link it with `-framework CoreGraphics -framework Foundation -lobjc`. Voidstrap builds it inside its own helper. You need a Mac.

## License

[MPL 2.0](LICENSE.txt)
