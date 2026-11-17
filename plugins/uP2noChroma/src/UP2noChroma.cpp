// uP2noChroma - a four-module colour/lo-fi multi-effect for the MPC, by uP2noGood
//
// SPDX-License-Identifier: MIT
//
// In the spirit of the Hologram Electronics Chroma Console pedal (layout and effect list), without
// its looper. Original code; nothing is taken from the pedal or any other product.
//
// Signal path:
//
//   in L/R --+--> [module 1] -> [module 2] -> [module 3] -> [module 4] --> wet
//            |      (order set by Routing; default Character > Movement > Diffusion > Texture)
//            +--------------------------------------------------------------> dry
//   out = (dry + (wet - dry) * Mix) * Output  -> knee clip (soft ceiling at 0 dBFS)
//
//   CHARACTER  Drive, Sweeten, Fuzz, Howl, Swell        knobs Tilt, Amount, Sensitivity, Vol
//   MOVEMENT   Doubler, Vibrato, Phaser, Tremolo, Pitch knobs Rate, Amount, Drift, Vol
//   DIFFUSION  Cascade, Reels, Space, Collage, Reverse  knobs Time, Amount, Drift, Vol (wet only)
//   TEXTURE    Filter, Squash, Cassette, Broken, Interference   Amount, Vol, Filter Style, Drift
//
//   Each module has an effect selector and an On switch. Changing either fades the module out and
//   back in over 15 ms (the new effect starts from silence: no stale echoes).
//   Input Level (Low .. Very High) is the pedal's calibration: it sets how hard the saturating and
//   compressing stages are hit (+12, +6, 0, -6 dB internally), not the output level.
//   Tap tempo is replaced by Sync: Vibrato/Phaser/Tremolo rates and Cascade/Reels/Collage times and
//   the Reverse segment follow the MPC tempo in note divisions.
//
// Parameters (order and ids are permanent): see the enum below and claude/chroma-console-plugin-plan.md.

#include "mpc_plugin.h"
#include "Common.h"
#include "Character.h"
#include "Movement.h"
#include "Diffusion.h"
#include "Texture.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace chroma;

namespace {

enum ParamIndex {
    kOutput = 0, kMix,
    kCharFx, kCharOn, kTilt, kCharAmt, kSens, kCharVol,
    kMoveFx, kMoveOn, kRate, kMoveAmt, kMoveDrift, kMoveVol,
    kDiffFx, kDiffOn, kTime, kDiffAmt, kDiffDrift, kDiffVol,
    kTexFx, kTexOn, kTexAmt, kTexVol,
    kFilterStyle, kInputLevel, kRouting,
    kMoveSync, kMoveDiv, kDiffSync, kDiffDiv,
    kSpread, kPingPong, kDuck, kTexDrift, kTexTone,
    kNumParams
};

enum Module { mChar = 0, mMove, mDiff, mTex, kNumModules };
const char kModLetter[kNumModules] = { 'C', 'M', 'D', 'T' };
const float kCal[4] = { 4.0f, 2.0f, 1.0f, 0.5f };          // Low, Medium, High, Very High
constexpr float kVolMin = -30.0f;                          // bottom of the Vol knobs = off
constexpr int kChunk = 128;

// the 24 orders of the four modules, in lexicographic order (index 0 = C > M > D > T)
struct Orders
{
    int o[24][4];
    Orders()
    {
        int p[4] = {0, 1, 2, 3}, k = 0;
        do { for (int i = 0; i < 4; ++i) o[k][i] = p[i]; ++k; } while (std::next_permutation(p, p + 4));
    }
};
const Orders& orders() { static Orders s; return s; }

float volGain(float db) { return db <= kVolMin + 0.05f ? 0.0f : dbToGain(db); }

} // namespace

