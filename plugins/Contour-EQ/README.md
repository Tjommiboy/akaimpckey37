# Contour EQ (test build 0.3)

A visual 4-band EQ for the MPC by **uP2noGood**. The MPC does not let plugins draw their own graphics, so the EQ curve is drawn as **160 bars** whose heights the plugin sets, with a live **spectrum analyzer** (dim bars) behind them and a **draggable dot** for every band.

This build is a hardware test: the bars, dots and analyzer are made from MPC skin widgets the SDK marks "less tested", so the main question is how they behave on a real MPC.

## Pages

| Page | What |
|---|---|
| **GRAPH** | 160-bar response + analyzer. Tap BAND 1-4 to choose which dot you drag. Q-Links: one column per band, Freq (top), Gain, Q, Shape (bottom). |
| **B1-2 / B3-4** | Knobs for every band: Freq, Gain, Q, Shape, Slope, On. |
| **SETUP** | Output, Analyzer (Off / Pre / Post), Speed, analyzer Tilt, Refresh (Normal / Force). |

Shapes: Bell, Low Shelf, High Shelf, Low Cut, High Cut (6-48 dB/oct, Q adds resonance), Notch, Band Pass, Tilt. Zero latency, stereo. The graph spans ±18 dB.

## Changes in 0.2

0.1 on a Key 37: the bars only updated when the page was reopened (the MPC does not redraw slider widgets that the plugin changes itself). In 0.2:

- The bars are **Meter widgets**, which the MPC is meant to animate. The EQ curve is drawn as each meter's peak marker (a bright line), and the analyzer bars have peak hold.
- **SETUP → Refresh** has three options if the graph still freezes: *Normal* (meters only), *Display* (asks the MPC to re-read every value ~25 times a second), and *Notify* (reports each changed bar to the MPC like a knob turn). Both run on a separate thread, never on the audio thread.

## Changes in 0.3

0.2 on a Key 37: still no live redraw in any Refresh mode; the picture is right when the page is reopened. In 0.3:

- Filled bars are back (the EQ fills from 0 dB, the analyzer from the bottom).
- The display parameters are now **automatable** and **Refresh defaults to Notify**: the MPC may ignore change reports for parameters that cannot be automated. Side effect: 320 "EQ n" / "Spec n" entries in the automation parameter list. Do not record automation with Refresh on Notify.

## Changes in 0.5

0.4 on a Key 37: 80 bars clearly smoother than 160 (about 5-10 updates a second), so the MPC is limited by how many bars it redraws. In 0.5:

- One GRAPH page; its bars follow **SETUP → Bars** (EQ curve: 160 / **80**, default 80) and **Ana Bars** (analyzer: **80** / 40). No more mismatched "shadow" bars.
- Each analyzer bar on screen shows the loudest of its group, so nothing is missed at 40.
- A bar that stops just short of a pixel is still redrawn once, so no stale bars are left behind.
- The analyzer pauses for about 0.3 s while the EQ curve moves, so edits redraw first.

## What to check on the MPC

1. Do the white bars change when you turn a band's Gain or Freq on the B pages?
2. Do the blue analyzer bars move while audio plays? If not, try SETUP → Refresh: Force.
3. Can you drag a dot on GRAPH? Does it follow your finger (up = more gain)?
4. Is GRAPH (160 bars) smooth, or does the screen lag compared with G-80?
5. Any crackles while the analyzer runs?

## Accuracy

The bars are computed from the filter coefficients and checked against the measured impulse response of every preset: within 0.1 dB from 20 Hz to 20 kHz.

## Rebuilding the skin

`skins/` is generated. After changing parameters or `ui.json`:

```
python3 tools/build.py plugins/Contour-EQ --target native
python3 plugins/Contour-EQ/skin_tools/make_skin.py
python3 tools/build.py plugins/Contour-EQ
```

## Not in this build

Only 4 bands; no dynamic EQ, mid/side or linear phase. Planned once the display is confirmed on hardware.

MIT licence. Inspired by modern visual EQs; not affiliated with FabFilter.
