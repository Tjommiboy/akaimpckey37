Put DX7 32-voice bank files (*.syx) in this folder.

Accepted: standard DX7 32-voice bulk dumps (4104 bytes, starting F0 43 00 09 20 00) and raw
4096-byte banks. Every voice of every bank shows up as a program in the MPC's preset list,
after the uP2noGood presets, named "<file name> <voice name>". Files are read in name order,
so name them e.g. "01 ROM1A.syx", "02 ROM1B.syx" to choose the order.

After adding or removing banks, rebuild / reinstall the plugin and reload it on the track.

The .syx files in this folder are NOT committed to git (see ../../.gitignore): they stay on your
computer and your MPC only.
