# quiet-shelf

A small C build experiment. The program only prints a short message and exits.

## Build

```sh
cmake -S . -B build
cmake --build build
```

## Run

```sh
./build/quiet_shelf
```

On Windows, use the executable path produced by your CMake generator (for
example `build\Debug\quiet_shelf.exe`).
