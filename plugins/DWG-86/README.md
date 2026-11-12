# DWG-86

An 8-voice synth for Akai MPC standalones by **uP2noGood**, in the spirit of the mid-80s Korg DW-6000 / DW-8000: digital waveforms through an analogue-style resonant filter.

It is not a sample-accurate clone. No ROM data or firmware is used. The 16 waveforms are built additively when the plugin loads, from harmonic recipes that aim for the same flavour.

## Signal path

```
Osc1 (16 waves, 16'/8'/4') ─┐
Osc2 (16 waves, 16'/8'/4',  ├─► 24 dB resonant low-pass ─► VCA ─► digital delay ─► out (stereo)
      interval, detune)     │    (EG ±, key track, MG,      (ADBSSR EG,
Noise ──────────────────────┘     aftertouch, bend)          velocity, AT)
```

## Waveforms

1 Saw · 2 Square · 3 Piano · 4 E.Piano 1 · 5 E.Piano 2 · 6 Clavi · 7 Organ · 8 Brass · 9 Sax · 10 Violin · 11 A.Guitar · 12 E.Guitar · 13 Bass · 14 Dig.Bass · 15 Bell · 16 Sine

## Pages

| Page | Controls |
|---|---|
| **OSC** | Osc1/Osc2 wave and level; octave (16'/8'/4'); Interval (unison, min 3rd, maj 3rd, 4th, 5th); Detune; Noise; Assign (Poly / Unison) and Uni Detune; Portamento |
| **VCF** | Cutoff, Resonance (self-oscillates near the top), EG Amount (bipolar, so negative = inverted EG), Key Track (0, 1/4, 1/2, Full), VCF Velo, MG>VCF, AT>VCF, Bend>VCF |
| **ENV** | Two DW-style six-stage envelopes: **Attack → Decay to Break point → Slope to Sustain → Release**. The top two rows are for the VCF and the bottom two for the VCA. |
| **MOD** | MG (LFO) wave (Tri / Saw / Ramp / Square), rate, delay (vibrato fades in after each key press), MG>Osc; mod wheel and aftertouch vibrato; bend range; **Auto Bend** (Osc1 / Osc2 / Both, Up or Down into the note, time, amount) |
| **DELAY** | Time (1–1000 ms), Feedback, Level, Mod Rate and Mod depth (the left and right taps are modulated in opposite phase, so short times with modulation give chorus or flanger effects), master Volume |

## Presets

Init DW · DW Brass · Digi Piano · Unison Bass · Strings · Organ · Clav Funk · Glass Bells · Whistle Lead

## Tips

- **Break point and slope** are what make the DW envelopes special. Set Break below Sustain and the level dips, then swells back up (good for brass). Set it above and the sound plateaus before settling.
- **Chorus:** set Dly Time to 15–25 ms, Fdbk to 0, Dly Mod to 40–60 % and Dly Level to 40–50 %.
- **Unison** stacks all 8 voices on one note, spread by Uni Detune. Use it with portamento for leads and basses.
- **Auto Bend** in "Up" mode with a short time and 1–2 semitones gives the classic brass "blip".

## Building

From the SDK root folder:

```
python tools/build.py plugins/DWG-86
```

Copy `plugins/DWG-86/build/uP2noGood - VST - DWG-86/` into `VST2 Plugins` on a USB stick or SD card. The MPC needs **MPC OS 3.10.1 or later** with **Preferences → Enhancements → Enable Enhancements + External Plugin Support** turned on.

## Licence

MIT. "Korg", "DW-6000" and "DW-8000" are trademarks of Korg Inc., used here only to describe the inspiration. This project is not affiliated with Korg.
