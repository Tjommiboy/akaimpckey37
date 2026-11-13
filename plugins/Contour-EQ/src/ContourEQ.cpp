// Contour EQ - an MPC effect plugin by uP2noGood
//
// SPDX-License-Identifier: MIT
//
// A "visual" parametric EQ for the MPC's touch screen. The MPC does not let a plugin draw
// its own graphics, so the picture is built from parameters the plugin writes itself:
//
//   * kNumBars  "EQ bar" parameters   - the EQ's response (dB) at kNumBars log-spaced
//                                        frequencies, shown as bipolar vertical colour bars.
//   * kNumBars  "spectrum" parameters - a live FFT analyzer of the audio, shown as dim bars
//                                        behind the EQ bars.
//   * Each band's Freq / Gain parameters are also driven by an XYPad dot on the graph, so
//     dragging a dot moves the band.
//
// These display parameters are not automatable. They are written from the audio thread
// (atomic stores, no allocation), the EQ bars only when a band setting changes, the
// spectrum about 20 times a second.
//
// Signal path (stereo, zero latency):
//   in -> [band 1] -> [band 2] -> [band 3] -> [band 4] -> output gain -> out
//   Each band is 1-4 biquad sections:
//     Bell, Low Shelf, High Shelf, Notch, Band Pass : 1 section (RBJ cookbook, prewarped)
//     Tilt                                          : 2 shelves (-g/2 below, +g/2 above)
//     Low Cut / High Cut                            : Butterworth, 6-48 dB/oct (1-4 sections),
//                                                     Q adds resonance to the sharpest pole pair
//   Coefficients glide linearly across each block, so dragging a band does not zipper.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/smoother.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace mpc::dsp;

namespace {

constexpr int   kNumBands   = 4;
constexpr int   kNumBars    = 160;          // graph resolution (bars across the screen)
constexpr int   kMaxSect    = 4;            // biquads per band (48 dB/oct cut = 4)
constexpr float kGraphDb    = 18.0f;        // EQ graph spans -18 .. +18 dB (same as the Gain range)
constexpr float kFMin       = 20.0f, kFMax = 20000.0f;
constexpr int   kFftBits    = 11, kFftSize = 1 << kFftBits;   // 2048-point analyzer
constexpr int   kFftHop     = 2048;         // one analysis per 2048 samples (~21 per second)
constexpr float kSpecFloor  = -90.0f, kSpecTop = 0.0f;        // analyzer range in dB

enum Shape { kBell, kLowShelf, kHighShelf, kLowCut, kHighCut, kNotch, kBandPass, kTilt, kNumShapes };

struct Coefs  { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };   // designed in double (also used for the graph)
struct CoefsF { float  b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };   // what the audio path runs

// Frequency of bar i's centre, and of its edges (i - 0.5, i + 0.5), on a log axis 20 Hz .. 20 kHz.
inline float barFreq(float pos) { return kFMin * std::pow(kFMax / kFMin, pos / (float)kNumBars); }

// ---- biquad design (RBJ Audio EQ Cookbook), w0 prewarped by the bilinear transform ----------
Coefs peaking(double f, double q, double db, double sr)
{
    const double A = std::pow(10.0, db / 40.0), w = (2.0 * M_PI) * f / sr, cw = std::cos(w), al = std::sin(w) / (2.0 * q);
    const double a0 = 1.0 + al / A;
    return { (1.0 + al * A) / a0, -2.0 * cw / a0, (1.0 - al * A) / a0, -2.0 * cw / a0, (1.0 - al / A) / a0 };
}
Coefs shelf(bool high, double f, double q, double db, double sr)
{
    const double A = std::pow(10.0, db / 40.0), w = (2.0 * M_PI) * f / sr, cw = std::cos(w), sw = std::sin(w);
    const double al = sw / (2.0 * q), sA = 2.0 * std::sqrt(A) * al;
    double b0, b1, b2, a0, a1, a2;
    if (!high) {
        b0 = A * ((A + 1) - (A - 1) * cw + sA); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - sA);
        a0 = (A + 1) + (A - 1) * cw + sA;       a1 = -2 * ((A - 1) + (A + 1) * cw);    a2 = (A + 1) + (A - 1) * cw - sA;
    } else {
        b0 = A * ((A + 1) + (A - 1) * cw + sA); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - sA);
        a0 = (A + 1) - (A - 1) * cw + sA;       a1 = 2 * ((A - 1) - (A + 1) * cw);      a2 = (A + 1) - (A - 1) * cw - sA;
    }
    return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
}
Coefs pass2(bool high, double f, double q, double sr)
{
    const double w = (2.0 * M_PI) * f / sr, cw = std::cos(w), al = std::sin(w) / (2.0 * q), a0 = 1.0 + al;
    if (high) return { (1 + cw) / 2 / a0, -(1 + cw) / a0, (1 + cw) / 2 / a0, -2 * cw / a0, (1 - al) / a0 };
    return { (1 - cw) / 2 / a0, (1 - cw) / a0, (1 - cw) / 2 / a0, -2 * cw / a0, (1 - al) / a0 };
}
Coefs pass1(bool high, double f, double sr)
{
    const double k = std::tan(M_PI * f / sr), a0 = k + 1.0;
    if (high) return { 1.0 / a0, -1.0 / a0, 0.0, (k - 1.0) / a0, 0.0 };
    return { k / a0, k / a0, 0.0, (k - 1.0) / a0, 0.0 };
}
Coefs notch(double f, double q, double sr)
{
    const double w = (2.0 * M_PI) * f / sr, cw = std::cos(w), al = std::sin(w) / (2.0 * q), a0 = 1.0 + al;
    return { 1.0 / a0, -2 * cw / a0, 1.0 / a0, -2 * cw / a0, (1 - al) / a0 };
}
Coefs bandpass(double f, double q, double sr)
{
    const double w = (2.0 * M_PI) * f / sr, cw = std::cos(w), al = std::sin(w) / (2.0 * q), a0 = 1.0 + al;
    return { al / a0, 0.0, -al / a0, -2 * cw / a0, (1 - al) / a0 };
}

