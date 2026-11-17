// uP2noGoog - a monophonic three-oscillator synth for Akai MPC standalones by uP2noGood,
// in the spirit of the 1970s Minimoog Model D. Not a circuit clone: no schematics,
// firmware or code from Moog or anyone else are used. "Minimoog" and "Model D" are
// trademarks of Moog Music Inc.; this project is not affiliated with Moog.
//
// Signal flow (everything up to the filter runs at 2x the host rate):
//
//   Osc 1 ─┐                                 Filter contour ─► Amount of Contour
//   Osc 2 ─┤                                        │
//   Osc 3 ─┼─► MIXER (5 ch, on/off + volume) ─► soft saturation ─► 4-pole nonlinear ladder (24 dB/oct)
//   Noise ─┤      (hot levels overdrive)                                │  Cutoff, Emphasis,
//   Feedback (output fed back in) ─┘                                    │  Kbd Ctrl 1 (1/3) + 2 (2/3)
//                                                                       ▼
//                                     2x decimation ─► VCA (Loudness contour) ─► Volume ─► out (mono → L+R)
//
// Controllers: Tune, Glide (+ Glide On), Modulation Mix (Osc 3 <-> Noise), Osc Mod and Filter Mod
// switches, Osc 3 Control (keyboard tracking for Osc 3), Decay switch (release = decay time).
// Modulation amount = Mod Amt knob + mod wheel (CC1) + aftertouch (if AT>Mod is up).
// Note logic: mono, low-note priority, single trigger (legato does not restart the contours),
// glide always from the last pitch.
//
// Extras beyond the original (all default to off): velocity to loudness / filter, aftertouch to
// modulation / filter, pitch-bend range, pre-filter Drive, oscillator Drift, A-440 reference tone.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/noise.h"
#include "dsp/biquad.h"

#include <cmath>
#include <cstdint>

namespace {

using mpc::clamp;

// ---------------------------------------------------------------------------
// Parameters. ORDER AND IDS ARE PERMANENT: only ever append at the end.
// ---------------------------------------------------------------------------
enum ParamIndex
{
    kVolume = 0,     // dB, master output (parameter 0 is nudged by the host on load: harmless)
    kTune,           // semitones, all oscillators
    kGlide,          // ms
    kGlideOn,        // bool
    kModMix,         // 0..10, 0 = Osc 3, 10 = Noise
    kOscMod,         // bool, modulation -> Osc 1 + 2 pitch
    kOsc3Ctrl,       // bool, Osc 3 follows the keyboard
    kFiltMod,        // bool, modulation -> cutoff
    kDecaySw,        // bool, release uses the decay times
    kOsc1Range,      // LO 32' 16' 8' 4' 2'
    kOsc1Wave,
    kOsc2Range,
    kOsc2Freq,       // semitones relative to Osc 1
    kOsc2Wave,
    kOsc3Range,
    kOsc3Freq,
    kOsc3Wave,
    kOsc1On, kOsc1Vol,
    kOsc2On, kOsc2Vol,
    kOsc3On, kOsc3Vol,
    kFbOn,   kFbVol,
    kNoiseOn, kNoiseVol, kNoiseColor,
    kCutoff,         // Hz
    kEmphasis,       // 0..10
    kContourAmt,     // 0..10
    kKbd1,           // bool, 1/3 keyboard tracking
    kKbd2,           // bool, 2/3 keyboard tracking
    kFAttack, kFDecay, kFSustain,
    kLAttack, kLDecay, kLSustain,
    kDrive,          // dB before the filter (extra)
    kDrift,          // 0..10 analogue pitch wander (extra)
    kBendRange,      // semitones (extra)
    kVeloLoud,       // % (extra)
    kVeloFilt,       // % (extra)
    kAtMod,          // % aftertouch -> modulation amount (extra)
    kAtFilt,         // % aftertouch -> cutoff (extra)
    kA440,           // bool, reference tone
    kModAmt,         // 0..10, modulation amount without touching the wheel
    kNumParams
};

// Range switch: octave offsets relative to 8'. LO is 7 octaves down, and in LO the Frequency
// knob sweeps about 6 octaves instead of a fifth (useful for Osc 3 as an LFO).
constexpr float kRangeOct[6] = { -7.0f, -2.0f, -1.0f, 0.0f, 1.0f, 2.0f };
constexpr int   kRangeLo = 0;

// Fixed pulse widths for the two rectangular waves (by ear, the original has no PWM).
constexpr float kWidePw   = 0.30f;
constexpr float kNarrowPw = 0.12f;

// 2^x, good to ~1e-5 relative over the range we use; no std::pow per sample.
inline float fastExp2(float x)
{
    x = clamp(x, -40.0f, 40.0f);
    const float fi = std::floor(x);
    const float f  = x - fi;
    const float p  = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * (0.0096181f + f * 0.0013334f))));
    return std::ldexp(p, (int)fi);
}

