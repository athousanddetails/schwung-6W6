# 6W6 — Drumatix for Ableton Move

A TR-606 style drum machine for [Schwung](https://github.com/charlesvestal/schwung)
on Ableton Move. Every voice is synthesized — no samples — and the kit is
fitted against recordings of a real TR-606, hit for hit. Plus a clap, because
a spare pad is a spare pad.

![6W6 on the Move](docs/img/device-ui1.png)
![6W6 editor pages](docs/img/device-ui2.png)
![6W6 on the device](docs/img/device-ui3.png)

## Voices

| Voice | Engine |
|---|---|
| Bass Drum | Swept sine body, filtered noise and impulse transients — short and clicky like the 606, not the long 808-style tail |
| Snare | Tuned shell plus two bandpass-shaped noise "wires"; Snappy moves only the wires |
| Low / Hi Tom | Main mode with quieter resonances around it; the low tom falls into pitch, the high tom leaves a lower ring |
| Closed / Open Hat | One metal bank of 32 measured partials — fitted from a hardware 606's hats — under two envelopes, like the circuit |
| Cymbal | The same metal source with its own 32-line table measured off a hardware cymbal, down to 266 Hz where a hat's table stops at 3.7 kHz |
| Hand Clap | Four timed noise bursts and a diffuse tail (the RD-6 fit from the source repo; the 606 never had one) |

Every voice has **Tune, Decay, Drive, a Distortion type** (Diode / Hard Clip /
Wavefolder / Bitcrush) and **Level**, plus a **Master Drive / Distortion**
across the kit. Every continuous control is a **0–127 pot**; centre is the
fitted 606. **Hat choke is a switch**: Off, CH cuts OH (the 606's wiring), or
Mutual.

## Workflow on the Move

- **Pads (left 4×4)** play and select drums; the parameter page follows what
  you hit. Row 1: BD SD LT HT. Row 2: CH OH CY CP. **Shift+Pad** selects
  silently (works during playback). **Mute+Pad** mutes that drum (`[M]` in the
  title bar). Pad 16 opens **Master**.
- **Knobs 1–8** edit the visible page, drawn with Schwung's stock knob grid
  (host 0.12.1+): **jog** cycles pages, **Shift+Jog** jumps sections, **jog
  click** opens the section list, **Shift** reveals values / fine mode,
  **Mute+knob** resets a pot to its fitted default.
- **Sequencing:** use Move's own sequencer — a drum track with a kit, muted
  (HiJack), track MIDI OUT on the slot's channel. Each drum is its own lane.
  Note map: drum rack (36–43, default) or General MIDI, switchable.
- Works with [Movy](https://github.com/DimaDake/schwung-movy) — a
  `movy_config.json` ships with the module.

## Remote panel

A TR-606-style panel in the browser — level knobs across the top, an
INSTRUMENT selector, the selected drum's controls, and a live sixteen-step
strip that edits the slot's built-in step lanes and follows the playhead.
Open `move.local:7700/remote-ui` while 6W6 is the slot's synth.

![6W6 remote panel](docs/img/remote-ui.png)

## Install

Requires Schwung **0.12.1 or newer**. Via the Schwung Module Store /
[schwung-manager](https://github.com/charlesvestal/schwung), or manually:
build, then copy `dist/6w6/` to
`/data/UserData/schwung/modules/sound_generators/6w6/` on the device.

## Building

Requires Docker (cross-compiles for the Move's ARM64, pinned to glibc 2.35):

```bash
./scripts/build.sh all            # builds build/dsp.so + dist/6w6-module.tar.gz
./scripts/deploy.sh move.local    # safe deploy (atomic rename, never over a live .so)
```

`scripts/build.sh` also builds `sd606_loadtest`, an on-device test that
dlopens the real `dsp.so` exactly as Schwung's chain host does and checks
that every pad sounds, that pots change the audio, the hat choke, mutes,
state round-trip and the sequencer — end to end.

## Credits and provenance

6W6 stands on other people's work and says so:

- **[606-Inspired-Synth-Drums](https://github.com/analogcode/606-Inspired-Synth-Drums)**
  by Matthew Fecher / AudioKit Pro (MIT) — the voices. Vendored unmodified
  under `src/vendor/606`; every sound 6W6 makes starts there. The cymbal the
  repo does not ship, the hat bank from a second unit, the drive and
  distortion stages, hat choke and accent are 6W6's additions around it.
- **[9W9](https://github.com/athousanddetails/schwung-9W9)** — the module
  architecture, pad gestures and the four distortion flavours, so the two
  kits feel identical under the hands.
- **Schwung** by Charles Vestal and contributors — the platform and the
  shared `param_pages` knob grid; **Movy** by DimaDake for the page model.

Built with AI assistance. GPL-3.0; see `LICENSE` and `THIRD_PARTY.md`.