// Iterative radix-2 complex FFT, in place. Tables are built in prepare().
struct Fft
{
    std::vector<float> cosT, sinT; std::vector<int> rev;
    void init()
    {
        cosT.resize(kFftSize / 2); sinT.resize(kFftSize / 2); rev.resize(kFftSize);
        for (int i = 0; i < kFftSize / 2; ++i) { cosT[(size_t)i] = std::cos(kTwoPi * i / kFftSize); sinT[(size_t)i] = -std::sin(kTwoPi * i / kFftSize); }
        for (int i = 0; i < kFftSize; ++i) { int r = 0; for (int b = 0; b < kFftBits; ++b) if (i & (1 << b)) r |= 1 << (kFftBits - 1 - b); rev[(size_t)i] = r; }
    }
    void bitReverse(float* re, float* im) const
    {
        for (int i = 0; i < kFftSize; ++i) { const int j = rev[(size_t)i]; if (j > i) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); } }
    }
    // One butterfly stage (stage = 1 .. kFftBits). Running all stages in order after bitReverse = full FFT.
    void stage(float* re, float* im, int st) const
    {
        {
            const int len = 1 << st;
            const int half = len >> 1, step = kFftSize / len;
            for (int s = 0; s < kFftSize; s += len)
                for (int k = 0; k < half; ++k)
                {
                    const float c = cosT[(size_t)(k * step)], sn = sinT[(size_t)(k * step)];
                    const int a = s + k, b = a + half;
                    const float tr = re[b] * c - im[b] * sn, ti = re[b] * sn + im[b] * c;
                    re[b] = re[a] - tr; im[b] = im[a] - ti; re[a] += tr; im[a] += ti;
                }
        }
    }
    void run(float* re, float* im) const { bitReverse(re, im); for (int st = 1; st <= kFftBits; ++st) stage(re, im, st); }
};

} // namespace

class ContourEQ : public mpc::Plugin
{
public:
    // Parameter layout. Never reorder: saved projects find parameters by id, the skin by index.
    enum { kOutput = 0, kBandBase = 1, kPerBand = 6 };   // per band: Freq, Gain, Q, Shape, Slope, On
    enum { bFreq, bGain, bQ, bShape, bSlope, bOn };
    enum { kAnalyzer = kBandBase + kNumBands * kPerBand, kSpeed, kTiltSpec, kRefresh, kFirstEqBar };
    static constexpr int kFirstSpecBar = kFirstEqBar + kNumBars;
    static constexpr int kTotalParams  = kFirstSpecBar + kNumBars;