inline float polyBlep(float t, float dt)
{
    if (t < dt)        { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}

// One oscillator. Waves: 0 Tri, 1 Tri-Saw (Osc 3: Reverse Saw), 2 Saw, 3 Square, 4 Wide, 5 Narrow.
struct Osc
{
    float phase = 0.0f;
    bool  hasRevSaw = false;

    float process(int wave, float inc)
    {
        const float t = phase;
        float y;
        switch (wave)
        {
            case 0: y = tri(t); break;
            case 1:
                if (hasRevSaw) y = -saw(t, inc);
                else           y = 0.5f * (tri(t) + saw(t, inc)) * 1.3f;   // "shark tooth" blend
                break;
            case 2: y = saw(t, inc); break;
            case 3: y = pulse(t, inc, 0.5f); break;
            case 4: y = pulse(t, inc, kWidePw); break;
            default: y = pulse(t, inc, kNarrowPw); break;
        }
        phase += inc;
        if (phase >= 1.0f) phase -= 1.0f;
        return y;
    }

    // At 2x oversampling a plain triangle aliases very little (its harmonics fall at 12 dB/oct).
    static float tri(float t) { return 1.0f - 4.0f * std::fabs(t - 0.5f); }
    static float saw(float t, float dt) { return 2.0f * t - 1.0f - polyBlep(t, dt); }
    static float pulse(float t, float dt, float pw)
    {
        float y = t < pw ? 1.0f : -1.0f;
        y += polyBlep(t, dt);
        float t2 = t + 1.0f - pw; if (t2 >= 1.0f) t2 -= 1.0f;
        y -= polyBlep(t2, dt);
        return y - (2.0f * pw - 1.0f);   // AC-coupled like the hardware: no DC offset
    }
};

// Analogue-style contour: RC-shaped attack (aims past 1 and stops at 1), exponential decay to
// sustain, exponential release. Times are roughly "how long until the segment is done".
struct Contour
{
    enum Stage { Idle, Attack, Decay, Release };
    Stage stage = Idle;
    float level = 0.0f;
    float aCoef = 1.0f, dCoef = 1.0f, rCoef = 1.0f, sustain = 1.0f;

    static float coefFor(float seconds, float sr, float shape)
    {
        return 1.0f - std::exp(-shape / std::fmax(1.0f, seconds * sr));
    }
    void set(float attackMs, float decayMs, float sus01, float releaseMs, float sr)
    {
        aCoef   = coefFor(attackMs * 0.001f, sr, 1.466f);   // reach 1.0 from 0 aiming at 1.3
        dCoef   = coefFor(decayMs * 0.001f, sr, 2.3f);      // ~90 % of the way
        rCoef   = coefFor(releaseMs * 0.001f, sr, 2.3f);
        sustain = sus01;
    }
    void gateOn()  { stage = Attack; }
    void gateOff() { if (stage != Idle) stage = Release; }
    float next()
    {
        switch (stage)
        {
            case Attack:
                level += aCoef * (1.3f - level);
                if (level >= 1.0f) { level = 1.0f; stage = Decay; }
                break;
            case Decay:   level = mpc::dsp::flushDenormal(level + dCoef * (sustain - level)); break;
            case Release:
                level += rCoef * (0.0f - level);
                if (level < 1.0e-5f) { level = 0.0f; stage = Idle; }
                break;
            case Idle: level = 0.0f; break;
        }
        return level;
    }
};

// Four-pole transistor-ladder low-pass: four one-pole stages in series with global feedback for
// resonance. Solved without a unit delay in the feedback loop (Zavalishin's "zero-delay feedback"
// method) so tuning and self-oscillation hold across the whole range; the transistor-ladder
// saturation is modelled by a tanh at the input where the feedback is summed (after Huovilainen,
// DAFx 2004, who places the dominant nonlinearity there). Self-oscillates at the top of Emphasis.
struct Ladder
{
    float s[4] = { 0, 0, 0, 0 };
    void reset() { for (int i = 0; i < 4; ++i) s[i] = 0.0f; }

    // G: g/(1+g) with g = tan(pi*fc/fs); k: feedback 0..~4.3 (4 = edge of self-oscillation)
    float process(float x, float G, float k)
    {
        const float oneMinusG = 1.0f - G;
        // y4 = G^4*u + S, where S is what the stored states contribute.
        const float S = ((oneMinusG * s[0] * G + oneMinusG * s[1]) * G + oneMinusG * s[2]) * G + oneMinusG * s[3];
        const float G4 = G * G * G * G;
        float u = (x - k * S) / (1.0f + k * G4);
        u = mpc::dsp::fastTanh(u);
        float in = u;
        for (int i = 0; i < 4; ++i)
        {
            const float v = (in - s[i]) * G;
            const float y = v + s[i];
            s[i] = mpc::dsp::flushDenormal(y + v);
            in = y;
        }
        return in;
    }
};

} // namespace

