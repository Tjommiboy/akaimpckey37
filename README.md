# akaimpckey37 — MPC plugins by uP2noGood

Custom plugins for the Akai MPC Key 37 (and other MPC OS 3.10.1+ standalones), built with
[inMusic's MPC Plugin SDK](https://github.com/inmusic-software/mpc-plugin-builder-sdk).

| Plugin | What it is |
|---|---|
| [DWG-86](plugins/DWG-86/README.md) | 8-voice synth in the spirit of the Korg DW-6000 / DW-8000: 16 digital waves, 24 dB resonant filter, six-stage envelopes, auto bend, unison, modulated digital delay |

## Build

Needs Python 3.9+. The first build downloads the Zig cross compiler (~50 MB).

```
python tools/build.py plugins/DWG-86
```

Then copy `plugins/DWG-86/build/uP2noGood - VST - DWG-86/` into a folder called `VST2 Plugins` on a USB stick or SD card,
and on the MPC turn on Preferences → Enhancements → **Enable Enhancements** and **External Plugin Support**.

## About this repo

Everything outside `plugins/` is a copy of the MPC Plugin SDK (MIT, © inMusic Brands), snapshot of commit `febd2ca`.
The SDK's own documentation is in [SDK-README.md](SDK-README.md), [GETTING_STARTED.md](GETTING_STARTED.md) and `docs/`.
Open this folder in Claude Code (or VS Code with Claude Code) to make more plugins: `AGENTS.md` teaches the agent the workflow.
