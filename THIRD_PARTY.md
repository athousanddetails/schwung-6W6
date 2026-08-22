# Third-party code

6W6 is licensed GPL-3.0 (see `LICENSE`). It incorporates the following.

## 606-Inspired-Synth-Drums — MIT

<https://github.com/analogcode/606-Inspired-Synth-Drums>
Copyright (c) 2026 Matthew Fecher / AudioKit Pro.

The drum voices. Vendored **unmodified** in `src/vendor/606`, licence text at
`src/vendor/606/LICENSE.MIT`. Every sound 6W6 makes starts in these files:
`BassDrum.hpp`, `Snare.hpp`, `Toms.hpp`, `HiHats.hpp`, `Clap.hpp` and the
shared DSP in `SynthDrumCommon.hpp`.

MIT is GPL-3.0-compatible, so the combined work ships under GPL-3.0 while
these files remain MIT in their own right. Do not edit them — changes belong
upstream.

What 6W6 adds around them: the cymbal the repo does not ship
(`src/dsp/sd606_cymbal.h`, a third `HiHatSpec` for the same
`MetalHiHatVoice`), the post-voice drive and distortion stage, the master
stage, hi-hat choke, accent, kit balance, and the Schwung plugin wrapper.

## 9W9 — GPL-3.0

<https://github.com/athousanddetails/schwung-9W9>

The module architecture: the plugin_api_v2 wrapper shape, the pad and note
maps, silent-select, per-lane mutes, pad-follow and the step sequencer are
ported from 9W9 so the two kits behave identically under the hands. The
distortion flavours in `src/dsp/sd606_shape.h` come from its `er99_circuit.h`.
