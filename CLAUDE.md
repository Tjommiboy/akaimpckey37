# CLAUDE.md

This repository's agent instructions live in [AGENTS.md](AGENTS.md). Read it first, then use the skills in `.claude/skills/`.

@AGENTS.md

## uP2noGood house rules

### Q-Link layout (applies to every plugin in this repo)

- Each bank of four knobs controls **one logical unit only**: everything for Osc 1, then everything for Osc 2, then Osc 3; one envelope; one filter section. Never mix units in a bank.
- Examples: OSC page = Osc1 Range, Tune/Freq, Wave, Vol | Osc2 Range, Freq, Wave, Vol | Osc3 ... ; FILTER page = Cutoff, Emphasis/Resonance, Env Amount, Kbd Track 1 | Attack, Decay, Sustain, Kbd Track 2; AMP page = Attack, Decay, Sustain, Release/Decay switch | Volume, velocity amounts, ...
- The MPC Key 37 has 4 knobs and banks by **columns** of the 4 x 4 grid (knobs 1-4 = Q-Link 13, 9, 5, 1), whatever `Bank Direction` says. So each group of four must be **one column**:
  - in `ui.json`, slot i goes to row i // 4, column i % 4: list each group down a column, not across a row;
  - in hand-written skins, group g -> Q-Link 13+g, 9+g, 5+g, 1+g, `"Bank Direction": "Column"`, and `qlinkBoundsData[g]` drawn around that group.
- Program Mode Q-Links: the 16 most important parameters, grouped the same way.
