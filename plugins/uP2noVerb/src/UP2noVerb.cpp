// uP2noVerb - a vintage-digital style reverb for the MPC, by uP2noGood
//
// SPDX-License-Identifier: MIT
//
// In the spirit of the hardware digital reverbs of the late 1970s and 1980s and of the plugins that
// celebrate them. Original code. The reverb tank follows the well-known published "figure-8"
// allpass-loop topology (J. Dattorro, "Effect Design Part 1", J. Audio Eng. Soc. 1997), which is
// itself a description of that school of hall/plate algorithm; nothing here comes from any product.
//
// Signal path:
//
//   in L/R -> predelay -> 4 series allpasses per channel (Early Diffusion) --+--> early line (taps)
//                                                                            |
//        +-------------------------------------------------------------------+
//        v                                                   (L feeds half A, R feeds half B)
//   half A: [modulated allpass] -> delay -> damping -> [allpass] -> delay --> into half B
//   half B: [modulated allpass] -> delay -> damping -> [allpass] -> delay --> into half A
//
//   wet = taps from both halves (L and R use different taps) [+ diffused input, by Attack]
//   wet -> Low Cut -> High Cut -> colour filter;   out = dry * (1 - Mix) + wet * Mix, then Output
//
//   damping (once per half-loop): gain from Decay; treble above High Freq turned down by High
//   Shelf; bass below Bass Xover decays Bass Mult times as long.
//
// Modes are tunings of this network (delay scale and ratios, diffusion, modulation), except:
//   Ambience  early taps plus a short tail; Attack is the early/late balance.
//   Nonlin    no tank: a flat burst of taps (gated reverb). Size = length; Decay is not used.
//
// Color: 1970s = dark (about 10 kHz), deep noisy modulation, soft clipping in the loop.
//        1980s = full bandwidth, noisy modulation, soft clipping in the loop.
//        Now   = clean, gentle sine modulation.
//
// Parameters (order and ids are permanent):
//   0 Output  1 Mix  2 Predelay  3 Decay  4 Mode  5 Color  6 Size  7 Attack
//   8 High Freq  9 High Shelf  10 Bass Xover  11 Bass Mult  12 Mod Rate  13 Mod Depth
//   14 High Cut  15 Low Cut  16 Early Diff  17 Late Diff

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/onepole.h"
#include "dsp/smoother.h"

#include <cmath>
#include <vector>

using namespace mpc::dsp;