class UP2noGoog : public mpc::Plugin
{
public:
    UP2noGoog() : mpc::Plugin(makeInfo())
    {
        const std::vector<std::string> ranges = { "LO", "32'", "16'", "8'", "4'", "2'" };
        const std::vector<std::string> waves  = { "Tri", "TriSaw", "Saw", "Square", "Wide", "Narrow" };
        const std::vector<std::string> waves3 = { "Tri", "RevSaw", "Saw", "Square", "Wide", "Narrow" };

        addParam(mpc::floatParam("volume",     "Volume",     -40.0f, 6.0f, -6.0f, "dB", 1));
        addParam(mpc::floatParam("tune",       "Tune",       -2.0f, 2.0f, 0.0f, "st", 2));
        addParam(mpc::logParam  ("glide",      "Glide",      1.0f, 10000.0f, 120.0f, "ms", 0));
        addParam(mpc::boolParam ("glide_on",   "Glide On",   false));
        addParam(mpc::floatParam("mod_mix",    "Mod Mix",    0.0f, 10.0f, 0.0f, "", 1));
        addParam(mpc::boolParam ("osc_mod",    "Osc Mod",    false));
        addParam(mpc::boolParam ("osc3_ctrl",  "Osc3 Ctrl",  true));
        addParam(mpc::boolParam ("filt_mod",   "Filter Mod", false));
        addParam(mpc::boolParam ("decay_sw",   "Decay Sw",   false));
        addParam(mpc::choiceParam("osc1_range","Osc1 Range", ranges, 3));
        addParam(mpc::choiceParam("osc1_wave", "Osc1 Wave",  waves, 2));
        addParam(mpc::choiceParam("osc2_range","Osc2 Range", ranges, 3));
        addParam(mpc::floatParam("osc2_freq",  "Osc2 Freq",  -7.0f, 7.0f, 0.0f, "st", 2));
        addParam(mpc::choiceParam("osc2_wave", "Osc2 Wave",  waves, 2));
        addParam(mpc::choiceParam("osc3_range","Osc3 Range", ranges, 3));
        addParam(mpc::floatParam("osc3_freq",  "Osc3 Freq",  -7.0f, 7.0f, 0.0f, "st", 2));
        addParam(mpc::choiceParam("osc3_wave", "Osc3 Wave",  waves3, 2));
        addParam(mpc::boolParam ("osc1_on",    "Osc1 On",    true));
        addParam(mpc::floatParam("osc1_vol",   "Osc1 Vol",   0.0f, 10.0f, 8.0f, "", 1));
        addParam(mpc::boolParam ("osc2_on",    "Osc2 On",    false));
        addParam(mpc::floatParam("osc2_vol",   "Osc2 Vol",   0.0f, 10.0f, 8.0f, "", 1));
        addParam(mpc::boolParam ("osc3_on",    "Osc3 On",    false));
        addParam(mpc::floatParam("osc3_vol",   "Osc3 Vol",   0.0f, 10.0f, 8.0f, "", 1));
        addParam(mpc::boolParam ("fb_on",      "Fdbk On",    false));
        addParam(mpc::floatParam("fb_vol",     "Fdbk Vol",   0.0f, 10.0f, 0.0f, "", 1));
        addParam(mpc::boolParam ("noise_on",   "Noise On",   false));
        addParam(mpc::floatParam("noise_vol",  "Noise Vol",  0.0f, 10.0f, 5.0f, "", 1));
        addParam(mpc::choiceParam("noise_col", "Noise Col",  { "White", "Pink" }, 0));
        addParam(mpc::logParam  ("cutoff",     "Cutoff",     20.0f, 20000.0f, 1200.0f, "Hz", 0));
        addParam(mpc::floatParam("emphasis",   "Emphasis",   0.0f, 10.0f, 2.0f, "", 1));
        addParam(mpc::floatParam("contour",    "Contour Amt",0.0f, 10.0f, 5.0f, "", 1));
        addParam(mpc::boolParam ("kbd1",       "Kbd Ctrl 1", false));
        addParam(mpc::boolParam ("kbd2",       "Kbd Ctrl 2", false));
        addParam(mpc::logParam  ("f_attack",   "F Attack",   1.0f, 10000.0f, 5.0f, "ms", 0));
        addParam(mpc::logParam  ("f_decay",    "F Decay",    2.0f, 10000.0f, 400.0f, "ms", 0));
        addParam(mpc::floatParam("f_sustain",  "F Sustain",  0.0f, 10.0f, 3.0f, "", 1));
        addParam(mpc::logParam  ("l_attack",   "L Attack",   1.0f, 10000.0f, 2.0f, "ms", 0));
        addParam(mpc::logParam  ("l_decay",    "L Decay",    2.0f, 10000.0f, 400.0f, "ms", 0));
        addParam(mpc::floatParam("l_sustain",  "L Sustain",  0.0f, 10.0f, 10.0f, "", 1));
        addParam(mpc::floatParam("drive",      "Drive",      0.0f, 12.0f, 0.0f, "dB", 1));
        addParam(mpc::floatParam("drift",      "Drift",      0.0f, 10.0f, 2.0f, "", 1));
        addParam(mpc::intParam  ("bend_range", "Bend Range", 1, 12, 2, "st"));
        addParam(mpc::floatParam("velo_loud",  "Velo>Loud",  0.0f, 100.0f, 0.0f, "%", 0));
        addParam(mpc::floatParam("velo_filt",  "Velo>Filt",  0.0f, 100.0f, 0.0f, "%", 0));
        addParam(mpc::floatParam("at_mod",     "AT>Mod",     0.0f, 100.0f, 0.0f, "%", 0));
        addParam(mpc::floatParam("at_filt",    "AT>Filt",    0.0f, 100.0f, 0.0f, "%", 0));
        addParam(mpc::boolParam ("a440",       "A-440",      false));
        addParam(mpc::floatParam("mod_amt",    "Mod Amt",    0.0f, 10.0f, 0.0f, "", 1));

        addPresets();
    }

