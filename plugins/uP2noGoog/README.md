# uP2noGoog (test build 0.2)

A monophonic three-oscillator synth for Akai MPC standalones by **uP2noGood**, in the spirit of the 1970s Minimoog Model D. It is not a circuit clone: no schematics, firmware or code from Moog or anyone else are used. "Minimoog" and "Model D" are trademarks of Moog Music Inc.; this project is not affiliated with Moog.

## Signal path

```
Osc 1 ─┐
Osc 2 ─┤
Osc 3 ─┼─► MIXER ─► soft saturation ─► 24 dB ladder filter ─► VCA ─► Volume ─► out
Noise ─┤   (5 ch, on/off       (Cutoff, Emphasis,            (Loudness
Fdbk ──┘    + volume)           Contour Amt, Kbd Ctrl 1+2)    contour)
```

Oscillators, mixer and filter run at 2x the MPC's sample rate.

## Pages

| Page | Controls |
|---|---|
| **PANEL** | The whole synth on one screen, laid out like a 1970s Model D: CONTROLLERS, OSCILLATOR BANK (rotary Range and Waveform selectors), MIXER, MODIFIERS, OUTPUT. Q-Links: Filter row, Filter Contour row, Loudness Contour row, Controllers column. Double-tap a knob for the big edit view. |
| **OSC** | Range (LO, 32', 16', 8', 4', 2') and Wave for each oscillator; Tune; Osc 2/3 Freq (±7 semitones, about ±6 octaves in LO); Drift; A-440 reference tone; Osc3 Ctrl (Osc 3 follows the keyboard) |
| **MIXER** | Volume and On for Osc 1-3 and Noise; Feedback volume/On; Noise colour (White/Pink); Drive |
| **FILTER** | Cutoff, Emphasis, Contour Amt, Kbd Ctrl 1 (1/3 tracking) and 2 (2/3; both = full); filter contour Attack, Decay, Sustain |
| **AMP** | Loudness contour Attack, Decay, Sustain; Decay Sw (release = decay time); Volume; velocity to loudness / filter; Glide On |
| **MOD** | Mod Mix (Osc 3 ↔ Noise), Mod Amt, Osc Mod, Filter Mod; Glide time; Bend Range; aftertouch to modulation / filter |

The PANEL page and the restyled detail pages are drawn by `skin_tools/make_skin.py`.

## How it behaves

- **Waves**: Triangle, Tri-Saw, Saw, Square, Wide pulse, Narrow pulse. Osc 3 has Reverse Saw instead of Tri-Saw (handy as an LFO shape).
- **Osc 3 as LFO**: Range LO + Osc3 Ctrl off, turn on Osc Mod and/or Filter Mod, raise Mod Amt or the mod wheel. Osc3 Freq sets the rate (about 0.3-15 Hz).
- **Mono, low-note priority, single trigger**: holding a key and playing another changes pitch without restarting the contours. Glide (with Glide On) always slides from the last pitch.
- **Overdrive**: hot mixer levels saturate before the filter, like the original. The Feedback channel feeds the output back into the mixer (from the 2016 reissue's mod) for growl and screams.
- **Filter**: self-oscillates at the top of Emphasis, in tune across the range. With both Kbd Ctrl switches on it plays in tune from the keyboard.
- **Modulation amount** = Mod Amt + mod wheel + aftertouch (AT>Mod).
- Pitch bend range 1-12 semitones (default 2).

## Presets

Init · Fat Bass · Classic Lead · Brass · Flute · Pluck · Osc3 Wobble · Screamer · Noise Wind

## Test results (host simulator)

All checks pass. In tune to 0.01 Hz from C2 to C6; filter self-oscillation within 0.1 Hz of the Cutoff setting from 220 Hz to 3 kHz. About 1.2 % of a desktop core; expect a few percent on the MPC (to be checked on the Key 37).

## What to check on the MPC

1. CPU on the Key 37 (one instance, then two).
2. Are the presets in tune with the MPC's other instruments?
3. Does the mod wheel and aftertouch on the Key 37 reach the plugin?
4. Feel of Cutoff and Emphasis on the Q-Links.

## Changes in 0.2

- PANEL overview page with custom graphics: silver-and-black knobs with 0-10 scales, rotary Range/Waveform selectors with printed legends and waveform icons, rocker switches with lamps, wood end cheeks.
- Detail pages restyled to match (same knobs, rocker toggles, warm focus colour). The White/Pink switch now shows its choices.

## Rebuilding the skin

`skins/` is generated. After changing parameters or `ui.json` (needs Python with Pillow):

```
python3 tools/build.py plugins/uP2noGoog --target native
python3 plugins/uP2noGoog/skin_tools/make_skin.py
python3 tools/build.py plugins/uP2noGoog
```

## Building

From the SDK root folder:

```
python3 tools/build.py plugins/uP2noGoog
```

Copy `plugins/uP2noGoog/build/uP2noGood - VST - uP2noGoog/` into `VST2 Plugins` on a USB stick or SD card. The MPC needs **MPC OS 3.10.1 or later** with **Preferences → Enhancements → Enable Enhancements + External Plugin Support** turned on.

## Licence

MIT. The filter is our own code, using Zavalishin's zero-delay-feedback method with the input saturation placement described by Huovilainen (DAFx 2004).