namespace {

enum ParamIndex { kOutput = 0, kMix, kPredelay, kDecay, kMode, kColor, kSize, kAttack,
                  kHighFreq, kHighShelf, kBassXover, kBassMult, kModRate, kModDepth,
                  kHighCut, kLowCut, kEarlyDiff, kLateDiff, kNumParams };

enum Mode { mConcertHall = 0, mBrightHall, mPlate, mRoom, mChamber, mAmbience, mCathedral, mNonlin, kNumModes };

// A delay line with a fractional (linear) read. Memory is allocated in alloc() only.
struct Line
{
    std::vector<float> b; int w = 0, mask = 0;
    void alloc(int samples) { int s = 16; while (s < samples + 8) s <<= 1; b.assign((size_t)s, 0.0f); mask = s - 1; w = 0; }
    void clear() { std::fill(b.begin(), b.end(), 0.0f); }
    void write(float x) { b[(size_t)w] = x; w = (w + 1) & mask; }
    float read(float d) const                      // d samples ago (1 = the last one written)
    {
        const float p = (float)w - d;
        const int i = (int)std::floor(p);
        const float f = p - (float)i;
        const float a = b[(size_t)(i & mask)], c = b[(size_t)((i + 1) & mask)];
        return a + f * (c - a);
    }
    float allpass(float x, float d, float g)        // Schroeder allpass around this line
    {
        const float z = read(d);
        const float v = x - g * z;
        write(v);
        return z + g * v;
    }
};

// Delay times in milliseconds for one tuning: input allpasses (4), then per half:
// modulated allpass, delay 1, allpass 2, delay 2.
struct Tuning { float in[4]; float a[4]; float b[4]; };
const Tuning kTuneHall  = { {4.77f, 3.60f, 12.73f, 9.31f}, {22.58f, 149.6f, 60.5f, 125.0f}, {30.5f, 141.7f, 89.2f, 106.3f} };
const Tuning kTuneRoom  = { {3.11f, 4.93f, 7.87f, 11.19f}, {17.3f, 97.1f, 43.7f, 71.9f},   {23.9f, 83.3f, 55.1f, 101.3f} };
const Tuning kTunePlate = { {5.31f, 3.89f, 10.07f, 7.43f}, {19.1f, 113.9f, 37.3f, 89.7f},  {26.3f, 104.3f, 47.9f, 79.9f} };

struct ModeSpec { const Tuning* t; float scale, earlyDiff, lateDiff, mod, bright; };
const ModeSpec kModes[kNumModes] = {
    { &kTuneHall,  1.30f, 0.70f, 0.85f, 1.25f, 0.0f },   // Concert Hall: big, sparse start, lush chorus
    { &kTuneHall,  1.22f, 0.80f, 0.90f, 1.00f, 1.0f },   // Bright Hall: same family, brighter
    { &kTunePlate, 0.72f, 1.00f, 1.00f, 0.80f, 0.6f },   // Plate: dense and bright from the first moment
    { &kTuneRoom,  0.45f, 0.95f, 0.95f, 0.55f, 0.2f },   // Room: small and quick to build
    { &kTuneRoom,  0.70f, 1.00f, 1.00f, 0.70f, 0.3f },   // Chamber: medium, dense
    { &kTuneRoom,  0.38f, 0.90f, 0.90f, 0.50f, 0.4f },   // Ambience: early taps + short tail
    { &kTuneHall,  1.90f, 0.55f, 0.80f, 1.10f, -0.3f },  // Cathedral: very large, slow build, darker
    { &kTunePlate, 1.00f, 1.00f, 1.00f, 0.00f, 0.5f },   // Nonlin: gated burst (tank unused)
};

constexpr float kMaxScale   = 1.90f * 1.60f * 1.02f;      // largest mode scale x largest Size
constexpr float kMaxPreMs   = 300.0f;
constexpr float kEarlyMaxMs = 640.0f;                      // early / nonlin tap line
constexpr int   kEarlyTaps  = 6;
constexpr int   kNonlinTaps = 16;
// tap positions as fractions of the window (fixed, irregular), L and R different
const float kEarlyL[kEarlyTaps] = {0.11f, 0.23f, 0.37f, 0.52f, 0.71f, 0.93f};
const float kEarlyR[kEarlyTaps] = {0.14f, 0.27f, 0.33f, 0.58f, 0.66f, 1.00f};
const float kEarlyG[kEarlyTaps] = {0.80f, -0.70f, 0.62f, 0.54f, -0.46f, 0.40f};

} // namespace

class UP2noVerb : public mpc::Plugin
{
public:
    UP2noVerb() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        addParam(floatParam ("output",   "Output",     -24.0f, 12.0f, 0.0f, "dB", 1));
        addParam(floatParam ("mix",      "Mix",        0.0f, 100.0f, 30.0f, "%", 0));
        addParam(floatParam ("predelay", "Predelay",   0.0f, kMaxPreMs, 12.0f, "ms", 0));
        addParam(logParam   ("decay",    "Decay",      0.2f, 70.0f, 2.2f, "s", 2));
        addParam(choiceParam("mode",     "Mode",       {"Concert Hall", "Bright Hall", "Plate", "Room", "Chamber", "Ambience", "Cathedral", "Nonlin"}, 0));
        addParam(choiceParam("color",    "Color",      {"1970s", "1980s", "Now"}, 1));
        addParam(floatParam ("size",     "Size",       0.0f, 100.0f, 55.0f, "%", 0));
        addParam(floatParam ("attack",   "Attack",     0.0f, 100.0f, 30.0f, "%", 0));
        addParam(logParam   ("highfreq", "High Freq",  500.0f, 16000.0f, 5500.0f, "Hz", 0));
        addParam(floatParam ("highshelf","High Shelf", -24.0f, 0.0f, -8.0f, "dB", 1));
        addParam(logParam   ("bassxover","Bass Xover", 80.0f, 2000.0f, 400.0f, "Hz", 0));
        addParam(floatParam ("bassmult", "Bass Mult",  0.25f, 4.0f, 1.25f, "x", 2));
        addParam(logParam   ("modrate",  "Mod Rate",   0.1f, 5.0f, 0.6f, "Hz", 2));
        addParam(floatParam ("moddepth", "Mod Depth",  0.0f, 100.0f, 45.0f, "%", 0));
        addParam(logParam   ("highcut",  "High Cut",   1000.0f, 20000.0f, 9000.0f, "Hz", 0));
        addParam(logParam   ("lowcut",   "Low Cut",    10.0f, 1500.0f, 60.0f, "Hz", 0));
        addParam(floatParam ("earlydiff","Early Diff", 0.0f, 100.0f, 80.0f, "%", 0));
        addParam(floatParam ("latediff", "Late Diff",  0.0f, 100.0f, 70.0f, "%", 0));
        if (numParams() != kNumParams) mpc::log("uP2noVerb: parameter count mismatch");