    void prepare(double sampleRate, int maxBlockSize) override
    {
        (void)maxBlockSize;
        sr_  = (float)sampleRate;
        sr2_ = 2.0f * sr_;
        // 4th-order Butterworth at ~0.43 x host rate (19 kHz at 44.1k), run at 2x, for decimation.
        const float dc = 0.43f * sr_;
        dec1_.setCoeffs(mpc::dsp::BiquadCoeffs::lowpass(sr2_, dc, 0.5412f));
        dec2_.setCoeffs(mpc::dsp::BiquadCoeffs::lowpass(sr2_, dc, 1.3066f));
        reset();
    }

    void reset() override
    {
        // Free-running oscillators with different start phases, like the hardware.
        osc_[0] = Osc(); osc_[0].phase = 0.00f;
        osc_[1] = Osc(); osc_[1].phase = 0.37f;
        osc_[2] = Osc(); osc_[2].phase = 0.71f; osc_[2].hasRevSaw = true;
        ladder_.reset();
        dec1_.reset(); dec2_.reset();
        fEnv_ = Contour(); lEnv_ = Contour();
        numHeld_ = 0;
        pitch_ = targetPitch_ = 60.0f;
        velocity_ = 1.0f;
        bend_ = bendTarget_ = 0.0f;
        wheel_ = 0.0f; aftertouch_ = atSmoothed_ = 0.0f;
        lastOut_ = 0.0f;
        a440Phase_ = 0.0f;
        for (int i = 0; i < 3; ++i) { drift_[i] = 0.0f; driftTarget_[i] = 0.0f; }
        driftCounter_ = 0;
        gain_ = dbToGain(get(kVolume));
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int n = ctx.numFrames;
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;

        // ---- block-rate parameter reads ----------------------------------
        const bool  decaySw   = getBool(kDecaySw);
        const float fRelease  = decaySw ? get(kFDecay) : 8.0f;
        const float lRelease  = decaySw ? get(kLDecay) : 8.0f;
        fEnv_.set(get(kFAttack), get(kFDecay), get(kFSustain) * 0.1f, fRelease, sr_);
        lEnv_.set(get(kLAttack), get(kLDecay), get(kLSustain) * 0.1f, lRelease, sr_);

        const float tune      = get(kTune);
        const float glideCoef = getBool(kGlideOn) ? 1.0f - std::exp(-2.3f / std::fmax(1.0f, get(kGlide) * 0.001f * sr_)) : 1.0f;
        const float modMix    = get(kModMix) * 0.1f;
        const bool  oscMod    = getBool(kOscMod);
        const bool  filtMod   = getBool(kFiltMod);
        const bool  osc3Kbd   = getBool(kOsc3Ctrl);

        int   range[3] = { getInt(kOsc1Range), getInt(kOsc2Range), getInt(kOsc3Range) };
        int   wave[3]  = { getInt(kOsc1Wave),  getInt(kOsc2Wave),  getInt(kOsc3Wave) };
        float freq[3]  = { 0.0f, get(kOsc2Freq), get(kOsc3Freq) };
        float level[3] = { getBool(kOsc1On) ? volCurve(get(kOsc1Vol)) : 0.0f,
                           getBool(kOsc2On) ? volCurve(get(kOsc2Vol)) : 0.0f,
                           getBool(kOsc3On) ? volCurve(get(kOsc3Vol)) : 0.0f };
        const float noiseLvl  = getBool(kNoiseOn) ? volCurve(get(kNoiseVol)) : 0.0f;
        const bool  pink      = getInt(kNoiseColor) == 1;
        const float fbLvl     = getBool(kFbOn) ? volCurve(get(kFbVol)) * 1.5f : 0.0f;
        const float preGain   = 0.5f * dbToGain(get(kDrive));

        // Fixed per-oscillator offsets in semitones (range, tune, frequency knob).
        float offset[3];
        for (int i = 0; i < 3; ++i)
        {
            const bool lo = range[i] == kRangeLo;
            offset[i] = 12.0f * kRangeOct[clamp(range[i], 0, 5)] + tune + freq[i] * (lo ? 5.0f : 1.0f);
        }

        const float cutoffOct  = std::log2(get(kCutoff));
        const float emph       = get(kEmphasis) * 0.1f;
        const float k          = 4.3f * emph;
        const float resoComp   = 1.0f + 0.35f * emph;
        const float contourOct = get(kContourAmt) * 0.5f;              // 0..5 octaves at full contour
        const float kbdTrack   = (getBool(kKbd1) ? 1.0f / 3.0f : 0.0f) + (getBool(kKbd2) ? 2.0f / 3.0f : 0.0f);
        const float bendRange  = (float)getInt(kBendRange);
        const float veloLoud   = get(kVeloLoud) * 0.01f;
        const float veloFilt   = get(kVeloFilt) * 0.01f;
        const float atMod      = get(kAtMod) * 0.01f;
        const float atFilt     = get(kAtFilt) * 0.01f;
        const bool  a440       = getBool(kA440);
        const float modBase    = get(kModAmt) * 0.1f;
        const float targetGain = dbToGain(get(kVolume));

        updateDrift(n);

        int ev = 0;
        for (int i = 0; i < n; ++i)
        {
            while (ev < ctx.numMidiEvents && ctx.midi[ev].sampleOffset <= i)
                handleMidi(ctx.midi[ev++]);

            // ---- control signals (host rate) ------------------------------
            pitch_      += glideCoef * (targetPitch_ - pitch_);
            bend_       += 0.002f * (bendTarget_ - bend_);
            atSmoothed_ += 0.002f * (aftertouch_ - atSmoothed_);
            gain_       += 0.002f * (targetGain - gain_);

            const float fe = fEnv_.next();
            const float le = lEnv_.next();
            const float amount = clamp(modBase + wheel_ + atMod * atSmoothed_, 0.0f, 1.0f);
            const float kbdPitch = pitch_ + bend_ * bendRange;

            float baseCut = cutoffOct + contourOct * fe
                          + kbdTrack * (pitch_ - 60.0f) / 12.0f
                          + veloFilt * (velocity_ - 1.0f) * 2.0f
                          + atFilt * atSmoothed_ * 2.0f;

            float filtered = 0.0f;
            for (int sub = 0; sub < 2; ++sub)
            {
                // Osc 3 first: it is also the modulation source.
                const float p3 = (osc3Kbd ? kbdPitch : 60.0f) + offset[2] + drift_[2];
                const float o3 = osc_[2].process(wave[2], hz(p3) / sr2_);
                const float nz = pink ? pinkNoise_.process() * 3.0f : whiteNoise_.process();
                const float modSrc = (1.0f - modMix) * o3 + modMix * nz;
                const float pitchMod = oscMod ? modSrc * 12.0f * amount * amount : 0.0f;

                const float o1 = osc_[0].process(wave[0], hz(kbdPitch + offset[0] + drift_[0] + pitchMod) / sr2_);
                const float o2 = osc_[1].process(wave[1], hz(kbdPitch + offset[1] + drift_[1] + pitchMod) / sr2_);

                // ---- mixer: hot levels overdrive it, like the original ------
                float mix = level[0] * o1 + level[1] * o2 + level[2] * o3 + noiseLvl * nz + fbLvl * lastOut_;
                mix = mpc::dsp::fastTanh(mix * preGain);

                // ---- ladder filter ------------------------------------------
                const float cutOct = baseCut + (filtMod ? modSrc * amount * 3.0f : 0.0f);
                const float fc = clamp(fastExp2(cutOct), 10.0f, 0.42f * sr2_);
                const float tg = std::tan(mpc::dsp::kPi * fc / sr2_);
                // A whisper of circuit noise lets the filter start to self-oscillate with no input, like the hardware.
                float y = ladder_.process(mix * 0.9f + 1.0e-4f * nz, tg / (1.0f + tg), k) * resoComp;
                y = mpc::dsp::flushDenormal(dec2_.process(dec1_.process(y)));
                if (sub == 1) filtered = y;
            }

            // ---- VCA ---------------------------------------------------------
            const float vGain = 1.0f - veloLoud * (1.0f - velocity_);
            float out = filtered * le * vGain;
            lastOut_ = mpc::dsp::flushDenormal(out);

            if (a440)
            {
                out += 0.25f * std::sin(mpc::dsp::kTwoPi * a440Phase_);
                a440Phase_ += 440.0f / sr_;
                if (a440Phase_ >= 1.0f) a440Phase_ -= 1.0f;
            }

            out = mpc::dsp::flushDenormal(mpc::dsp::sanitize(out * gain_ * 2.0f));
            outL[i] = out;
            if (outR) outR[i] = out;
        }
        while (ev < ctx.numMidiEvents) handleMidi(ctx.midi[ev++]);
    }

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "uP2noGoog";
        info.vendor       = "uP2noGood";
        info.uniqueId     = MPC_FOURCC('U', 'D', '7', 'V');
        info.version      = 202;            // 0.2.2.0
        info.isInstrument = true;
        info.numInputs    = 0;
        info.numOutputs   = 2;
        info.tailSamples  = 0;
        return info;
    }

    static float dbToGain(float dB) { return std::pow(10.0f, dB / 20.0f); }
    static float volCurve(float v) { const float x = v * 0.1f; return x * x; }   // audio-taper pot
    static float hz(float midiPitch) { return 440.0f * fastExp2((midiPitch - 69.0f) * (1.0f / 12.0f)); }

    // Slow random walk per oscillator, new target about every 60 ms.
    void updateDrift(int frames)
    {
        const float amt = get(kDrift) * 0.1f * 0.07f;
        driftCounter_ += frames;
        if (driftCounter_ > (int)(0.06f * sr_))
        {
            driftCounter_ = 0;
            for (int i = 0; i < 3; ++i) driftTarget_[i] = driftRng_.nextBipolar() * amt;
        }
        const float c = clamp((float)frames / (0.4f * sr_), 0.0f, 1.0f);
        for (int i = 0; i < 3; ++i) drift_[i] += c * (driftTarget_[i] - drift_[i]);
    }

    // ---- mono note handling: low-note priority, single trigger -----------
    void handleMidi(const mpc::MidiEvent& e)
    {
        if (e.isNoteOn())
        {
            const bool wasEmpty = numHeld_ == 0;
            if (numHeld_ < kMaxHeld) held_[numHeld_++] = e.note();
            if (wasEmpty)
            {
                velocity_ = e.velocity() / 127.0f;
                fEnv_.gateOn();
                lEnv_.gateOn();
            }
            targetPitch_ = (float)lowestHeld();
        }
        else if (e.isNoteOff())
        {
            for (int i = 0; i < numHeld_; ++i)
                if (held_[i] == e.note()) { held_[i] = held_[--numHeld_]; break; }
            if (numHeld_ == 0) { fEnv_.gateOff(); lEnv_.gateOff(); }
            else targetPitch_ = (float)lowestHeld();
        }
        else if (e.isAllNotesOff())
        {
            numHeld_ = 0;
            fEnv_.gateOff(); lEnv_.gateOff();
        }
        else if (e.isPitchBend())
        {
            bendTarget_ = e.pitchBend() / 8192.0f;
        }
        else if (e.isAftertouch())
        {
            aftertouch_ = e.data[1] / 127.0f;
        }
        else if (e.isController() && e.controller() == 1)
        {
            wheel_ = e.controllerValue() / 127.0f;
        }
    }

    int lowestHeld() const
    {
        int lo = 127;
        for (int i = 0; i < numHeld_; ++i) if (held_[i] < lo) lo = held_[i];
        return lo;
    }

    // ---- built-in programs ------------------------------------------------
    struct Set { int index; float value; };
    void program(const char* name, std::initializer_list<Set> changes)
    {
        std::vector<float> v((size_t)numParams());
        for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
        for (const Set& s : changes) v[(size_t)s.index] = s.value;
        addProgram(name, v);
    }

    void addPresets()
    {
        program("Init", {});
        program("Fat Bass", {
            { kOsc1Range, 1 }, { kOsc1Wave, 2 }, { kOsc2On, 1 }, { kOsc2Range, 1 }, { kOsc2Wave, 3 }, { kOsc2Freq, -0.08f },
            { kOsc3On, 1 }, { kOsc3Range, 2 }, { kOsc3Wave, 2 }, { kOsc3Freq, 0.07f }, { kOsc2Vol, 7 }, { kOsc3Vol, 6 },
            { kCutoff, 300 }, { kEmphasis, 3 }, { kContourAmt, 6 }, { kFAttack, 2 }, { kFDecay, 250 }, { kFSustain, 2 },
            { kLDecay, 300 }, { kLSustain, 9 }, { kKbd1, 1 }, { kVeloFilt, 40 } });
        program("Classic Lead", {
            { kOsc1Range, 3 }, { kOsc1Wave, 2 }, { kOsc2On, 1 }, { kOsc2Range, 3 }, { kOsc2Wave, 2 }, { kOsc2Freq, 0.1f },
            { kOsc3On, 1 }, { kOsc3Range, 2 }, { kOsc3Wave, 3 }, { kOsc3Vol, 6 },
            { kCutoff, 900 }, { kEmphasis, 4 }, { kContourAmt, 5 }, { kFAttack, 20 }, { kFDecay, 600 }, { kFSustain, 5 },
            { kGlideOn, 1 }, { kGlide, 90 }, { kKbd1, 1 }, { kKbd2, 1 }, { kAtMod, 60 }, { kVeloLoud, 30 } });
        program("Brass", {
            { kOsc1Range, 2 }, { kOsc1Wave, 2 }, { kOsc2On, 1 }, { kOsc2Range, 2 }, { kOsc2Wave, 2 }, { kOsc2Freq, -0.12f },
            { kOsc3On, 1 }, { kOsc3Range, 3 }, { kOsc3Wave, 2 }, { kOsc3Freq, 0.1f }, { kOsc3Vol, 5 },
            { kCutoff, 400 }, { kEmphasis, 1.5f }, { kContourAmt, 6 }, { kFAttack, 90 }, { kFSustain, 6 },
            { kLAttack, 30 }, { kLSustain, 9 }, { kDecaySw, 1 }, { kLDecay, 250 }, { kFDecay, 300 }, { kKbd2, 1 },
            { kVeloFilt, 50 }, { kAtFilt, 30 } });
        program("Flute", {
            { kOsc1Range, 3 }, { kOsc1Wave, 0 }, { kNoiseOn, 1 }, { kNoiseVol, 2 }, { kNoiseColor, 1 },
            { kCutoff, 1800 }, { kEmphasis, 0 }, { kContourAmt, 2 }, { kFAttack, 60 }, { kFSustain, 7 },
            { kLAttack, 70 }, { kLSustain, 9 }, { kDecaySw, 1 }, { kLDecay, 200 }, { kKbd1, 1 }, { kKbd2, 1 },
            { kOscMod, 1 }, { kOsc3Range, 0 }, { kOsc3Ctrl, 0 }, { kOsc3Wave, 0 }, { kOsc3Freq, 3.0f }, { kModAmt, 1.5f }, { kAtMod, 50 } });
        program("Pluck", {
            { kOsc1Range, 3 }, { kOsc1Wave, 4 }, { kOsc2On, 1 }, { kOsc2Range, 2 }, { kOsc2Wave, 2 }, { kOsc2Vol, 6 },
            { kCutoff, 250 }, { kEmphasis, 5 }, { kContourAmt, 7 }, { kFAttack, 1 }, { kFDecay, 180 }, { kFSustain, 0 },
            { kLAttack, 1 }, { kLDecay, 600 }, { kLSustain, 0 }, { kDecaySw, 1 }, { kKbd1, 1 }, { kVeloLoud, 50 },
            { kVeloFilt, 60 } });
        program("Osc3 Wobble", {
            { kOsc1Range, 1 }, { kOsc1Wave, 2 }, { kOsc2On, 1 }, { kOsc2Range, 2 }, { kOsc2Wave, 3 }, { kOsc2Freq, 0.05f },
            { kOsc3Range, 0 }, { kOsc3Ctrl, 0 }, { kOsc3Wave, 0 }, { kOsc3Freq, 1.0f },
            { kFiltMod, 1 }, { kModAmt, 7 }, { kCutoff, 350 }, { kEmphasis, 6 }, { kContourAmt, 2 },
            { kLSustain, 10 }, { kDrive, 4 } });
        program("Screamer", {
            { kOsc1Range, 2 }, { kOsc1Wave, 2 }, { kOsc1Vol, 10 }, { kOsc2On, 1 }, { kOsc2Vol, 10 }, { kOsc2Range, 3 },
            { kOsc2Freq, 0.15f }, { kFbOn, 1 }, { kFbVol, 6 }, { kCutoff, 1500 }, { kEmphasis, 7 }, { kContourAmt, 4 },
            { kFSustain, 6 }, { kGlideOn, 1 }, { kGlide, 60 }, { kKbd1, 1 }, { kKbd2, 1 }, { kAtFilt, 50 } });
        program("Noise Wind", {
            { kOsc1On, 0 }, { kNoiseOn, 1 }, { kNoiseVol, 9 }, { kNoiseColor, 0 }, { kCutoff, 600 }, { kEmphasis, 8.5f },
            { kContourAmt, 4 }, { kFAttack, 1500 }, { kFDecay, 3000 }, { kFSustain, 3 }, { kLAttack, 800 }, { kDecaySw, 1 },
            { kLDecay, 2500 }, { kKbd1, 1 }, { kKbd2, 1 }, { kFiltMod, 1 }, { kOsc3Range, 0 }, { kOsc3Ctrl, 0 },
            { kOsc3Freq, -3.0f }, { kModAmt, 4 } });
    }

    static constexpr int kMaxHeld = 16;

    Osc      osc_[3];
    Ladder   ladder_;
    Contour  fEnv_, lEnv_;
    mpc::dsp::Biquad dec1_, dec2_;
    mpc::dsp::WhiteNoise whiteNoise_;
    mpc::dsp::PinkNoise  pinkNoise_;
    mpc::dsp::Xorshift32 driftRng_;

    float sr_ = 44100.0f, sr2_ = 88200.0f;
    int   held_[kMaxHeld] = {};
    int   numHeld_ = 0;
    float pitch_ = 60.0f, targetPitch_ = 60.0f, velocity_ = 1.0f;
    float bend_ = 0.0f, bendTarget_ = 0.0f;
    float wheel_ = 0.0f, aftertouch_ = 0.0f, atSmoothed_ = 0.0f;
    float lastOut_ = 0.0f, gain_ = 0.5f, a440Phase_ = 0.0f;
    float drift_[3] = {}, driftTarget_[3] = {};
    int   driftCounter_ = 0;
};

MPC_PLUGIN_ENTRY(UP2noGoog)
