// DWG-86 - an MPC instrument plugin by uP2noGood
//
// SPDX-License-Identifier: MIT
//
// An 8-voice hybrid synth in the spirit of the mid-80s "digital waveform"
// polysynths (Korg DW-6000 / DW-8000 era): digital single-cycle waveforms
// feeding an analogue-style resonant 24 dB low-pass filter, six-stage
// envelopes, auto bend, a delayable MG (LFO) and a modulated digital delay.
//
// Nothing here is copied from any instrument's ROM or firmware. The 16
// waveforms are generated additively at load time from hand-written harmonic
// recipes that aim for the same flavour (saw, square, piano, e-pianos, clavi,
// organ, brass, sax, violin, guitars, basses, bell, sine).
//
// Signal path per voice:
//   Osc1 (wave, 16'/8'/4') + Osc2 (wave, octave, interval, detune) + noise
//   -> 4-pole ZDF ladder low-pass (cutoff, resonance, EG +/-, key track, MG, AT, bend)
//   -> VCA (ADBSSR envelope, velocity, aftertouch)
// Global: MG (tri/saw/ramp/square, delay fade-in per voice), auto bend, portamento,
//         poly or unison assign, stereo-modulated digital delay, master volume.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/smoother.h"
#include "dsp/voice.h"
#include "dsp/midi_cc.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace mpc::dsp;

namespace {

// ---------------------------------------------------------------------------
// Wavetables: 16 waves x 10 band-limited octave levels x 2048 samples.
// ---------------------------------------------------------------------------
constexpr int kNumWaves  = 16;
constexpr int kTableBits = 11;
constexpr int kTableSize = 1 << kTableBits;     // 2048
constexpr int kTableMask = kTableSize - 1;
constexpr int kNumLevels = 10;                  // 512, 256, ... 1 harmonics
constexpr int kMaxHarm   = 512;
constexpr int kStride    = kTableSize + 1;      // +1 guard sample for interpolation

float waveHarmonic(int w, int k)
{
    const float kf = (float)k;
    auto bump = [kf](float centre, float width) { const float d = kf - centre; return std::exp(-(d * d) / width); };
    switch (w)
    {
        case 0:  return 1.0f / kf;                                                      // Saw
        case 1:  return (k & 1) ? 1.0f / kf : 0.0f;                                     // Square
        case 2:  return std::pow(kf, -1.4f) * (1.0f + 0.6f * bump(4.0f, 4.0f))          // Piano
                        * (k > 40 ? std::exp(-(kf - 40.0f) / 20.0f) : 1.0f);
        case 3:  switch (k) { case 1: return 1.0f; case 2: return 0.25f; case 3: return 0.08f;   // E.Piano 1
                              case 7: return 0.18f; case 8: return 0.10f; default: return 0.0f; }
        case 4:  return (k <= 24 ? std::pow(kf, -1.7f) : 0.0f) + 0.15f * bump(10.0f, 2.0f) / kf * 4.0f; // E.Piano 2
        case 5:  return std::sin(kPi * kf * 0.12f) / std::pow(kf, 0.9f);               // Clavi
        case 6:  switch (k) { case 1: return 1.0f; case 2: return 0.75f; case 3: return 0.55f;   // Organ
                              case 4: return 0.45f; case 6: return 0.30f; case 8: return 0.35f;
                              case 10: return 0.12f; case 12: return 0.15f; case 16: return 0.10f;
                              default: return 0.0f; }
        case 7:  return (1.0f / kf) * (1.0f + 1.2f * bump(5.0f, 8.0f))                 // Brass
                        * (k > 24 ? std::exp(-(kf - 24.0f) / 12.0f) : 1.0f);
        case 8:  return ((k & 1) ? 1.0f : 0.35f) / kf * (1.0f + 1.5f * bump(8.0f, 10.0f)); // Sax
        case 9:  { const float s = std::sin(kf * 0.85f);                                // Violin
                   return (1.0f / kf) * (0.6f + 0.8f * s * s) * (1.0f + 0.8f * bump(12.0f, 30.0f)); }
        case 10: return std::sin(kPi * kf * 0.22f) / std::pow(kf, 1.2f);               // A.Guitar
        case 11: return ((k & 1) ? 1.0f : 0.5f) / std::pow(kf, 0.75f) * std::exp(-kf / 60.0f); // E.Guitar
        case 12: return k == 1 ? 1.0f : 0.8f * std::pow(kf, -1.8f);                    // Bass
        case 13: switch (k) { case 1: return 1.0f; case 2: return 0.3f; case 3: return 0.6f;     // Dig.Bass
                              case 5: return 0.55f; case 7: return 0.5f; case 9: return 0.35f;
                              case 11: return 0.25f; case 13: return 0.2f; default: return 0.0f; }
        case 14: switch (k) { case 1: return 0.5f; case 3: return 0.2f; case 5: return 1.0f;     // Bell
                              case 8: return 0.6f; case 11: return 0.45f; case 15: return 0.35f;
                              case 19: return 0.2f; default: return 0.0f; }
        default: return k == 1 ? 1.0f : 0.0f;                                           // Sine
    }
}

const std::vector<std::string>& waveNames()
{
    static const std::vector<std::string> n = {"Saw", "Square", "Piano", "E.Piano 1", "E.Piano 2", "Clavi",
                                               "Organ", "Brass", "Sax", "Violin", "A.Guitar", "E.Guitar",
                                               "Bass", "Dig.Bass", "Bell", "Sine"};
    return n;
}

// ---------------------------------------------------------------------------
// Six-stage DW-style envelope: Attack -> Decay to Break Point -> Slope to
// Sustain -> (hold) -> Release. Exponential segments, retriggers from the
// current level so stolen / repeated notes do not click.
// ---------------------------------------------------------------------------
struct EnvCoefs { float atk, dec, slp, rel, brk, sus; };

struct DWEnvelope
{
    enum Stage { Idle, Attack, Decay, Slope, Release };
    Stage stage = Idle;
    float level = 0.0f;

