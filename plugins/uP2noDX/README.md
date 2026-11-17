# uP2noDX (test build 0.1)

A 6-operator FM synth for Akai MPC standalones by **uP2noGood**, compatible with DX7 voice data: 32 algorithms, 16 voices, and it plays DX7 cartridge files (.syx). The sound engine is **msfa**, the DX7-accurate engine also used inside Dexed (Apache License 2.0, see `src/msfa/README.md`).

"DX7" is a trademark of Yamaha Corporation. This project is not affiliated with Yamaha, and no Yamaha patches or ROM data are included.

## The classic patches

Put DX7 32-voice bank files (`*.syx`) in `Content/Cartridges/` and rebuild. Every voice of every bank appears in the MPC's preset list after the uP2noGood presets, named `<file name> <voice name>`. Files load in name order, so name them e.g. `01 ROM1A.syx`, `02 ROM1B.syx`.

The `.syx` files are your own and are **never committed to git** (`.gitignore`); they live only on your computer and your MPC.

Tested: a generated 32-voice bank in the standard DX7 bulk format loads with all 4,640 values exact.

## Pages (function keys)

| Key | Page(s) | Knob banks (one unit per bank) |
|---|---|---|
| 1 | **PANEL**: algorithm chart with the current algorithm lit, big algorithm diagram, LCD/LED, VOICE and LFO controls, operator strips (on/off, level, ratio), MONO / PORTA / OSC SYNC / MK I buttons | VOICE (Algorithm, Feedback, Transpose, Volume) · LFO (Speed, Delay, Pitch Mod, Amp Mod) · OP 1-4 levels · OP 5-6 levels |
| 2 | **OP 1 … OP 6** (press again for the next operator) | Rates R1-R4 · Levels L1-L4 · Frequency (Mode, Coarse, Fine, Detune) · Output (Level, Velocity, AM Sens, Rate Scaling) |
| 3 | **SCL 1 … SCL 6**: keyboard level scaling | Break point, Left depth, Left curve · Right depth, Right curve · Operator on/off |
| 4 | **MOD** | Pitch EG rates · Pitch EG levels · LFO (Wave, Speed, Delay, Sync) · LFO depths (PMD, AMD, Pitch Sens, Osc Sync) |
| 5 | **VOICE** | Voice (Algorithm, Feedback, Transpose, Volume) · Play (Poly/Mono, Bend, Porta On, Porta Time) · Mod wheel (Range, >Pitch, >Amp, >EG) · Aftertouch (Range, >Pitch, >Amp, >EG) |

All values use the DX7's own ranges (0-99 etc.). Coarse shows the frequency ratio (e.g. x14.00), the break point shows a note name.

## Built-in presets

INIT VOICE · uP TINES · uP BASS · uP BELLS · uP BRASS · uP MARIMBA · uP PAD (our own voices).

## Playing

- 16 voices, oldest-note stealing; sustain pedal (CC 64).
- MONO: one voice, the last held key wins; with PORTA on it glides between notes (also in poly).
- Mod wheel and aftertouch: set their range and targets (pitch / amp / EG bias) on the VOICE page. Default: wheel to pitch (vibrato through the LFO).
- **MK I**: emulates the original's output converter (12-bit DAC with a x1/x2/x4/x8 range switch) for its slightly gritty top end. The original's 49,096 Hz sample rate is not emulated in this build.
- MIDI events are applied in 64-sample steps (up to 1.5 ms), which is how the engine works.

## Test results (host simulator)

All checks pass. In tune to 0.01 Hz from C2 to C6. 16 voices ≈ 1.2 % of a desktop core; check on the MPC. Levels: one note -10 to -20 dBFS, 8-note chords up to about 0 dBFS (a soft limiter catches dense chords).

## What to check on the MPC

1. CPU with 16 voices on the Key 37.
2. Do the classic banks sound right (drop a few in `Content/Cartridges/`)?
3. PANEL: does the algorithm chart highlight and the big diagram follow the Algorithm knob? Readability of the small chart.
4. Do the OP and SCL keys cycle through the six operators?

## Rebuilding the skin

`skins/` is generated (needs Python with Pillow). After changing parameters or `ui.json`:

```
python3 tools/build.py plugins/uP2noDX --target native
python3 plugins/uP2noDX/skin_tools/make_skin.py
python3 tools/build.py plugins/uP2noDX
```

## Building

```
python3 tools/build.py plugins/uP2noDX
```

Install over the network (developer mode on): `python3 tools/mpc_remote.py <MPC IP> update plugins/uP2noDX`, then `... restart`. The MPC needs **MPC OS 3.10.1 or later** with **Preferences → Enhancements → Enable Enhancements + External Plugin Support**.

## Licence

`src/UP2noDX.cpp`, `skin_tools/` and the small stubs (`src/libMTSClient.h`, `src/debugger/Logger.h`, `src/msfa/tuning.h`): MIT.
`src/msfa/` engine: Apache License 2.0 (Copyright Google Inc., Pascal Gauthier, Jean Pierre Cimalando and contributors); see `src/msfa/LICENSE` and `src/msfa/README.md`.
