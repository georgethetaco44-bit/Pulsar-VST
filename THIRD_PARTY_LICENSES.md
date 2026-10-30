# Third-party dependencies (SPEC §10.4). All permissive; JUCE (AGPLv3) is the
# only copyleft dependency. Keep this file current as dependencies change.

## JUCE 8 — AGPLv3 (or commercial)
Fetched via CPM, pinned to tag 8.0.15 (see CMakeLists.txt).
Upstream: https://github.com/juce-framework/JUCE
The plugin/Standalone link it; all Pulsar binaries are therefore AGPLv3,
matching the repo LICENSE.

## WDL / EEL2 — zlib-style licence (Cockos / Nullsoft)
Vendored subset in `third_party/WDL` (see `third_party/WDL/VENDORED.md` for
the exact pin and file list). EEL2 files carry this header text:

> Copyright (C) 1999-2003 Nullsoft, Inc. / Copyright (C) 2004-2013 Cockos
> Incorporated (per file; aarch64 glue and later asm changes are
> Copyright (C) Cockos Incorporated, same terms).
>
> This software is provided 'as-is', without any express or implied
> warranty. In no event will the authors be held liable for any damages
> arising from the use of this software.
>
> Permission is granted to anyone to use this software for any purpose,
> including commercial applications, and to alter it and redistribute it
> freely, subject to the following restrictions:
>
> 1. The origin of this software must not be misrepresented; you must not
>    claim that you wrote the original software. If you use this software
>    in a product, an acknowledgment in the product documentation would be
>    appreciated but is not required.
> 2. Altered source versions must be plainly marked as such, and must not be
>    misrepresented as being the original software.
> 3. This notice may not be removed or altered from any source distribution.

## ysfx x86-64 GAS conversion — zlib (file header notes Apache-2.0 project)
`third_party/WDL/WDL/eel2/asm-nseel-x64-sse.S` is ysfx's GAS rewrite of
WDL's `asm-nseel-x64-sse.asm`, ported onto the pinned WDL (port deltas are
documented in `third_party/WDL/VENDORED.md`). Its in-file header states:
Copyright (C) 2005 and later Cockos Incorporated, Copyright (C) 2021 and
later Jean Pierre Cimalando, under the same zlib-style terms as above
(SPDX-License-Identifier: Zlib). The ysfx project as a whole is Apache-2.0,
but this single file's header grants zlib terms, so it is kept verbatim.

## DaisySP — MIT (Electro-smith)
Fetched via CPM, pinned to commit 2c72eaf9eac5fc0dca1919d65d606da907832618
(see CMakeLists.txt). Upstream: https://github.com/electro-smith/DaisySP

Only MIT-repo modules are built into `daisysp_mit` (Oscillator, Svf,
LadderFilter, Adsr, Decimator, Chorus/ChorusEngine, Resonator,
KarplusString `String`, plus their header-only helpers and
Dynamics/crossfade + Utility/dcblock for the string).
`DaisySP-LGPL` is never compiled or linked (we fetch with CPM
`DOWNLOAD_ONLY` — CPM 0.42.1 does not forward `GIT_SUBMODULES`, so the
submodule directory may exist inertly in the CPM cache — and build only
the whitelisted MIT sources below into `daisysp_mit`; a configure-time
assertion fails the build if the LGPL classes ever enter the tree), so
lexicon entry uses MIT-repo `LadderFilter` and the master bus reverb is
our own FDN.

`Source/Filters/ladder.h` carries its own in-file attribution
(Huovilainen New Moog model, via Teensy's Audio library and Infrasonic
Audio LLC) under the same permissive terms; the vendored headers' licence
text is authoritative.

## CPM.cmake — MIT (Lars Melchior et al.)
`cmake/CPM.cmake` is the stock CPM distribution file (MIT), with its
licence text at the top of the file.
