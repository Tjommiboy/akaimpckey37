# msfa (FM engine)

The FM engine in this folder is **msfa** ("Music Synthesizer for Android") by Raph Levien and
contributors, Copyright 2012-2019 Google Inc., Pascal Gauthier, Jean Pierre Cimalando and others,
licensed under the **Apache License, Version 2.0** (see `LICENSE` in this folder).

Source: the `Source/msfa` folder of Dexed (https://github.com/asb2m10/dexed), commit
fea2f0f0f2c397799f59b27eb11c038cd34b22c5 (2026-10-14). Dexed as a whole is GPL-3; only the msfa
files, which keep their Apache-2.0 headers, are used here. No other Dexed code is included.

Changes for uP2noDX:
- The msfa `.cc`/`.cpp` files and headers are unmodified. Each is compiled through a one-line wrapper `../msfa_<name>.cpp`, because the SDK build compiles only `src/*.cpp`.
- Dexed's `tuning.h` / `tuning.cc` are NOT included (no Apache header, they depend on JUCE and
  Surge's Tunings library). `tuning.h` in this folder is a new file written for uP2noDX
  (standard tuning only).
- `../libMTSClient.h` and `../debugger/Logger.h` are small stubs written for uP2noDX so the
  unmodified msfa files compile without Dexed's dependencies.