        //                        Out   Mix  Pre   Decay Mode Col Size  Att   HiF     HiSh   BsX    BsM    Rate  Dep   HiCut    LoCut  ED    LD
        addProgram("Concert Hall", {0.0f, 30.0f, 12.0f, 2.2f, 0, 1, 55.0f, 30.0f, 5500.0f, -8.0f, 400.0f, 1.25f, 0.6f, 45.0f, 9000.0f,  60.0f, 80.0f, 70.0f});
        addProgram("Vocal Plate",  {0.0f, 25.0f, 30.0f, 1.6f, 2, 1, 50.0f, 10.0f, 7000.0f, -5.0f, 300.0f, 0.90f, 0.8f, 35.0f, 11000.0f, 150.0f, 100.0f, 80.0f});
        addProgram("Snare Room",   {0.0f, 22.0f, 0.0f,  0.7f, 3, 2, 45.0f, 5.0f,  6000.0f, -6.0f, 250.0f, 0.80f, 0.5f, 20.0f, 10000.0f, 120.0f, 95.0f, 80.0f});
        addProgram("70s Chamber",  {0.0f, 30.0f, 8.0f,  1.4f, 4, 0, 55.0f, 20.0f, 4500.0f, -10.0f, 350.0f, 1.10f, 0.9f, 60.0f, 8000.0f, 80.0f, 90.0f, 75.0f});
        addProgram("Drum Ambience",{0.0f, 20.0f, 0.0f,  0.5f, 5, 2, 50.0f, 35.0f, 7000.0f, -4.0f, 300.0f, 0.80f, 0.5f, 15.0f, 12000.0f, 150.0f, 90.0f, 70.0f});
        addProgram("Blade Synth",  {0.0f, 45.0f, 40.0f, 9.0f, 6, 0, 80.0f, 70.0f, 4000.0f, -9.0f, 500.0f, 1.60f, 0.4f, 70.0f, 7000.0f, 50.0f, 60.0f, 70.0f});
        addProgram("Gated Snare",  {0.0f, 35.0f, 0.0f,  1.0f, 7, 1, 45.0f, 20.0f, 6000.0f, -4.0f, 300.0f, 1.00f, 0.5f, 0.0f,  9000.0f, 120.0f, 100.0f, 80.0f});
    }

    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        sr_ = (float)sampleRate;
        const float ms = sr_ * 0.001f;
        float inMax[4] = {0, 0, 0, 0}, aMax[4] = {0, 0, 0, 0}, bMax[4] = {0, 0, 0, 0};
        for (const ModeSpec& m : kModes)
            for (int i = 0; i < 4; ++i)
            {
                inMax[i] = std::max(inMax[i], m.t->in[i]);
                aMax[i]  = std::max(aMax[i],  m.t->a[i]);
                bMax[i]  = std::max(bMax[i],  m.t->b[i]);
            }
        for (int c = 0; c < 2; ++c)
        {
            pre_[c].alloc((int)(kMaxPreMs * ms) + 4);
            early_[c].alloc((int)(kEarlyMaxMs * ms) + 4);
            for (int i = 0; i < 4; ++i) inAp_[c][i].alloc((int)(inMax[i] * ms) + 4);
        }
        for (int i = 0; i < 4; ++i)
        {
            const float extra = (i == 0) ? 3.0f : 0.0f;                  // room for the modulation
            tank_[0][i].alloc((int)((aMax[i] * kMaxScale + extra) * ms) + 8);
            tank_[1][i].alloc((int)((bMax[i] * kMaxScale + extra) * ms) + 8);
        }
        scale_.setTime(180.0f, sampleRate);
        preLen_.setTime(60.0f, sampleRate);
        mix_.setTime(20.0f, sampleRate);
        out_.setTime(20.0f, sampleRate);
        for (int c = 0; c < 2; ++c) for (auto& f : colour_[c]) f.setCutoff(13500.0f, sampleRate);
        wobble_[0].setCutoff(2.5f, sampleRate); wobble_[1].setCutoff(3.1f, sampleRate);
        reset();
    }

    void reset() override
    {
        for (int c = 0; c < 2; ++c)
        {
            pre_[c].clear(); early_[c].clear();
            for (auto& l : inAp_[c]) l.clear();
            for (auto& l : tank_[c]) l.clear();
            hfLp_[c].reset(); bassLp_[c].reset(); lowCut_[c].reset(); hiCut_[c][0].reset(); hiCut_[c][1].reset();
            for (auto& f : colour_[c]) f.reset();
            wobble_[c].reset();
        }
        scale_.snap(targetScale());
        preLen_.snap(get(kPredelay) * 0.001f * sr_ + 1.0f);
        mix_.snap(get(kMix) * 0.01f);
        out_.snap(dbToGain(get(kOutput)));
        lfo_ = 0.0f;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int n = ctx.numFrames;
        if (ctx.numOutputs < 1 || n <= 0) return;
        const bool haveIn = ctx.inputs != nullptr && ctx.numInputs > 0 && ctx.inputs[0] != nullptr;
        const float* inL = haveIn ? ctx.inputs[0] : nullptr;
        const float* inR = (haveIn && ctx.numInputs > 1 && ctx.inputs[1] != nullptr) ? ctx.inputs[1] : inL;
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;

        // ----- per block ------------------------------------------------------
        const int mode = mpc::clamp(getInt(kMode), 0, (int)kNumModes - 1);
        const int color = getInt(kColor);                         // 0 = 1970s, 1 = 1980s, 2 = Now
        const ModeSpec& M = kModes[mode];
        const Tuning& T = *M.t;
        const float ms = sr_ * 0.001f;
        const bool nonlin = mode == mNonlin, ambience = mode == mAmbience;

        scale_.setTarget(targetScale());
        preLen_.setTarget(get(kPredelay) * ms + 1.0f);
        mix_.setTarget(get(kMix) * 0.01f);
        out_.setTarget(dbToGain(get(kOutput)));

        const float attack = get(kAttack) * 0.01f;
        const float ed = get(kEarlyDiff) * 0.01f * M.earlyDiff;
        const float ld = get(kLateDiff) * 0.01f * M.lateDiff;
        const float gIn1 = 0.75f * ed, gIn2 = 0.625f * ed;
        const float gAp1 = -0.70f * ld, gAp2 = 0.50f * ld;

        // decay: gain per half-loop from the loop time at the target scale
        float rt = get(kDecay);
        if (ambience) rt = 0.15f + 0.45f * rt;                    // Ambience keeps its tail short
        const float sc = scale_.target();
        const float tA = (T.a[0] + T.a[1] + T.a[2] + T.a[3]) * sc * 0.001f;
        const float tB = (T.b[0] + T.b[1] + T.b[2] + T.b[3]) * sc * 0.001f;
        const float bm = get(kBassMult);
        const float gMid[2] = { std::min(0.9995f, std::pow(10.0f, -3.0f * tA / rt)), std::min(0.9995f, std::pow(10.0f, -3.0f * tB / rt)) };
        const float gLow[2] = { std::min(0.9995f, std::pow(10.0f, -3.0f * tA / (rt * bm))), std::min(0.9995f, std::pow(10.0f, -3.0f * tB / (rt * bm))) };
        const float shelfDb = get(kHighShelf) * (1.0f - 0.45f * mpc::clamp(M.bright, 0.0f, 1.0f)) + (M.bright < 0.0f ? 3.0f * M.bright : 0.0f);
        const float gShelf = dbToGain(shelfDb * 0.35f);           // per half-loop
        const float hf = mpc::clamp(get(kHighFreq) * (1.0f + 0.5f * M.bright), 300.0f, 0.45f * sr_);
        for (int c = 0; c < 2; ++c)
        {
            hfLp_[c].setCutoff(hf, sr_);
            bassLp_[c].setCutoff(get(kBassXover), sr_);
            lowCut_[c].setCutoff(get(kLowCut), sr_);
            const float hc = std::min(get(kHighCut), 0.45f * sr_);
            hiCut_[c][0].setCutoff(hc, sr_); hiCut_[c][1].setCutoff(hc, sr_);
        }
        // modulation
        const float colMod = color == 0 ? 1.4f : (color == 1 ? 1.0f : 0.7f);
        const float colNoise = color == 0 ? 1.0f : (color == 1 ? 0.6f : 0.0f);
        const float depth = get(kModDepth) * 0.01f * M.mod * colMod * 1.0f * ms;          // up to ~1 ms (x mode, colour)
        const float lfoInc = get(kModRate) / sr_;
        const bool clip = color != 2;
        // level: longer decays carry more energy, keep the wet level roughly even
        const float norm = mpc::clamp(std::sqrt(2.0f / rt), 0.30f, 1.5f);
        const float earlyGain = nonlin ? 0.0f : (ambience ? 1.0f - 0.7f * attack : (1.0f - attack) * 0.45f);
        const float tankGain = nonlin ? 0.0f : (ambience ? 0.3f + 0.7f * attack : 1.0f) * norm;
        const float winMs = nonlin ? 80.0f + 5.0f * get(kSize) : 12.0f + 0.6f * get(kSize);   // nonlin 80..580 ms, early 12..72 ms
        const float win = std::min(winMs, kEarlyMaxMs - 2.0f) * ms;

        for (int i = 0; i < n; ++i)
        {
            const float dryL = inL ? inL[i] : 0.0f, dryR = inR ? inR[i] : 0.0f;
            const float s = scale_.process() * ms;                // samples per millisecond of tuning
            const float pl = preLen_.process();

            // predelay and input diffusion
            float x[2];
            x[0] = pre_[0].read(pl); pre_[0].write(dryL);
            x[1] = pre_[1].read(pl); pre_[1].write(dryR);
            for (int c = 0; c < 2; ++c)
            {
                // input allpasses keep their own size: they set the texture, not the room
                x[c] = inAp_[c][0].allpass(x[c], T.in[0] * ms, gIn1);
                x[c] = inAp_[c][1].allpass(x[c], T.in[1] * ms, gIn1);
                x[c] = inAp_[c][2].allpass(x[c], T.in[2] * ms, gIn2);
                x[c] = inAp_[c][3].allpass(x[c], T.in[3] * ms, gIn2);
                early_[c].write(x[c]);
            }

            float wetL = 0.0f, wetR = 0.0f;
            if (nonlin)
            {
                // a flat burst: taps spread over the window, Attack tilts it from flat to reversed
                for (int k = 0; k < kNonlinTaps; ++k)
                {
                    const float f = ((float)k + 0.5f) / (float)kNonlinTaps;
                    const float g = ((1.0f - attack) + attack * 2.0f * f) * ((k & 1) ? -0.36f : 0.36f);
                    const float jl = kEarlyL[k % kEarlyTaps] * 0.04f, jr = kEarlyR[k % kEarlyTaps] * 0.04f;
                    wetL += g * early_[0].read(1.0f + (f + jl) * 0.95f * win);
                    wetR += g * early_[1].read(1.0f + (f + jr) * 0.95f * win);
                }
            }
            else
            {
                // modulation: a slow sine pair plus (1970s/1980s) a random wander
                lfo_ += lfoInc; if (lfo_ >= 1.0f) lfo_ -= 1.0f;
                float q = lfo_ + 0.25f; if (q >= 1.0f) q -= 1.0f;
                rng_ = rng_ * 1664525u + 1013904223u;
                const float white = (float)(int)(rng_ >> 8) * (1.0f / 8388608.0f) - 1.0f;
                const float w0 = wobble_[0].process(white) * 40.0f, w1 = wobble_[1].process(-white) * 40.0f;
                const float m0 = depth * (fastSin01(lfo_) + colNoise * w0);
                const float m1 = depth * (fastSin01(q) + colNoise * w1);
                const float extra = 1.6f * ms;                    // keeps the modulated read behind the write

                // read the end of each half before anything is written this sample
                const float endA = tank_[0][3].read(T.a[3] * s);
                const float endB = tank_[1][3].read(T.b[3] * s);
                const float feed[2] = { x[0] + endB + 1.0e-18f, x[1] + endA + 1.0e-18f };
                const float mod[2] = { m0, m1 };
                const float* tt[2] = { T.a, T.b };

                // output taps (taken before writing; fractions of each delay, after Dattorro)
                const Line* A = tank_[0]; const Line* B = tank_[1];
                wetL =  B[1].read(1.0f + 0.063f * T.b[1] * s) + B[1].read(1.0f + 0.707f * T.b[1] * s) - B[2].read(1.0f + 0.72f * T.b[2] * s)
                      + B[3].read(1.0f + 0.54f * T.b[3] * s) - A[1].read(1.0f + 0.47f * T.a[1] * s) - A[2].read(1.0f + 0.10f * T.a[2] * s)
                      - A[3].read(1.0f + 0.29f * T.a[3] * s);
                wetR =  A[1].read(1.0f + 0.085f * T.a[1] * s) + A[1].read(1.0f + 0.81f * T.a[1] * s) - A[2].read(1.0f + 0.68f * T.a[2] * s)
                      + A[3].read(1.0f + 0.72f * T.a[3] * s) - B[1].read(1.0f + 0.50f * T.b[1] * s) - B[2].read(1.0f + 0.13f * T.b[2] * s)
                      - B[3].read(1.0f + 0.04f * T.b[3] * s);
                wetL *= 0.6f * tankGain; wetR *= 0.6f * tankGain;

                for (int h = 0; h < 2; ++h)
                {
                    Line* L = tank_[h];
                    float v = L[0].allpass(feed[h], tt[h][0] * s + extra + mod[h], gAp1);
                    const float d1 = L[1].read(tt[h][1] * s);
                    L[1].write(v);
                    // damping, once per half-loop
                    float lp = hfLp_[h].process(d1);
                    v = lp + gShelf * (d1 - lp);                  // treble shelf
                    lp = bassLp_[h].process(v);
                    v = lp * gLow[h] + (v - lp) * gMid[h];        // bass decays Bass Mult times as long
                    if (clip) v = softClip(v * 0.7f) * (1.0f / 0.7f);
                    v = L[2].allpass(v, tt[h][2] * s, gAp2);
                    L[3].write(v);
                }

                if (earlyGain > 0.0f)
                {
                    float eL = 0.0f, eR = 0.0f;
                    if (ambience)
                        for (int k = 0; k < kEarlyTaps; ++k)
                        {
                            eL += kEarlyG[k] * early_[0].read(1.0f + kEarlyL[k] * win);
                            eR += kEarlyG[k] * early_[1].read(1.0f + kEarlyR[k] * win);
                        }
                    else { eL = x[0]; eR = x[1]; }                // low Attack: the diffused input comes straight through
                    wetL += earlyGain * 0.5f * eL; wetR += earlyGain * 0.5f * eR;
                }
            }

            // wet EQ and colour
            wetL = hiCut_[0][1].process(hiCut_[0][0].process(lowCut_[0].process(wetL)));
            wetR = hiCut_[1][1].process(hiCut_[1][0].process(lowCut_[1].process(wetR)));
            if (color == 0)
            {
                for (auto& f : colour_[0]) wetL = f.process(wetL);
                for (auto& f : colour_[1]) wetR = f.process(wetR);
            }

            const float mix = mix_.process(), og = out_.process();
            outL[i] = sanitize((dryL + (wetL - dryL) * mix) * og);
            if (outR) outR[i] = sanitize((dryR + (wetR - dryR) * mix) * og);
        }
    }

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "uP2noVerb";
        info.vendor       = "uP2noGood";
        info.uniqueId     = MPC_FOURCC('U', 'Q', 'p', 'o');   // == "uid" in plugin.json
        info.version      = 100;                                // 0.1.0.0
        info.isInstrument = false;
        info.numInputs    = 2;
        info.numOutputs   = 2;
        info.tailSamples  = 0;                                  // unknown: depends on Decay
        return info;
    }

    float targetScale() const
    {
        const int mode = mpc::clamp(getInt(kMode), 0, (int)kNumModes - 1);
        return kModes[mode].scale * (0.4f + 0.012f * get(kSize));       // Size 0..100 -> 0.4x..1.6x
    }

    Line pre_[2], early_[2], inAp_[2][4], tank_[2][4];
    OnePoleLP hfLp_[2], bassLp_[2], hiCut_[2][2], colour_[2][4], wobble_[2];
    OnePoleHP lowCut_[2];
    Smoother scale_, preLen_, mix_, out_;
    float sr_ = 44100.0f, lfo_ = 0.0f;
    uint32_t rng_ = 22222u;
};

MPC_PLUGIN_ENTRY(UP2noVerb)
