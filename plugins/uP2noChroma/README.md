# uP2noChroma (v0.1.0, effect, uid `Ukl6`)

A four-module colour and lo-fi multi-effect for the MPC, in the spirit of the Hologram Electronics
Chroma Console pedal (its module layout and effect list), without the looper. By uP2noGood. Original
code (MIT); nothing is taken from the pedal or any other product.

## Signal path

    in -> [Character] -> [Movement] -> [Diffusion] -> [Texture] -> Mix with dry -> Output -> soft ceiling
          (any of the 24 orders with Routing; each module: one of five effects, On/Off)

## Pages (one Q-Link bank of four per column; knobs 1-4 = Q-Link 13, 9, 5, 1 + column)

**CONSOLE**. Each column also has a touch row of five effect buttons and an ON switch in its header.

| Character (red) | Movement (amber) | Diffusion (green) | Texture (blue) |
|---|---|---|---|
| Tilt | Rate | Time | Amount |
| Amount | Amount | Amount | Vol |
| Sensitivity | Drift | Drift | Filter Style |
| Vol | Vol | Vol (wet only) | Drift |

**SETUP**

| Move tempo | Diff tempo | Texture tone | Global |
|---|---|---|---|
| Sync | Sync | Tone | Mix |
| Division | Division | | Output |
| Spread | Ping-Pong | | Input Level |
| | Duck | | Routing |

## The effects

| Module | Effect | Main knob | Amount | Drift / Sensitivity |
|---|---|---|---|---|
| Character | Drive | Tilt = tone | drive | Sensitivity = how early it breaks up |
| | Sweeten | Tilt = tone | compression + saturation (preamp EQ built in) | |
| | Fuzz | Tilt = tone and bias (left: starved, gated; right: fat) | gain; "blown speaker" near max | |
| | Howl | Tilt: left = singing sustain, right = synth-like stabs | gain and resonance | |
| | Swell | Tilt = tone | swell time 40 ms to 3 s | Sensitivity = trigger threshold |
| Movement | Doubler | Rate = doubling time 8-110 ms | how much double | Drift = sudden pitch lurches |
| | Vibrato | Rate = speed | depth | Drift = sine to random warble, wider |
| | Phaser | Rate = speed | intensity, 2 to 12 stages | Drift = random sweep |
| | Tremolo | Rate = speed | depth, squarer near max | Drift = unsteady rate and depth |
| | Pitch | Rate = -12..+12 semitones | wet mix | Drift = lo-fi (sample-and-hold, fewer bits) |
| Diffusion | Cascade | Time = 30-600 ms (BBD: longer is darker) | feedback, runs away at max | Drift = wobble and worn repeats |
| | Reels | Time = 50-800 ms tape echo | feedback, runs away at max | Drift = wow, flutter, dropouts |
| | Space | Time = chamber, room, hall, vast, cloud (0.35-17 s) | wet/dry | Drift = pitch modulation |
| | Collage | Time = slice 25 ms-1.5 s | repeats and feedback | Drift = double-speed slices, pitch wobble |
| | Reverse | Time = speed 0.5x / 1x (centre) / 2x | wet/dry | Drift = pitch wobble |
| Texture | Filter | (Filter Style LPF / Tilt / HPF) | cutoff (Tilt: left darker, right thinner, centre open) | Drift = cutoff wander; Tone = resonance |
| | Squash | | heavy compression, overdrive near max | Drift = pumping wobble |
| | Cassette | | saturation, band-limit, wow, flutter, hiss | Drift = more wow, dropouts |
| | Broken | | sags in pitch and level, more often | Drift = less regular |
| | Interference | | radio static, then telecom dropouts, then dissolving | Drift = more events |

- **Input Level** (Low / Medium / High / Very High) is the pedal's calibration: how hard the drive,
  compression and tape stages are hit (+12 / +6 / 0 / -6 dB inside). Use High for normal MPC material.
- **Sync** replaces tap tempo: Vibrato, Phaser and Tremolo rates; Cascade, Reels and Collage times; and
  the Reverse segment follow the MPC tempo in note divisions. With the sequencer playing, the
  modulation phase is locked to the song.
- **Vol** at the bottom of its range is Off. Diffusion Vol changes only the wet part.
- Changing an effect or switching a module fades it out and back in over 15 ms; a delay or reverb
  starts empty (no old echoes).

## Programs

Init, Tape Memory, Fuzz Cathedral, Broken Radio, Backwards Dream, Stab Collage, Squashed Room.

## Honest notes

- Not yet heard on an MPC. Tested with the SDK host simulator and an offline harness: all 20 effects
  at normal and extreme settings (Amount, Drift, Tilt, Sensitivity at max, Input Level Low, Sync,
  Ping-Pong, Duck) stay finite and under 0 dBFS; Pitch intervals, tempo sync and the Reverse direction
  were checked by measurement; switching effects and modules on and off was checked for clicks.
  Tuning by ear is still to do.
- Interference makes clicks and crackle on purpose. Swell needs notes that ring; on short plucks with a
  long swell time it stays quiet.
- Not included: the looper (CAPTURE), knob recording (GESTURE: use MPC automation), presets on the
  pedal's preset slots, MIDI control (MPC effects get no MIDI).
- CPU on the desktop: about 0.5-1 % of real time with all four modules on (Space and Phaser are the
  heaviest). Check it on the Key 37.

## Build

    python3 tools/build.py plugins/uP2noChroma
    .sdk-tools/host_sim plugins/uP2noChroma/build/native/up2nochroma.dylib test --wav plugins/uP2noChroma/build/test.wav

The skin in `skins/` is generated by `skin_tools/make_skin.py` (needs Pillow; re-run it after changing
the layout, then rebuild). `--preview file.png` writes a picture of both pages.
