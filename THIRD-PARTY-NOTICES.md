# Third-party notices

SpaceWindow is free software under the GNU General Public License, version 2
or (at your option) any later version ([LICENSE.txt](LICENSE.txt)). It
contains code from the following projects, under their own licenses, which
allow it; their notices are kept here.

## SpaceMonger 1.4.0

SpaceWindow is based on SpaceMonger 1.4.0 by Sean Werkema
([github.com/seanofw/spacemonger1](https://github.com/seanofw/spacemonger1)):
its treemap layout, colour schemes, tips and behaviour.

```
MIT License

Copyright (c) 1998-2020 Sean Werkema

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## XaoS

The **Mandelbrot Set** and **Julia Set** animations (`src/Core/Fractal.h`,
`Fractal.cpp`) are a port of XaoS's zooming engine, formulas, palettes and
autopilot ([github.com/xaos-project/XaoS](https://github.com/xaos-project/XaoS)).

```
XaoS, a fast portable realtime fractal zoomer
Copyright (C) 1996,1997 by Jan Hubicka and Thomas Marsh, and the XaoS
developers

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the Free
Software Foundation; either version 2 of the License, or (at your option)
any later version.
```

## Background animations

Apart from the fractals (above), the background animations were written for
SpaceWindow; these are where they come from.

- **DOOM Fire** follows the PSX DOOM fire as described by Fabien Sanglard in
  ["How DOOM fire was done"](https://fabiensanglard.net/doom_fire_psx/),
  including its 37 colours, and as played in a terminal by
  [kiedtl/fire](https://github.com/kiedtl/fire) (MIT License) and the
  [ly](https://github.com/fairyglade/ly) display manager (WTFPL).
- **Colour Mix** is a port of the folding pattern, and takes the red, blue and
  black, of ly's `colormix` animation (WTFPL); **Matrix** and **Game of
  Life** follow its `matrix` and `gameoflife` animations.

## Dracula Theme

The colours of SpaceWindow's built-in **Dracula** and **Alucard** themes are
the palettes of the Dracula Theme:
[draculatheme.com](https://draculatheme.com),
[github.com/dracula/dracula-theme](https://github.com/dracula/dracula-theme).

```
The MIT License (MIT)

Copyright (c) 2023 Dracula Theme

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
