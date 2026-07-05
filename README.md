# Pararena 2.0
Sources to Pararena, the commercial Macintosh game by John Calhoun, published by Casady &amp; Greene, Inc.

![Sample Box Art](https://github.com/softdorothy/pararena_2/blob/master/Misc/Pararena%20Sample%20Art.png)

## Playing it today

A native SDL3 port for modern macOS, Linux, and Windows — with gamepad
support and fullscreen — lives in [`port/`](port/). It compiles the
original game code in `Sources/` verbatim over a small Toolbox shim and
uses the original art/sound/physics data extracted from
`Pararena.project.r`. See [`port/README.md`](port/README.md) to build and
play, and [`MODERNIZATION.md`](MODERNIZATION.md) for the engineering
evaluation behind the port.