    static int bandParam(int band, int which) { return kBandBase + band * kPerBand + which; }

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "Contour EQ";
        info.vendor       = "uP2noGood";
        info.uniqueId     = MPC_FOURCC(kUid[0], kUid[1], kUid[2], kUid[3]);   // must match plugin.json "uid"
        info.version      = 200;     // 0.2.0.0 - test build
        info.isInstrument = false;
        info.numInputs    = 2;
        info.numOutputs   = 2;
        return info;
    }

    ContourEQ() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        addParam(floatParam("out", "Output", -24.0f, 12.0f, 0.0f, "dB", 1));

        static const float defF[kNumBands] = {80.0f, 400.0f, 2500.0f, 10000.0f};
        static const int   defS[kNumBands] = {kLowShelf, kBell, kBell, kHighShelf};
        const std::vector<std::string> shapes = {"Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch", "Band Pass", "Tilt"};
        const std::vector<std::string> slopes = {"6 dB", "12 dB", "18 dB", "24 dB", "36 dB", "48 dB"};
        for (int b = 0; b < kNumBands; ++b)
        {
            char id[16], nm[16]; const int n = b + 1;
            std::snprintf(id, sizeof id, "b%dfreq", n);  std::snprintf(nm, sizeof nm, "%d Freq", n);  addParam(logParam(id, nm, kFMin, kFMax, defF[b], "Hz", 0));
            std::snprintf(id, sizeof id, "b%dgain", n);  std::snprintf(nm, sizeof nm, "%d Gain", n);  addParam(floatParam(id, nm, -kGraphDb, kGraphDb, 0.0f, "dB", 1));
            std::snprintf(id, sizeof id, "b%dq", n);     std::snprintf(nm, sizeof nm, "%d Q", n);     addParam(logParam(id, nm, 0.1f, 18.0f, 0.71f, "", 2));
            std::snprintf(id, sizeof id, "b%dshape", n); std::snprintf(nm, sizeof nm, "%d Shape", n); addParam(choiceParam(id, nm, shapes, defS[b]));
            std::snprintf(id, sizeof id, "b%dslope", n); std::snprintf(nm, sizeof nm, "%d Slope", n); addParam(choiceParam(id, nm, slopes, 1));
            std::snprintf(id, sizeof id, "b%don", n);    std::snprintf(nm, sizeof nm, "%d On", n);    addParam(boolParam(id, nm, true));
        }
        addParam(choiceParam("ana", "Analyzer", {"Off", "Pre", "Post"}, 2));
        addParam(choiceParam("speed", "Speed", {"Slow", "Medium", "Fast"}, 1));
        addParam(choiceParam("tilt", "Ana Tilt", {"0 dB", "3 dB", "4.5 dB"}, 2));
        addParam(choiceParam("refresh", "Refresh", {"Normal", "Display", "Notify"}, 0));   // test switch: how to make the MPC redraw

        // Display-only parameters (written by the plugin, shown as bars).
        for (int i = 0; i < kNumBars; ++i)
        {
            char id[16], nm[16];
            std::snprintf(id, sizeof id, "eq%03d", i); std::snprintf(nm, sizeof nm, "EQ %d", i + 1);
            Param p = floatParam(id, nm, -kGraphDb, kGraphDb, 0.0f, "dB", 1); p.automatable = false; addParam(p);
        }
        for (int i = 0; i < kNumBars; ++i)
        {
            char id[16], nm[16];
            std::snprintf(id, sizeof id, "sp%03d", i); std::snprintf(nm, sizeof nm, "Spec %d", i + 1);
            Param p = floatParam(id, nm, kSpecFloor, kSpecTop, kSpecFloor, "dB", 0); p.automatable = false; addParam(p);
        }
        if (numParams() != kTotalParams) mpc::log("ContourEQ: parameter count mismatch (%d vs %d)", numParams(), kTotalParams);

        addPrograms();
    }

    ~ContourEQ() override
    {
        running_ = false;
        if (notifier_.joinable()) notifier_.join();
    }

    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        if (!notifier_.joinable()) { running_ = true; notifier_ = std::thread([this] { notifierLoop(); }); }
        sr_ = (float)sampleRate;
        output_.setTime(20.0f, sampleRate);
        output_.snap(dbToGain(get(kOutput)));
        fft_.init();
        ring_.assign(kFftSize, 0.0f); re_.assign(kFftSize, 0.0f); im_.assign(kFftSize, 0.0f); win_.resize(kFftSize);
        for (int i = 0; i < kFftSize; ++i) win_[(size_t)i] = 0.5f - 0.5f * std::cos(kTwoPi * i / (kFftSize - 1));

        // Per-bar trig for the response, and FFT bin ranges for the analyzer.
        const float binHz = sr_ / (float)kFftSize;
        for (int i = 0; i < kNumBars; ++i)
        {
            const float f = std::min(barFreq((float)i + 0.5f), 0.49f * sr_), w = kTwoPi * f / sr_;
            phi_[i] = std::pow(std::sin(0.5 * (double)w), 2.0);
            barOct_[i] = std::log2(f / 1000.0f);
            float lo = barFreq((float)i) / binHz, hi = barFreq((float)i + 1.0f) / binHz;
            binLo_[i] = std::max(1, (int)std::ceil(lo)); binHi_[i] = std::min(kFftSize / 2 - 1, (int)std::floor(hi));
            binCentre_[i] = std::min((float)(kFftSize / 2 - 2), f / binHz);
        }
        reset();
    }

    void reset() override
    {
        for (auto& b : bands_) { b.n = 0; for (auto& s : b.s) { s.z[0][0] = s.z[0][1] = s.z[1][0] = s.z[1][1] = 0.0f; } }
        for (auto& c : cache_) c = -1.0e9f;      // force a full redesign on the next block
        std::fill(ring_.begin(), ring_.end(), 0.0f);
        ringPos_ = 0; hopCount_ = 0; anaStep_ = -1;
        for (int i = 0; i < kNumBars; ++i) specDb_[i] = get(kFirstSpecBar + i);   // decay from what is shown
        updateBands(true);
        responseDirty_ = true;
        holdDisplay_ = (int)(0.3f * sr_);   // keep the restored picture for a moment after a load/reset
        output_.snap(dbToGain(get(kOutput)));
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int n = ctx.numFrames;
        const bool changed = updateBands(false, n);
        if (changed) responseDirty_ = true;

        float* L = ctx.outputs[0];
        float* R = ctx.numOutputs > 1 ? ctx.outputs[1] : ctx.outputs[0];
        const float* inL = ctx.numInputs > 0 ? ctx.inputs[0] : L;
        const float* inR = ctx.numInputs > 1 ? ctx.inputs[1] : inL;
        const int ana = getInt(kAnalyzer);
        output_.setTarget(dbToGain(get(kOutput)));

        for (int i = 0; i < n; ++i)
        {
            float l = inL[i], r = inR[i];                    // read inputs first (in-place buffers)
            if (ana == 1) pushAnalyzer(0.5f * (l + r));
            for (auto& b : bands_)
                for (int k = 0; k < b.n; ++k)
                {
                    Sect& s = b.s[k];
                    s.cur.b0 += s.d.b0; s.cur.b1 += s.d.b1; s.cur.b2 += s.d.b2; s.cur.a1 += s.d.a1; s.cur.a2 += s.d.a2;
                    l = s.tick(0, l); r = s.tick(1, r);
                }
            const float g = output_.process();
            l *= g; r *= g;
            if (ana == 2) pushAnalyzer(0.5f * (l + r));
            L[i] = l; if (R != L) R[i] = r;
        }
        for (auto& b : bands_) for (int k = 0; k < b.n; ++k) b.s[k].snapTo(b.s[k].tgt);

        // ---- display parameters ----
        sinceResponse_ += n;
        if (holdDisplay_ > 0) { holdDisplay_ -= n; return; }
        analyseStep();
        bool wroteSomething = false;
        if (responseDirty_ && sinceResponse_ >= (int)(0.03f * sr_)) { writeResponse(); responseDirty_ = false; sinceResponse_ = 0; wroteSomething = true; }
        if (ana == 0 && specActive_) { for (int i = 0; i < kNumBars; ++i) { specDb_[i] = kSpecFloor; setNormalised(kFirstSpecBar + i, 0.0f); } specActive_ = false; wroteSomething = true; }
        if (specFresh_) { specFresh_ = false; wroteSomething = true; }
        (void)wroteSomething;   // redraw requests are made by the notifier thread, never from here
    }

