// tuning.h - written for uP2noDX (uP2noGood), MIT licence. Replaces Dexed's tuning.h/.cc, which
// are not part of the Apache-2.0 msfa code and depend on JUCE and Surge's Tunings library.
// Only standard 12-TET tuning is provided; the interface is what dx7note.cc needs.
#ifndef __SYNTH_TUNING_H
#define __SYNTH_TUNING_H

#include "synth.h"
#include <memory>

class TuningState {
public:
    virtual ~TuningState() { }
    // Q24 log2 frequency of a MIDI note in standard tuning (A4 = 440 Hz).
    virtual int32_t midinote_to_logfreq(int midinote) {
        const int base = 50857777;          // (1 << 24) * (log2(440) - 69/12)
        const int step = (1 << 24) / 12;
        return base + step * midinote;
    }
    virtual bool is_standard_tuning() { return true; }
};

inline std::shared_ptr<TuningState> createStandardTuning() { return std::make_shared<TuningState>(); }

#endif