    void noteOn()  { stage = Attack; }
    void noteOff() { if (stage != Idle) stage = Release; }
    void reset()   { stage = Idle; level = 0.0f; }
    bool active() const { return stage != Idle; }

    float process(const EnvCoefs& c)
    {
        switch (stage)
        {
            case Attack:
                level += (1.3f - level) * c.atk;
                if (level >= 1.0f) { level = 1.0f; stage = Decay; }
                break;
            case Decay:
                level = c.brk + (level - c.brk) * c.dec;
                if (std::fabs(level - c.brk) < 1.0e-3f) stage = Slope;
                break;
            case Slope:
                level = c.sus + (level - c.sus) * c.slp;
                break;
            case Release:
                level *= c.rel;
                if (level < 1.0e-4f) { level = 0.0f; stage = Idle; }
                break;
            default: break;
        }
        return level;
    }
};

// exp coefficient so a segment covers ~99% of its distance in `ms`
inline float segCoef(float ms, double sr, float timeConstants = 4.6f)
{
    const float n = std::max(1.0f, ms * 0.001f * (float)sr);
    return std::exp(-timeConstants / n);
}

} // namespace

class DWG86 : public mpc::Plugin
{
public:
    enum Params
    {
        kVolume,
        kO1Oct, kO1Wave, kO1Level, kO2Oct, kO2Wave, kO2Level, kInterval, kDetune, kNoise,
        kABSelect, kABMode, kABTime, kABAmount,
        kCutoff, kReso, kKeyTrack, kEGAmount, kVCFVelo,
        kFAtk, kFDec, kFBrk, kFSlp, kFSus, kFRel,
        kAAtk, kADec, kABrk, kASlp, kASus, kARel, kVCAVelo,
        kMGWave, kMGRate, kMGDelay, kMGOsc, kMGVCF, kWheelMG,
        kATVib, kATVCF, kATVCA,
        kAssign, kUniDetune, kPorta, kBendRange, kBendVCF,
        kDlyTime, kDlyFdbk, kDlyModRate, kDlyMod, kDlyLevel,
        kNumParams
    };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "DWG-86";
        i.vendor = "uP2noGood";
        i.uniqueId = MPC_FOURCC('U', 'm', 'r', 'E');   // must match "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    DWG86() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        const std::vector<std::string> octs = {"16'", "8'", "4'"};
        addParam(floatParam("vol",     "Volume", -40.0f, 6.0f, -6.0f, "dB", 1));
        // OSC
        addParam(choiceParam("o1oct",  "Osc1 Oct", octs, 1));
        addParam(choiceParam("o1wave", "Osc1 Wave", waveNames(), 0));
        addParam(floatParam("o1lvl",   "Osc1 Level", 0.0f, 100.0f, 100.0f, "%", 0));
        addParam(choiceParam("o2oct",  "Osc2 Oct", octs, 1));
        addParam(choiceParam("o2wave", "Osc2 Wave", waveNames(), 1));
        addParam(floatParam("o2lvl",   "Osc2 Level", 0.0f, 100.0f, 70.0f, "%", 0));
        addParam(choiceParam("o2int",  "Interval", {"Unison", "Min 3rd", "Maj 3rd", "4th", "5th"}, 0));
        addParam(floatParam("o2det",   "Detune", 0.0f, 50.0f, 8.0f, "ct", 0));
        addParam(floatParam("noise",   "Noise", 0.0f, 100.0f, 0.0f, "%", 0));
        // AUTO BEND
        addParam(choiceParam("absel",  "AB Select", {"Off", "Osc1", "Osc2", "Both"}, 0));
        addParam(choiceParam("abmode", "AB Mode", {"Up", "Down"}, 0));
        addParam(logParam  ("abtime",  "AB Time", 5.0f, 2000.0f, 120.0f, "ms", 0));
        addParam(floatParam("abamt",   "AB Amount", 0.0f, 12.0f, 3.0f, "st", 1));
        // VCF
        addParam(logParam  ("cutoff",  "Cutoff", 20.0f, 18000.0f, 2500.0f, "Hz", 0));
        addParam(floatParam("reso",    "Resonance", 0.0f, 100.0f, 15.0f, "%", 0));
        addParam(choiceParam("kbt",    "Key Track", {"0", "1/4", "1/2", "Full"}, 2));
        addParam(floatParam("egamt",   "EG Amount", -100.0f, 100.0f, 45.0f, "%", 0));
        addParam(floatParam("fvel",    "VCF Velo", 0.0f, 100.0f, 30.0f, "%", 0));
        // VCF EG
        addParam(logParam  ("fatk",    "F Attack", 0.5f, 10000.0f, 2.0f, "ms", 1));
        addParam(logParam  ("fdec",    "F Decay", 1.0f, 10000.0f, 400.0f, "ms", 0));
        addParam(floatParam("fbrk",    "F Break", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(logParam  ("fslp",    "F Slope", 1.0f, 20000.0f, 1500.0f, "ms", 0));
        addParam(floatParam("fsus",    "F Sustain", 0.0f, 100.0f, 30.0f, "%", 0));
        addParam(logParam  ("frel",    "F Release", 1.0f, 10000.0f, 300.0f, "ms", 0));
        // VCA EG
        addParam(logParam  ("aatk",    "A Attack", 0.5f, 10000.0f, 2.0f, "ms", 1));
        addParam(logParam  ("adec",    "A Decay", 1.0f, 10000.0f, 600.0f, "ms", 0));
        addParam(floatParam("abrk",    "A Break", 0.0f, 100.0f, 85.0f, "%", 0));
        addParam(logParam  ("aslp",    "A Slope", 1.0f, 20000.0f, 2500.0f, "ms", 0));
        addParam(floatParam("asus",    "A Sustain", 0.0f, 100.0f, 70.0f, "%", 0));
        addParam(logParam  ("arel",    "A Release", 1.0f, 10000.0f, 300.0f, "ms", 0));
        addParam(floatParam("avel",    "VCA Velo", 0.0f, 100.0f, 40.0f, "%", 0));
        // MG
        addParam(choiceParam("mgwave", "MG Wave", {"Tri", "Saw", "Ramp", "Square"}, 0));
        addParam(logParam  ("mgrate",  "MG Rate", 0.05f, 30.0f, 5.0f, "Hz", 2));
        addParam(floatParam("mgdly",   "MG Delay", 0.0f, 5000.0f, 300.0f, "ms", 0, 0.5f));
        addParam(floatParam("mgosc",   "MG>Osc", 0.0f, 12.0f, 0.0f, "st", 2, 0.5f));
        addParam(floatParam("mgvcf",   "MG>VCF", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("whlmg",   "Wheel>MG", 0.0f, 12.0f, 0.6f, "st", 2, 0.5f));
        // AFTERTOUCH
        addParam(floatParam("atvib",   "AT>Vibrato", 0.0f, 12.0f, 0.3f, "st", 2, 0.5f));
        addParam(floatParam("atvcf",   "AT>VCF", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("atvca",   "AT>VCA", 0.0f, 100.0f, 0.0f, "%", 0));
        // PLAY
        addParam(choiceParam("assign", "Assign", {"Poly", "Unison"}, 0));
        addParam(floatParam("unidet",  "Uni Detune", 0.0f, 50.0f, 12.0f, "ct", 0));
        addParam(floatParam("porta",   "Portamento", 0.0f, 2000.0f, 0.0f, "ms", 0, 0.5f));
        addParam(intParam  ("bend",    "Bend Range", 0, 12, 2, "st"));
        addParam(boolParam ("bendvcf", "Bend>VCF", false));
        // DELAY
        addParam(logParam  ("dtime",   "Dly Time", 1.0f, 1000.0f, 350.0f, "ms", 0));
        addParam(floatParam("dfb",     "Dly Fdbk", 0.0f, 95.0f, 35.0f, "%", 0));
        addParam(logParam  ("dmrate",  "Dly Mod Rt", 0.05f, 10.0f, 0.5f, "Hz", 2));
        addParam(floatParam("dmod",    "Dly Mod", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("dlvl",    "Dly Level", 0.0f, 100.0f, 0.0f, "%", 0));

        if (numParams() != kNumParams) mpc::log("DWG86: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);

        addPrograms();
        buildWavetables();
    }

    // ----- lifecycle --------------------------------------------------------
    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        sr_ = sampleRate;
        controllers_.prepare(sampleRate, 10.0f);
        volume_.setTime(20.0f, sampleRate);
        volume_.snap(dbToGain(get(kVolume)));
        for (auto& v : voices_) v.glide.setTime(0.0f, sampleRate);

        int size = 1;
        while (size < (int)(sampleRate * 1.1) + 1024) size <<= 1;
        if ((int)delayBuf_.size() != size) delayBuf_.assign((size_t)size, 0.0f);
        delayMask_ = size - 1;
        reset();
    }

    void reset() override
    {
        for (auto& v : voices_) { v.fenv.reset(); v.aenv.reset(); v.clearFilter(); v.note = -1; }
        alloc_.reset();
        controllers_.reset();
        heldCount_ = 0; uniPendingRelease_ = false;
        mgPhase_ = 0.0f; dModPhase_ = 0.0f; fbLp_ = 0.0f;
        std::fill(delayBuf_.begin(), delayBuf_.end(), 0.0f);
        writePos_ = 0;
        dTimeSmooth_ = get(kDlyTime) * 0.001f * (float)sr_;
        volume_.snap(dbToGain(get(kVolume)));
        lastAssign_ = getInt(kAssign);
    }

    // ----- audio ------------------------------------------------------------
    void process(mpc::ProcessContext& ctx) override
    {
        updateBlockSettings();

        int pos = 0;
        for (int e = 0; e < ctx.numMidiEvents; ++e)
        {
            const mpc::MidiEvent& ev = ctx.midi[e];
            if (ev.sampleOffset > pos) { render(ctx, pos, ev.sampleOffset); pos = ev.sampleOffset; }
            handleMidi(ev);
        }
        render(ctx, pos, ctx.numFrames);

        // Mono voice mix is in outputs[0]: run the delay and write stereo.
        float* L = ctx.outputs[0];
        float* R = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        const float targetT = clamp(get(kDlyTime) * 0.001f * (float)sr_, 1.0f, (float)(delayMask_ - 2048));
        const float fb      = get(kDlyFdbk) * 0.01f;
        const float wet     = get(kDlyLevel) * 0.01f;
        const float modAmt  = get(kDlyMod) * 0.01f * 0.008f * (float)sr_;   // up to 8 ms sweep
        const float modInc  = get(kDlyModRate) / (float)sr_;
        const float tSmooth = 1.0f - std::exp(-1.0f / (0.05f * (float)sr_));
        const float fbLpC   = 1.0f - std::exp(-kTwoPi * 5000.0f / (float)sr_);

        for (int i = 0; i < ctx.numFrames; ++i)
        {
            const float dry = L[i];
            dTimeSmooth_ += (targetT - dTimeSmooth_) * tSmooth;
            dModPhase_ += modInc; if (dModPhase_ >= 1.0f) dModPhase_ -= 1.0f;
            const float s = fastSin01(dModPhase_);
            const float tL = dTimeSmooth_ + modAmt * (0.5f + 0.5f * s);
            const float tR = dTimeSmooth_ + modAmt * (0.5f - 0.5f * s);
            const float rl = readDelay(tL), rr = readDelay(tR);
            fbLp_ += (0.5f * (rl + rr) - fbLp_) * fbLpC;
            delayBuf_[(size_t)writePos_] = flushDenormal(dry + fbLp_ * fb);
            writePos_ = (writePos_ + 1) & delayMask_;

            const float g = volume_.process();
            L[i] = (dry + rl * wet) * g;
            if (R) R[i] = (dry + rr * wet) * g;
        }
    }

private:
    static constexpr int kNumVoices = 8;
    static constexpr int kChunk = 64;
    static constexpr int kCtrlInterval = 16;

    struct Voice
    {
        DWEnvelope fenv, aenv;
        Smoother glide;
        float ph1 = 0.0f, ph2 = 0.0f, inc1 = 0.0f, inc2 = 0.0f;
        const float* tab1 = nullptr; const float* tab2 = nullptr;
        float s[4] = {0, 0, 0, 0};
        float G = 0.5f, G2 = 0.25f, G3 = 0.125f, G4 = 0.0625f, oneMinusG = 0.5f, k = 0.0f, inGain = 1.0f;
        int note = -1;
        float vel = 1.0f;
        float uniCents = 0.0f;
        float ab = 0.0f, abStep = 0.0f;       // auto bend offset (semitones) and per-sample step to 0
        float mgFade = 1.0f, mgFadeStep = 1.0f;
        int ctrl = 0;
        void clearFilter() { s[0] = s[1] = s[2] = s[3] = 0.0f; }
    };

    struct Settings
    {
        int o1oct, o1wave, o2oct, o2wave, absel, mgwave, assign;
        float o1lvl, o2lvl, interval, detune, noise, abmode, abtime, abamt;
        float cutoff, reso, kbt, egamt, fvel, avel;
        float mgdly, mgosc, mgvcf, whlmg, atvib, atvcf, atvca, unidet;
        bool bendvcf;
        EnvCoefs fc, ac;
    } s_{};

    // ----- wavetables -------------------------------------------------------
    void buildWavetables()
    {
        tables_.assign((size_t)kNumWaves * kNumLevels * kStride, 0.0f);
        std::vector<float> sinTab((size_t)kTableSize);
        for (int n = 0; n < kTableSize; ++n) sinTab[(size_t)n] = std::sin(kTwoPi * (float)n / (float)kTableSize);
        std::vector<float> amp((size_t)kMaxHarm + 1);

        for (int w = 0; w < kNumWaves; ++w)
        {
            for (int k = 1; k <= kMaxHarm; ++k) amp[(size_t)k] = waveHarmonic(w, k);
            float peak = 0.0f;
            for (int lv = 0; lv < kNumLevels; ++lv)
            {
                const int H = kMaxHarm >> lv;
                float* t = &tables_[((size_t)w * kNumLevels + (size_t)lv) * kStride];
                for (int k = 1; k <= H; ++k)
                {
                    const float a = amp[(size_t)k];
                    if (a == 0.0f) continue;
                    for (int n = 0; n < kTableSize; ++n) t[n] += a * sinTab[(size_t)((k * n) & kTableMask)];
                }
                if (lv == 0) for (int n = 0; n < kTableSize; ++n) peak = std::max(peak, std::fabs(t[n]));
            }
            const float norm = peak > 0.0f ? 1.0f / peak : 1.0f;
            for (int lv = 0; lv < kNumLevels; ++lv)
            {
                float* t = &tables_[((size_t)w * kNumLevels + (size_t)lv) * kStride];
                for (int n = 0; n < kTableSize; ++n) t[n] *= norm;
                t[kTableSize] = t[0];
            }
        }
    }

    const float* table(int wave, float inc) const
    {
        const float maxH = inc > 1.0e-9f ? 0.5f / inc : 1.0e9f;   // harmonics that fit below Nyquist
        int lv = 0;
        while (lv < kNumLevels - 1 && (float)(kMaxHarm >> lv) > maxH) ++lv;
        return &tables_[((size_t)wave * kNumLevels + (size_t)lv) * kStride];
    }

    static inline float readTable(const float* t, float phase)
    {
        const float p = phase * (float)kTableSize;
        const int i = (int)p;
        const float f = p - (float)i;
        return t[i] + f * (t[i + 1] - t[i]);
    }

    float readDelay(float d) const
    {
        const float rp = (float)writePos_ - d;
        const int i = (int)std::floor(rp);
        const float f = rp - (float)i;
        const float a = delayBuf_[(size_t)(i & delayMask_)];
        const float b = delayBuf_[(size_t)((i + 1) & delayMask_)];
        return a + f * (b - a);
    }

    // ----- per-block settings ----------------------------------------------
    void updateBlockSettings()
    {
        static const float kIntervals[5] = {0.0f, 3.0f, 4.0f, 5.0f, 7.0f};
        static const float kKbt[4] = {0.0f, 0.25f, 0.5f, 1.0f};
        s_.o1oct = getInt(kO1Oct) - 1; s_.o2oct = getInt(kO2Oct) - 1;
        s_.o1wave = clamp(getInt(kO1Wave), 0, kNumWaves - 1);
        s_.o2wave = clamp(getInt(kO2Wave), 0, kNumWaves - 1);
        s_.o1lvl = get(kO1Level) * 0.01f; s_.o2lvl = get(kO2Level) * 0.01f;
        s_.interval = kIntervals[clamp(getInt(kInterval), 0, 4)];
        s_.detune = get(kDetune); s_.noise = get(kNoise) * 0.01f;
        s_.absel = getInt(kABSelect); s_.abmode = getInt(kABMode) == 0 ? -1.0f : 1.0f;   // Up starts below
        s_.abtime = get(kABTime); s_.abamt = get(kABAmount);
        s_.cutoff = get(kCutoff); s_.reso = get(kReso) * 0.01f;
        s_.kbt = kKbt[clamp(getInt(kKeyTrack), 0, 3)];
        s_.egamt = get(kEGAmount) * 0.01f; s_.fvel = get(kVCFVelo) * 0.01f; s_.avel = get(kVCAVelo) * 0.01f;
        s_.mgwave = getInt(kMGWave); s_.mgdly = get(kMGDelay);
        s_.mgosc = get(kMGOsc); s_.mgvcf = get(kMGVCF) * 0.01f; s_.whlmg = get(kWheelMG);
        s_.atvib = get(kATVib); s_.atvcf = get(kATVCF) * 0.01f; s_.atvca = get(kATVCA) * 0.01f;
        s_.unidet = get(kUniDetune); s_.bendvcf = getBool(kBendVCF);
        s_.assign = getInt(kAssign);

        s_.fc = { segCoef(get(kFAtk), sr_, 1.47f), segCoef(get(kFDec), sr_), segCoef(get(kFSlp), sr_),
                  segCoef(get(kFRel), sr_), get(kFBrk) * 0.01f, get(kFSus) * 0.01f };
        s_.ac = { segCoef(get(kAAtk), sr_, 1.47f), segCoef(get(kADec), sr_), segCoef(get(kASlp), sr_),
                  segCoef(get(kARel), sr_), get(kABrk) * 0.01f, get(kASus) * 0.01f };

        alloc_.setBendRange((float)getInt(kBendRange));
        volume_.setTarget(dbToGain(get(kVolume)));
        mgInc_ = get(kMGRate) / (float)sr_;
        const float porta = get(kPorta);
        for (auto& v : voices_) v.glide.setTime(porta, sr_);

        if (s_.assign != lastAssign_)   // switching Poly <-> Unison: release everything cleanly
        {
            for (auto& v : voices_) { v.aenv.noteOff(); v.fenv.noteOff(); }
            alloc_.reset(); heldCount_ = 0; uniPendingRelease_ = false;
            lastAssign_ = s_.assign;
        }
    }

    // ----- MIDI ---------------------------------------------------------------
    void startVoice(int vi, int note, int velocity, float uniCents, int fromNote)
    {
        Voice& v = voices_[vi];
        const bool wasActive = v.aenv.active();
        v.note = note;
        v.vel = (float)velocity / 127.0f;
        v.uniCents = uniCents;
        if (get(kPorta) > 0.0f && fromNote >= 0) { if (!wasActive) v.glide.snap((float)fromNote); v.glide.setTarget((float)note); }
        else v.glide.snap((float)note);
        if (!wasActive)
        {
            v.ph1 = 0.0f;
            v.ph2 = (float)(nextRand() & 0xffff) / 65536.0f;
            v.clearFilter();
            v.fenv.level = 0.0f; v.aenv.level = 0.0f;
        }
        if (s_.absel != 0 && s_.abamt > 0.0f)
        {
            v.ab = s_.abmode * s_.abamt;
            v.abStep = s_.abamt / std::max(1.0f, s_.abtime * 0.001f * (float)sr_);
        }
        else { v.ab = 0.0f; v.abStep = 0.0f; }
        const float dlySamples = s_.mgdly * 0.001f * (float)sr_;
        if (dlySamples < 1.0f) { v.mgFade = 1.0f; v.mgFadeStep = 0.0f; }
        else { v.mgFade = 0.0f; v.mgFadeStep = 1.0f / dlySamples; }
        v.fenv.noteOn(); v.aenv.noteOn();
        v.ctrl = 0;
    }

    void releaseVoice(int vi) { voices_[vi].aenv.noteOff(); voices_[vi].fenv.noteOff(); }

    float uniSpread(int vi) const { return s_.unidet * (((float)vi / (float)(kNumVoices - 1)) * 2.0f - 1.0f); }

    void handleMidi(const mpc::MidiEvent& ev)
    {
        const bool unison = s_.assign == 1;
        if (ev.isNoteOn())
        {
            if (unison)
            {
                const int from = heldCount_ > 0 ? held_[heldCount_ - 1] : lastUniNote_;
                removeHeld(ev.note());
                if (heldCount_ < kMaxHeld) held_[heldCount_++] = ev.note();
                uniPendingRelease_ = false;
                for (int vi = 0; vi < kNumVoices; ++vi) startVoice(vi, ev.note(), ev.velocity(), uniSpread(vi), from);
                lastUniNote_ = ev.note();
            }
            else
            {
                const int last = alloc_.lastNote();
                const int vi = alloc_.noteOn(ev.note(), ev.velocity());
                startVoice(vi, ev.note(), ev.velocity(), 0.0f, last);
            }
        }
        else if (ev.isNoteOff())
        {
            if (unison)
            {
                removeHeld(ev.note());
                if (heldCount_ > 0)
                {
                    const int n = held_[heldCount_ - 1];   // fall back to the last held key, no retrigger
                    for (auto& v : voices_) { v.note = n; if (get(kPorta) > 0.0f) v.glide.setTarget((float)n); else v.glide.snap((float)n); }
                    lastUniNote_ = n;
                }
                else if (controllers_.sustain()) uniPendingRelease_ = true;
                else for (int vi = 0; vi < kNumVoices; ++vi) releaseVoice(vi);
            }
            else
            {
                const int vi = alloc_.noteOff(ev.note());
                if (vi >= 0) releaseVoice(vi);
            }
        }
        else if (ev.isAllNotesOff())
        {
            alloc_.allNotesOff([this](int vi) { releaseVoice(vi); });
            heldCount_ = 0; uniPendingRelease_ = false;
            if (unison) for (int vi = 0; vi < kNumVoices; ++vi) releaseVoice(vi);
        }
        else if (ev.isController() && ev.controller() == cc::Sustain)
        {
            controllers_.handle(ev);
            const bool down = ev.controllerValue() >= 64;
            if (unison)
            {
                if (!down && uniPendingRelease_ && heldCount_ == 0)
                    for (int vi = 0; vi < kNumVoices; ++vi) releaseVoice(vi);
                if (!down) uniPendingRelease_ = false;
            }
            else alloc_.setSustain(down, [this](int vi) { releaseVoice(vi); });
        }
        else if (ev.isPitchBend())
        {
            controllers_.handle(ev);
            alloc_.setPitchBend(ev.pitchBend());
        }
        else controllers_.handle(ev);   // mod wheel, aftertouch
    }

    void removeHeld(int note)
    {
        int j = 0;
        for (int i = 0; i < heldCount_; ++i) if (held_[i] != note) held_[j++] = held_[i];
        heldCount_ = j;
    }

    // ----- rendering ----------------------------------------------------------
    float mgValue(float ph) const
    {
        switch (s_.mgwave)
        {
            case 1:  return 2.0f * ph - 1.0f;                  // Saw (rising)
            case 2:  return 1.0f - 2.0f * ph;                  // Ramp (falling)
            case 3:  return ph < 0.5f ? 1.0f : -1.0f;          // Square
            default: return ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph;   // Triangle
        }
    }

    void render(mpc::ProcessContext& ctx, int from, int to)
    {
        float* out = ctx.outputs[0];
        while (from < to)
        {
            const int n = (to - from) < kChunk ? (to - from) : kChunk;
            for (int i = 0; i < n; ++i)
            {
                controllers_.tick();
                mgBuf_[i] = mgValue(mgPhase_);
                mgPhase_ += mgInc_; if (mgPhase_ >= 1.0f) mgPhase_ -= 1.0f;
            }
            for (int vi = 0; vi < kNumVoices; ++vi)
            {
                Voice& v = voices_[vi];
                if (!v.aenv.active()) continue;
                renderVoice(v, out + from, n);
                if (!v.aenv.active() && s_.assign == 0) alloc_.voiceFinished(vi);
            }
            from += n;
        }
    }

    void updateVoiceControl(Voice& v, float mg, float fenv)
    {
        const float note = v.glide.value();
        const float at = controllers_.aftertouch();
        const float vibDepth = s_.mgosc + s_.whlmg * controllers_.modWheel() + s_.atvib * at;
        const float common = note + alloc_.bendSemitones() + mg * v.mgFade * vibDepth + v.uniCents * 0.01f;
        const float ab1 = (s_.absel == 1 || s_.absel == 3) ? v.ab : 0.0f;
        const float ab2 = (s_.absel == 2 || s_.absel == 3) ? v.ab : 0.0f;
        const float n1 = common + 12.0f * (float)s_.o1oct + ab1;
        const float n2 = common + 12.0f * (float)s_.o2oct + s_.interval + s_.detune * 0.01f + ab2;
        const float invSr = 1.0f / (float)sr_;
        v.inc1 = clamp(440.0f * std::exp2((n1 - 69.0f) * (1.0f / 12.0f)) * invSr, 0.0f, 0.49f);
        v.inc2 = clamp(440.0f * std::exp2((n2 - 69.0f) * (1.0f / 12.0f)) * invSr, 0.0f, 0.49f);
        v.tab1 = table(s_.o1wave, v.inc1);
        v.tab2 = table(s_.o2wave, v.inc2);

        const float velEg = lerp(1.0f, v.vel, s_.fvel);
        float oct = s_.egamt * fenv * velEg * 8.0f
                  + s_.kbt * (note - 60.0f) / 12.0f
                  + s_.mgvcf * mg * v.mgFade * 3.0f
                  + s_.atvcf * at * 4.0f
                  + (s_.bendvcf ? alloc_.bendSemitones() / 12.0f : 0.0f);
        const float fc = clamp(s_.cutoff * std::exp2(clamp(oct, -10.0f, 10.0f)), 16.0f, 0.45f * (float)sr_);
        const float g = std::tan(kPi * fc * invSr);
        v.G = g / (1.0f + g);
        v.G2 = v.G * v.G; v.G3 = v.G2 * v.G; v.G4 = v.G3 * v.G;
        v.oneMinusG = 1.0f - v.G;
        v.k = s_.reso * 4.2f;
        v.inGain = 0.6f * (1.0f + 0.45f * v.k);
    }

    void renderVoice(Voice& v, float* out, int n)
    {
        const float velAmp = lerp(1.0f, v.vel, s_.avel);
        for (int i = 0; i < n; ++i)
        {
            const float fenv = v.fenv.process(s_.fc);
            const float aenv = v.aenv.process(s_.ac);

            if ((v.ctrl++ % kCtrlInterval) == 0) updateVoiceControl(v, mgBuf_[i], fenv);
            v.glide.process();
            if (v.ab != 0.0f)
            {
                if (v.ab > 0.0f) { v.ab -= v.abStep; if (v.ab < 0.0f) v.ab = 0.0f; }
                else             { v.ab += v.abStep; if (v.ab > 0.0f) v.ab = 0.0f; }
            }
            if (v.mgFade < 1.0f) { v.mgFade += v.mgFadeStep; if (v.mgFade > 1.0f) v.mgFade = 1.0f; }

            float mix = readTable(v.tab1, v.ph1) * s_.o1lvl + readTable(v.tab2, v.ph2) * s_.o2lvl;
            if (s_.noise > 0.0f) mix += s_.noise * ((float)(int32_t)nextRand() * (1.0f / 2147483648.0f));
            v.ph1 += v.inc1; if (v.ph1 >= 1.0f) v.ph1 -= 1.0f;
            v.ph2 += v.inc2; if (v.ph2 >= 1.0f) v.ph2 -= 1.0f;

            // 4-pole zero-delay-feedback ladder, tanh at the input for warmth / bounded resonance
            const float x = mix * v.inGain;
            const float S = v.G3 * v.oneMinusG * v.s[0] + v.G2 * v.oneMinusG * v.s[1]
                          + v.G * v.oneMinusG * v.s[2] + v.oneMinusG * v.s[3];
            const float y4est = (v.G4 * x + S) / (1.0f + v.k * v.G4);
            float u = fastTanh(x - v.k * y4est);
            for (int st = 0; st < 4; ++st)
            {
                const float w = (u - v.s[st]) * v.G;
                const float y = w + v.s[st];
                v.s[st] = flushDenormal(y + w);
                u = y;
            }

            const float gain = aenv * velAmp * (1.0f + s_.atvca * controllers_.aftertouch()) * (s_.assign == 1 ? 0.14f : 0.30f);
            out[i] += u * gain;
        }
    }

    inline uint32_t nextRand() { rng_ = rng_ * 1664525u + 1013904223u; return rng_; }

    // ----- presets ----------------------------------------------------------
    void addPrograms()
    {
        auto program = [this](const char* name, std::initializer_list<std::pair<int, float>> o)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            for (const auto& p : o) v[(size_t)p.first] = p.second;
            addProgram(name, std::move(v));
        };
        program("Init DW", {});
        program("DW Brass", {{kO1Wave, 7}, {kO2Wave, 0}, {kO2Level, 80}, {kDetune, 10},
                             {kABSelect, 3}, {kABMode, 0}, {kABTime, 80}, {kABAmount, 1.0f},
                             {kCutoff, 700}, {kReso, 10}, {kEGAmount, 55}, {kFAtk, 70}, {kFDec, 500}, {kFBrk, 60},
                             {kFSlp, 1500}, {kFSus, 45}, {kFRel, 250}, {kAAtk, 30}, {kASus, 85}, {kARel, 250},
                             {kMGOsc, 0.15f}, {kMGDelay, 600}});
        program("Digi Piano", {{kO1Wave, 3}, {kO2Wave, 14}, {kO2Oct, 2}, {kO2Level, 25}, {kDetune, 4},
                               {kCutoff, 5000}, {kReso, 0}, {kEGAmount, 20}, {kFDec, 900}, {kFBrk, 30}, {kFSus, 0},
                               {kAAtk, 1}, {kADec, 1500}, {kABrk, 45}, {kASlp, 4000}, {kASus, 0}, {kARel, 400},
                               {kVCAVelo, 70}, {kVCFVelo, 60},
                               {kDlyTime, 18}, {kDlyFdbk, 0}, {kDlyModRate, 0.6f}, {kDlyMod, 40}, {kDlyLevel, 45}});
        program("Unison Bass", {{kO1Oct, 0}, {kO1Wave, 13}, {kO2Oct, 1}, {kO2Wave, 12}, {kO2Level, 80},
                                {kCutoff, 300}, {kReso, 45}, {kEGAmount, 60}, {kFDec, 250}, {kFBrk, 20}, {kFSlp, 600},
                                {kFSus, 10}, {kFRel, 120}, {kADec, 800}, {kABrk, 80}, {kASus, 75}, {kARel, 90},
                                {kAssign, 1}, {kUniDetune, 10}, {kPorta, 40}, {kVolume, -4}});
        program("Strings", {{kO1Wave, 9}, {kO2Wave, 0}, {kDetune, 14}, {kO2Level, 85},
                            {kCutoff, 2200}, {kReso, 5}, {kEGAmount, 15}, {kFAtk, 400}, {kFSus, 70},
                            {kAAtk, 350}, {kADec, 1000}, {kABrk, 100}, {kASus, 100}, {kARel, 900},
                            {kMGOsc, 0.12f}, {kMGRate, 5.5f}, {kMGDelay, 800},
                            {kDlyTime, 22}, {kDlyFdbk, 10}, {kDlyModRate, 0.4f}, {kDlyMod, 60}, {kDlyLevel, 50}});
        program("Organ", {{kO1Wave, 6}, {kO2Wave, 6}, {kO2Oct, 2}, {kO2Level, 55}, {kDetune, 2},
                          {kCutoff, 9000}, {kReso, 0}, {kEGAmount, 0}, {kAAtk, 3}, {kABrk, 100}, {kASus, 100},
                          {kARel, 60}, {kVCAVelo, 0}, {kMGOsc, 0.1f}, {kMGRate, 6.5f}, {kMGDelay, 0}});
        program("Clav Funk", {{kO1Wave, 5}, {kO2Wave, 10}, {kO2Level, 40},
                              {kCutoff, 1400}, {kReso, 35}, {kEGAmount, 60}, {kFDec, 220}, {kFBrk, 15}, {kFSus, 5},
                              {kADec, 700}, {kABrk, 60}, {kASlp, 1200}, {kASus, 0}, {kARel, 80},
                              {kVCFVelo, 60}, {kVCAVelo, 40}, {kVolume, 0}});
        program("Glass Bells", {{kO1Wave, 14}, {kO2Wave, 15}, {kO2Oct, 2}, {kO2Level, 45}, {kInterval, 4},
                                {kCutoff, 6000}, {kEGAmount, 10}, {kADec, 2500}, {kABrk, 35}, {kASlp, 6000},
                                {kASus, 0}, {kARel, 1500}, {kVCAVelo, 60},
                                {kDlyTime, 420}, {kDlyFdbk, 55}, {kDlyMod, 15}, {kDlyLevel, 45}});
        program("Whistle Lead", {{kO1Wave, 15}, {kO2Wave, 8}, {kO2Level, 30}, {kO1Oct, 2}, {kO2Oct, 1},
                                 {kABSelect, 1}, {kABMode, 0}, {kABTime, 150}, {kABAmount, 2},
                                 {kCutoff, 3500}, {kEGAmount, 10}, {kAAtk, 15}, {kABrk, 100}, {kASus, 100}, {kARel, 200},
                                 {kMGOsc, 0.35f}, {kMGRate, 5.5f}, {kMGDelay, 450}, {kPorta, 60},
                                 {kDlyTime, 320}, {kDlyFdbk, 40}, {kDlyLevel, 30}});
    }

    // ----- state ------------------------------------------------------------
    double sr_ = 44100.0;
    std::vector<float> tables_;
    Voice voices_[kNumVoices];
    VoiceAllocator<kNumVoices> alloc_;
    MidiControllers controllers_;
    Smoother volume_;
    float mgBuf_[kChunk] = {0};
    float mgPhase_ = 0.0f, mgInc_ = 0.0f;
    static constexpr int kMaxHeld = 16;
    int held_[kMaxHeld] = {0};
    int heldCount_ = 0;
    int lastUniNote_ = -1;
    bool uniPendingRelease_ = false;
    int lastAssign_ = 0;
    uint32_t rng_ = 0x12345678u;
    std::vector<float> delayBuf_;
    int delayMask_ = 0, writePos_ = 0;
    float dTimeSmooth_ = 0.0f, dModPhase_ = 0.0f, fbLp_ = 0.0f;
};

MPC_PLUGIN_ENTRY(DWG86)