private:
    struct Sect
    {
        CoefsF cur, d; Coefs tgt;
        float z[2][2] = {{0, 0}, {0, 0}};          // TDF-II state per channel
        void zeroDelta() { d = CoefsF{0, 0, 0, 0, 0}; }
        void snapTo(const Coefs& c) { tgt = c; cur = CoefsF{(float)c.b0, (float)c.b1, (float)c.b2, (float)c.a1, (float)c.a2}; zeroDelta(); }
        inline float tick(int ch, float x)
        {
            float* zz = z[ch];
            const float y = cur.b0 * x + zz[0];
            zz[0] = flushDenormal(cur.b1 * x - cur.a1 * y + zz[1]);
            zz[1] = flushDenormal(cur.b2 * x - cur.a2 * y);
            return y;
        }
    };
    struct Band { int n = 0; Sect s[kMaxSect]; };

    // Re-design bands whose settings moved. Returns true if anything changed.
    // On a change, the coefficients glide from the old to the new values over this block.
    bool updateBands(bool snap, int blockLen = 1)
    {
        bool any = false;
        for (int b = 0; b < kNumBands; ++b)
        {
            float v[kPerBand]; bool diff = false;
            for (int k = 0; k < kPerBand; ++k) { v[k] = get(bandParam(b, k)); if (v[k] != cache_[b * kPerBand + k]) diff = true; }
            if (!diff) continue;
            any = true;
            const bool structural = snap || v[bShape] != cache_[b * kPerBand + bShape] || v[bSlope] != cache_[b * kPerBand + bSlope] || v[bOn] != cache_[b * kPerBand + bOn];
            for (int k = 0; k < kPerBand; ++k) cache_[b * kPerBand + k] = v[k];

            Coefs c[kMaxSect]; const int nSect = design(v, c);
            Band& bd = bands_[b];
            if (structural || nSect != bd.n)
            {
                for (int k = 0; k < nSect; ++k) { if (k >= bd.n) { bd.s[k].z[0][0] = bd.s[k].z[0][1] = bd.s[k].z[1][0] = bd.s[k].z[1][1] = 0; } bd.s[k].snapTo(c[k]); }
                bd.n = nSect;
            }
            else
            {
                const float inv = 1.0f / (float)std::max(1, blockLen);
                for (int k = 0; k < nSect; ++k)
                {
                    Sect& s = bd.s[k]; s.tgt = c[k];
                    s.d = { ((float)c[k].b0 - s.cur.b0) * inv, ((float)c[k].b1 - s.cur.b1) * inv, ((float)c[k].b2 - s.cur.b2) * inv, ((float)c[k].a1 - s.cur.a1) * inv, ((float)c[k].a2 - s.cur.a2) * inv };
                }
            }
        }
        return any;
    }

    int design(const float* v, Coefs* c) const
    {
        if (v[bOn] < 0.5f) return 0;
        const double f = clamp(v[bFreq], kFMin, 0.45f * sr_), g = v[bGain], q = clamp(v[bQ], 0.1f, 18.0f), sr = sr_;
        switch ((int)v[bShape])
        {
            case kBell:      c[0] = peaking(f, q, g, sr); return 1;
            case kLowShelf:  c[0] = shelf(false, f, q, g, sr); return 1;
            case kHighShelf: c[0] = shelf(true, f, q, g, sr); return 1;
            case kNotch:     c[0] = notch(f, q, sr); return 1;
            case kBandPass:  c[0] = bandpass(f, q, sr); return 1;
            case kTilt:      c[0] = shelf(false, f, 0.5, -0.5 * g, sr); c[1] = shelf(true, f, 0.5, 0.5 * g, sr); return 2;
            case kLowCut: case kHighCut:
            {
                static const int orders[6] = {1, 2, 3, 4, 6, 8};
                const bool high = (int)v[bShape] == kLowCut;             // a low cut is a high-pass
                const int order = orders[clamp((int)v[bSlope], 0, 5)];
                int n = 0;
                if (order & 1) c[n++] = pass1(high, f, sr);
                const int pairs = order / 2;
                const double res = q / 0.70710678;                            // Q 0.71 = plain Butterworth
                for (int k = 1; k <= pairs; ++k)
                {
                    const double theta = (order & 1) ? M_PI * k / order : M_PI * (2.0 * k - 1.0) / (2.0 * order);
                    double qk = 1.0 / (2.0 * std::cos(theta));
                    if (k == pairs) qk *= res;                               // sharpest pair carries the resonance
                    c[n++] = pass2(high, f, std::min(40.0, std::max(0.1, qk)), sr);
                }
                return n;
            }
            default: return 0;
        }
    }

    // EQ response at each bar, from the target coefficients. Cheap: no trig per call.
    void writeResponse()
    {
        for (int i = 0; i < kNumBars; ++i)
        {
            float db = 0.0f;
            for (const auto& b : bands_)
                for (int k = 0; k < b.n; ++k)
                {
                    // RBJ's numerically stable form, phi = sin^2(w/2): accurate at 20 Hz too
                    const Coefs& c = b.s[k].tgt; const double ph = phi_[i];
                    const double sb = c.b0 + c.b1 + c.b2, sa = 1.0 + c.a1 + c.a2;
                    const double num = sb * sb - 4.0 * (c.b0 * c.b1 + 4.0 * c.b0 * c.b2 + c.b1 * c.b2) * ph + 16.0 * c.b0 * c.b2 * ph * ph;
                    const double den = sa * sa - 4.0 * (c.a1 + 4.0 * c.a2 + c.a1 * c.a2) * ph + 16.0 * c.a2 * ph * ph;
                    db += (float)(10.0 * std::log10(std::max(num, 1.0e-30) / std::max(den, 1.0e-30)));
                }
            set(kFirstEqBar + i, clamp(db, -kGraphDb, kGraphDb));
        }
    }

    inline void pushAnalyzer(float x)
    {
        ring_[(size_t)ringPos_] = x;
        ringPos_ = (ringPos_ + 1) & (kFftSize - 1);
        if (++hopCount_ >= kFftHop) { hopCount_ = 0; if (holdDisplay_ <= 0 && anaStep_ < 0) anaStep_ = 0; }
    }

    // The analysis is spread over several process() calls so no single audio block pays for a whole
    // FFT: step 0 copies + windows, steps 1..11 are the FFT stages, then two steps convert to bars.
    void analyseStep()
    {
        if (anaStep_ < 0) return;
        if (anaStep_ == 0)
        {
            for (int i = 0; i < kFftSize; ++i) { re_[(size_t)i] = ring_[(size_t)((ringPos_ + i) & (kFftSize - 1))] * win_[(size_t)i]; im_[(size_t)i] = 0.0f; }
            fft_.bitReverse(re_.data(), im_.data());
        }
        else if (anaStep_ <= kFftBits) fft_.stage(re_.data(), im_.data(), anaStep_);
        else if (anaStep_ == kFftBits + 1)
        {
            // power per bin, scaled so a full-scale sine reads about 0 dB (Hann: peak |X| = A*N/4)
            const float scale = 4.0f / (float)kFftSize;
            for (int i = 0; i <= kFftSize / 2; ++i) { const float a = re_[(size_t)i] * scale, b = im_[(size_t)i] * scale; re_[(size_t)i] = a * a + b * b; }
            barsFrom(0, kNumBars / 2);
        }
        else { barsFrom(kNumBars / 2, kNumBars); specActive_ = true; specFresh_ = true; anaStep_ = -1; return; }
        ++anaStep_;
    }

    void barsFrom(int i0, int i1)
    {
        static const float tilts[3] = {0.0f, 3.0f, 4.5f};
        static const float rel[3] = {0.12f, 0.3f, 0.6f};
        const float tilt = tilts[clamp(getInt(kTiltSpec), 0, 2)], r = rel[clamp(getInt(kSpeed), 0, 2)];
        for (int i = i0; i < i1; ++i)
        {
            float p;
            if (binHi_[i] >= binLo_[i]) { p = 0.0f; for (int k = binLo_[i]; k <= binHi_[i]; ++k) p = std::max(p, re_[(size_t)k]); }
            else { const int k = (int)binCentre_[i]; const float fr = binCentre_[i] - (float)k; p = re_[(size_t)k] + fr * (re_[(size_t)k + 1] - re_[(size_t)k]); }
            float db = 10.0f * std::log10(p + 1.0e-12f) + tilt * barOct_[i];
            db = clamp(db, kSpecFloor, kSpecTop + 6.0f);
            specDb_[i] = db > specDb_[i] ? db : specDb_[i] + (db - specDb_[i]) * r;
            set(kFirstSpecBar + i, specDb_[i]);
        }
    }

    // Test option for MPCs that do not redraw parameters the plugin changed itself. Runs on its
    // own thread (never the audio thread), about 25 times a second:
    //   Display : ask the host to re-read all parameter values (audioMasterUpdateDisplay)
    //   Notify  : report each display parameter that moved, like a knob turn (audioMasterAutomate)
    void notifierLoop()
    {
        float sent[2 * kNumBars];
        for (float& v : sent) v = -1.0f;
        int tick = 0;
        while (running_)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            const int mode = getInt(kRefresh);
            if (mode == 1) { requestDisplayUpdate(); continue; }
            if (mode != 2) continue;
            const bool doSpec = (++tick & 1) == 0;                 // analyzer at ~12 Hz, EQ at ~25 Hz
            for (int i = 0; i < 2 * kNumBars; ++i)
            {
                if (i >= kNumBars && !doSpec) break;
                const int idx = kFirstEqBar + i;
                const float v = getNormalised(idx);
                if (std::fabs(v - sent[i]) > (i < kNumBars ? 0.0005f : 0.004f)) { sent[i] = v; notifyHostParameterChanged(idx); }
            }
        }
    }

    void addPrograms()
    {
        auto program = [this](const char* name, std::initializer_list<std::pair<int, float>> o)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            for (const auto& p : o) v[(size_t)p.first] = p.second;
            addProgram(name, std::move(v));
        };
        auto P = [](int band, int which) { return bandParam(band, which); };
        program("Flat", {});
        program("Vocal Clarity", {{P(0, bShape), kLowCut}, {P(0, bFreq), 90}, {P(0, bSlope), 3},
                                  {P(1, bFreq), 300}, {P(1, bGain), -3}, {P(1, bQ), 1.2f},
                                  {P(2, bFreq), 3200}, {P(2, bGain), 3}, {P(2, bQ), 0.9f},
                                  {P(3, bFreq), 11000}, {P(3, bGain), 4}});
        program("Kick Punch",   {{P(0, bShape), kLowCut}, {P(0, bFreq), 30}, {P(0, bSlope), 3},
                                  {P(1, bFreq), 60}, {P(1, bGain), 4}, {P(1, bQ), 1.4f},
                                  {P(2, bFreq), 350}, {P(2, bGain), -5}, {P(2, bQ), 1.5f},
                                  {P(3, bShape), kBell}, {P(3, bFreq), 4000}, {P(3, bGain), 3}, {P(3, bQ), 1.2f}});
        program("Lo-Fi Band",   {{P(0, bShape), kLowCut}, {P(0, bFreq), 250}, {P(0, bSlope), 5}, {P(0, bQ), 1.4f},
                                  {P(1, bFreq), 1000}, {P(1, bGain), 3}, {P(1, bQ), 0.8f},
                                  {P(2, bOn), 0},
                                  {P(3, bShape), kHighCut}, {P(3, bFreq), 3500}, {P(3, bSlope), 5}, {P(3, bQ), 1.4f}});
        program("Bright Tilt",  {{P(0, bOn), 0}, {P(1, bShape), kTilt}, {P(1, bFreq), 1000}, {P(1, bGain), 6}, {P(2, bOn), 0}, {P(3, bOn), 0}});
    }

    float sr_ = 44100.0f;
    Band bands_[kNumBands];
    float cache_[kNumBands * kPerBand];
    Smoother output_;
    bool responseDirty_ = true; int sinceResponse_ = 1 << 30;

    double phi_[kNumBars] = {};
    float barOct_[kNumBars] = {}, binCentre_[kNumBars] = {};
    int binLo_[kNumBars] = {}, binHi_[kNumBars] = {};
    Fft fft_;
    std::vector<float> ring_, re_, im_, win_;
    int ringPos_ = 0, hopCount_ = 0;
    float specDb_[kNumBars];
    bool specActive_ = false, specFresh_ = false;
    int holdDisplay_ = 0, anaStep_ = -1;
    std::atomic<bool> running_{false};
    std::thread notifier_;

    static constexpr char kUid[5] = "UFCA";
};

MPC_PLUGIN_ENTRY(ContourEQ)