class UP2noChroma : public mpc::Plugin
{
public:
    UP2noChroma() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        std::vector<std::string> routes;
        for (int k = 0; k < 24; ++k)
        {
            std::string s;
            for (int i = 0; i < 4; ++i) { if (i) s += ">"; s += kModLetter[orders().o[k][i]]; }
            routes.push_back(s);
        }
        addParam(floatParam ("output",      "Output",       -24.0f, 12.0f, 0.0f, "dB", 1));
        addParam(floatParam ("mix",         "Mix",          0.0f, 100.0f, 100.0f, "%", 0));
        addParam(choiceParam("char_fx",     "Char FX",      {"Drive", "Sweeten", "Fuzz", "Howl", "Swell"}, 0));
        addParam(boolParam  ("char_on",     "Char On",      true));
        addParam(floatParam ("tilt",        "Tilt",         -100.0f, 100.0f, 0.0f, "", 0));
        addParam(floatParam ("char_amt",    "Char Amt",     0.0f, 100.0f, 30.0f, "%", 0));
        addParam(floatParam ("sens",        "Sensitivity",  -100.0f, 100.0f, 0.0f, "", 0));
        addParam(floatParam ("char_vol",    "Char Vol",     kVolMin, 12.0f, 0.0f, "dB", 1));
        addParam(choiceParam("move_fx",     "Move FX",      {"Doubler", "Vibrato", "Phaser", "Tremolo", "Pitch"}, 1));
        addParam(boolParam  ("move_on",     "Move On",      true));
        addParam(floatParam ("rate",        "Rate",         0.0f, 100.0f, 35.0f, "", 0));
        addParam(floatParam ("move_amt",    "Move Amt",     0.0f, 100.0f, 25.0f, "%", 0));
        addParam(floatParam ("move_drift",  "Move Drift",   0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam ("move_vol",    "Move Vol",     kVolMin, 12.0f, 0.0f, "dB", 1));
        addParam(choiceParam("diff_fx",     "Diff FX",      {"Cascade", "Reels", "Space", "Collage", "Reverse"}, 2));
        addParam(boolParam  ("diff_on",     "Diff On",      true));
        addParam(floatParam ("time",        "Time",         0.0f, 100.0f, 50.0f, "", 0));
        addParam(floatParam ("diff_amt",    "Diff Amt",     0.0f, 100.0f, 30.0f, "%", 0));
        addParam(floatParam ("diff_drift",  "Diff Drift",   0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam ("diff_vol",    "Diff Vol",     kVolMin, 12.0f, 0.0f, "dB", 1));
        addParam(choiceParam("tex_fx",      "Tex FX",       {"Filter", "Squash", "Cassette", "Broken", "Interference"}, 2));
        addParam(boolParam  ("tex_on",      "Tex On",       false));
        addParam(floatParam ("tex_amt",     "Tex Amt",      0.0f, 100.0f, 30.0f, "", 0));
        addParam(floatParam ("tex_vol",     "Tex Vol",      kVolMin, 12.0f, 0.0f, "dB", 1));
        addParam(choiceParam("filter_style","Filter Style", {"LPF", "Tilt", "HPF"}, 1));
        addParam(choiceParam("input_level", "Input Level",  {"Low", "Medium", "High", "Very High"}, 2));
        addParam(choiceParam("routing",     "Routing",      routes, 0));
        addParam(boolParam  ("move_sync",   "Move Sync",    false));
        addParam(choiceParam("move_div",    "Move Div",     divisionNames(), kDivisionQuarter));
        addParam(boolParam  ("diff_sync",   "Diff Sync",    false));
        addParam(choiceParam("diff_div",    "Diff Div",     divisionNames(), 9 /* 1/8D */));
        addParam(floatParam ("spread",      "Spread",       0.0f, 100.0f, 50.0f, "%", 0));
        addParam(boolParam  ("pingpong",    "Ping-Pong",    false));
        addParam(floatParam ("duck",        "Duck",         0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam ("tex_drift",   "Tex Drift",    0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam ("tex_tone",    "Tex Tone",     -100.0f, 100.0f, 0.0f, "", 0));
        if (numParams() != kNumParams) mpc::log("uP2noChroma: parameter count mismatch");

        //  Out Mix | CFx COn Tilt CAmt Sens CVol | MFx MOn Rate MAmt MDr MVol | DFx DOn Time DAmt DDr DVol | TFx TOn TAmt TVol | FSt In Rt | MSy MDv DSy DDv | Spr PP Duck TDr TTn
        addProgram("Init", {0, 100,  0, 1, 0, 30, 0, 0,  1, 1, 35, 25, 0, 0,  2, 1, 50, 30, 0, 0,  2, 0, 30, 0,  1, 2, 0,  0, 10, 0, 9,  50, 0, 0, 0, 0});
        addProgram("Tape Memory", {0, 100,  1, 1, -10, 40, 0, 0,  1, 1, 30, 35, 60, 0,  1, 1, 45, 40, 30, 0,  2, 1, 55, 0,  1, 2, 0,  0, 10, 0, 9,  50, 0, 0, 20, -10});
        addProgram("Fuzz Cathedral", {0, 100,  2, 1, 20, 70, 0, 0,  2, 1, 20, 50, 15, 0,  2, 1, 85, 55, 30, 0,  2, 0, 30, 0,  1, 2, 0,  0, 10, 0, 9,  70, 0, 0, 0, 0});
        addProgram("Broken Radio", {0, 100,  0, 1, 10, 25, 0, 0,  3, 1, 55, 40, 30, 0,  0, 1, 40, 35, 20, 0,  4, 1, 35, 0,  1, 2, 0,  0, 10, 0, 9,  80, 0, 0, 30, 0});
        addProgram("Backwards Dream", {0, 100,  4, 1, -20, 45, 20, 4,  1, 1, 25, 40, 50, 0,  4, 1, 50, 70, 30, 0,  0, 1, 70, 0,  0, 2, 0,  0, 10, 0, 9,  60, 0, 0, 0, 20});
        addProgram("Stab Collage", {0, 100,  3, 1, 60, 55, 0, -3,  4, 1, 100, 30, 0, 0,  3, 1, 45, 50, 30, 0,  3, 1, 25, 0,  1, 2, 0,  0, 10, 1, 7,  50, 1, 30, 20, 0});
        addProgram("Squashed Room", {0, 100,  1, 0, 0, 30, 0, 0,  0, 1, 20, 50, 20, 0,  2, 1, 20, 35, 10, 0,  1, 1, 60, 0,  1, 2, 0,  0, 10, 0, 9,  60, 0, 0, 0, 10});
    }

    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        sr_ = (float)sampleRate;
        chr_.prepare(sr_); mov_.prepare(sr_); dif_.prepare(sr_); tex_.prepare(sr_);
        reset();
    }

    void reset() override
    {
        chr_.reset(); mov_.reset(); dif_.reset(); tex_.reset();
        for (int m = 0; m < kNumModules; ++m)
        {
            slot_[m].cur = currentFx(m); slot_[m].g = isOn(m) ? 1.0f : 0.0f; slot_[m].dirty = false;
            vol_[m] = Ramp(); smMain_[m] = BlockSmooth(); smAmt_[m] = BlockSmooth(); smSecond_[m] = BlockSmooth();
        }
        mix_ = Ramp(); out_ = Ramp();
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int N = ctx.numFrames;
        const float* inL = ctx.numInputs > 0 && ctx.inputs ? ctx.inputs[0] : nullptr;
        const float* inR = ctx.numInputs > 1 && ctx.inputs ? ctx.inputs[1] : inL;
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        const mpc::Transport& tr = ctx.transport;
        const double bpm = tr.tempoValid ? tr.bpm : 120.0;

        for (int start = 0; start < N; start += kChunk)
        {
            const int n = std::min(kChunk, N - start);
            for (int i = 0; i < n; ++i)
            {
                const float l = inL ? inL[start + i] : 0.0f, r = inR ? inR[start + i] : l;
                dryL_[i] = wL_[i] = l; dryR_[i] = wR_[i] = r;
            }
            Ctl base;
            base.sr = sr_; base.bpm = bpm;
            base.playing = tr.playing; base.ppqValid = tr.ppqValid;
            base.ppq = tr.ppqPos + (double)start / (double)sr_ * safeBpm(bpm) / 60.0;
            base.cal = kCal[mpc::clamp(getInt(kInputLevel), 0, 3)];
            base.filterStyle = getInt(kFilterStyle);
            base.spread = get(kSpread) * 0.01f;
            base.duck = get(kDuck) * 0.01f;
            base.pingpong = getBool(kPingPong);
            base.tone = get(kTexTone) * 0.01f;

            const int route = mpc::clamp(getInt(kRouting), 0, 23);
            for (int k = 0; k < 4; ++k) runModule(orders().o[route][k], n, base);

            mix_.set(get(kMix) * 0.01f, n);
            out_.set(dbToGain(get(kOutput)), n);
            for (int i = 0; i < n; ++i)
            {
                const float mx = mix_.next(), og = out_.next();
                const float l = (dryL_[i] + (wL_[i] - dryL_[i]) * mx) * og;
                const float r = (dryR_[i] + (wR_[i] - dryR_[i]) * mx) * og;
                outL[start + i] = sanitize(kneeClip(l));
                if (outR) outR[start + i] = sanitize(kneeClip(r));
            }
        }
    }

    bool formatValue(int index, float v, std::string& out) override
    {
        char b[48];
        switch (index)
        {
        case kCharVol: case kMoveVol: case kDiffVol: case kTexVol:
            if (v <= kVolMin + 0.05f) { out = "Off"; return true; }
            std::snprintf(b, sizeof b, "%+.1f dB", (double)v); out = b; return true;
        case kRate:
        {
            const int fx = getInt(kMoveFx); const float r = v * 0.01f;
            if (fx == mDoubler) std::snprintf(b, sizeof b, "%.0f ms", (double)rateToDoublerMs(r));
            else if (fx == mPitch) std::snprintf(b, sizeof b, "%+d st", rateToSemis(r));
            else if (getBool(kMoveSync)) std::snprintf(b, sizeof b, "%s sync", divisionName(getInt(kMoveDiv)));
            else std::snprintf(b, sizeof b, "%.2f Hz", (double)rateToHz(r));
            out = b; return true;
        }
        case kTime:
        {
            const int fx = getInt(kDiffFx); const float t = v * 0.01f; const bool sync = getBool(kDiffSync);
            if (fx == dSpace) std::snprintf(b, sizeof b, "%s %.1f s", spaceName(t), (double)spaceRt60(t));
            else if (fx == dReverse) std::snprintf(b, sizeof b, "%.2fx", (double)reverseSpeed(t));
            else if (sync) std::snprintf(b, sizeof b, "%s sync", divisionName(getInt(kDiffDiv)));
            else std::snprintf(b, sizeof b, "%.0f ms", (double)(fx == dCascade ? cascadeMs(t) : fx == dReels ? reelsMs(t) : collageMs(t)));
            out = b; return true;
        }
        case kTexAmt:
        {
            if (getInt(kTexFx) == tFilter)
            {
                bool hp = false; const float hz = filterHz(v * 0.01f, getInt(kFilterStyle), hp);
                if (hz < 0.0f) out = "Open";
                else { std::snprintf(b, sizeof b, hz >= 1000.0f ? "%s %.1f kHz" : "%s %.0f Hz", hp ? "HP" : "LP", (double)(hz >= 1000.0f ? hz * 0.001f : hz)); out = b; }
            }
            else { std::snprintf(b, sizeof b, "%.0f %%", (double)v); out = b; }
            return true;
        }
        default: return false;
        }
    }

private:
    struct Slot { int cur = 0; float g = 0.0f; bool dirty = false; float inFade = 1.0f; };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "uP2noChroma";
        info.vendor       = "uP2noGood";
        info.uniqueId     = MPC_FOURCC('U', 'k', 'l', '6');   // == "uid" in plugin.json
        info.version      = 100;                                // 0.1.0.0
        info.isInstrument = false;
        info.numInputs    = 2;
        info.numOutputs   = 2;
        info.tailSamples  = 0;                                  // unknown: depends on Diffusion
        return info;
    }

    int currentFx(int m) const
    {
        static const int fxParam[kNumModules] = { kCharFx, kMoveFx, kDiffFx, kTexFx };
        return mpc::clamp(getInt(fxParam[m]), 0, 4);
    }
    bool isOn(int m) const
    {
        static const int onParam[kNumModules] = { kCharOn, kMoveOn, kDiffOn, kTexOn };
        return getBool(onParam[m]);
    }
    void resetModule(int m)
    {
        switch (m) { case mChar: chr_.reset(); break; case mMove: mov_.reset(); break; case mDiff: dif_.reset(); break; default: tex_.reset(); break; }
    }

    // One module on the work buffers wL_/wR_, with a 15 ms fade when it is switched or changes effect.
    void runModule(int m, int n, const Ctl& base)
    {
        Slot& s = slot_[m];
        const int want = currentFx(m);
        const bool on = isOn(m);
        const float rate = (float)n / (0.015f * sr_);

        // at silence: take the new effect (from a clean state)
        if (s.g <= 0.0f)
        {
            if (!on) { s.dirty = true; return; }                  // bypassed: dry passes untouched
            if (want != s.cur || s.dirty) { s.cur = want; resetModule(m); s.dirty = false; s.inFade = 0.0f; }
        }
        const float target = (on && want == s.cur) ? 1.0f : 0.0f;
        const float g0 = s.g;
        const float g1 = target > g0 ? std::min(1.0f, g0 + rate) : target < g0 ? std::max(0.0f, g0 - rate) : g0;
        s.g = g1;

        // module controls, smoothed per block
        Ctl c = base;
        float mainV = 0.0f, amtV = 0.0f, secV = 0.0f, volDb = 0.0f;
        switch (m)
        {
        case mChar: mainV = get(kTilt) * 0.01f; amtV = get(kCharAmt) * 0.01f; secV = get(kSens) * 0.01f; volDb = get(kCharVol); break;
        case mMove: mainV = get(kRate) * 0.01f; amtV = get(kMoveAmt) * 0.01f; secV = get(kMoveDrift) * 0.01f; volDb = get(kMoveVol);
                    c.sync = getBool(kMoveSync); c.divBeats = divisionBeats(getInt(kMoveDiv)); break;
        case mDiff: mainV = get(kTime) * 0.01f; amtV = get(kDiffAmt) * 0.01f; secV = get(kDiffDrift) * 0.01f; volDb = get(kDiffVol);
                    c.sync = getBool(kDiffSync); c.divBeats = divisionBeats(getInt(kDiffDiv)); break;
        default:    mainV = 0.0f; amtV = get(kTexAmt) * 0.01f; secV = get(kTexDrift) * 0.01f; volDb = get(kTexVol); break;
        }
        const bool discreteMain = (m == mMove && s.cur == mPitch);
        c.main = discreteMain ? mainV : smMain_[m].update(mainV, 40.0f, n, sr_);
        c.amt = smAmt_[m].update(amtV, 30.0f, n, sr_);
        c.second = smSecond_[m].update(secV, 40.0f, n, sr_);
        const float vg = volGain(volDb);

        float* src[2] = { wL_, wR_ };
        float tmpL[kChunk], tmpR[kChunk];
        if (s.inFade < 1.0f)
        {
            // a freshly reset effect also gets its input faded in, so delay lines and envelope
            // followers start from a smooth edge instead of a cut-in signal
            const float f0 = s.inFade, f1 = std::min(1.0f, f0 + 3.0f * rate), df = (f1 - f0) / (float)n;
            float f = f0;
            for (int i = 0; i < n; ++i) { f += df; tmpL[i] = src[0][i] * f; tmpR[i] = src[1][i] * f; }
            s.inFade = f1;
        }
        else
            for (int i = 0; i < n; ++i) { tmpL[i] = src[0][i]; tmpR[i] = src[1][i]; }

        if (m == mDiff)
        {
            c.vol = vg;                                       // Diffusion: Vol scales the wet part only
            dif_.process(s.cur, tmpL, tmpR, n, c);
            vol_[m].set(1.0f, n);
        }
        else
        {
            switch (m)
            {
            case mChar: chr_.process(s.cur, tmpL, tmpR, n, c); break;
            case mMove: mov_.process(s.cur, tmpL, tmpR, n, c); break;
            default:    tex_.process(s.cur, tmpL, tmpR, n, c); break;
            }
            vol_[m].set(vg, n);
        }

        const float dg = (g1 - g0) / (float)n;
        float g = g0;
        for (int i = 0; i < n; ++i)
        {
            g += dg;
            const float v = vol_[m].next();
            src[0][i] += (tmpL[i] * v - src[0][i]) * g;
            src[1][i] += (tmpR[i] * v - src[1][i]) * g;
        }
    }

    Character chr_; Movement mov_; Diffusion dif_; Texture tex_;
    Slot slot_[kNumModules];
    Ramp vol_[kNumModules], mix_, out_;
    BlockSmooth smMain_[kNumModules], smAmt_[kNumModules], smSecond_[kNumModules];
    float dryL_[kChunk] = {}, dryR_[kChunk] = {}, wL_[kChunk] = {}, wR_[kChunk] = {};
    float sr_ = 44100.0f;
};

MPC_PLUGIN_ENTRY(UP2noChroma)
