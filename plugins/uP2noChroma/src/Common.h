// Common.h - small building blocks shared by the four uP2noChroma modules.
//
// SPDX-License-Identifier: MIT
// Original code by uP2noGood. Nothing here is taken from any product.

#ifndef UP2NOCHROMA_COMMON_H
#define UP2NOCHROMA_COMMON_H

#include "dsp/math.h"
#include "dsp/onepole.h"
#include "dsp/svf.h"
#include "dsp/tempo.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace chroma {

using namespace mpc::dsp;

// ---------------------------------------------------------------------------- random
struct Rng
{
    uint32_t s = 0x1234567u;
    explicit Rng(uint32_t seed = 0x1234567u) : s(seed) {}
    uint32_t u32() { s = s * 1664525u + 1013904223u; return s; }
    float bi() { return (float)(u32() >> 8) * (2.0f / 16777216.0f) - 1.0f; }   // -1..1
    float uni() { return (float)(u32() >> 8) * (1.0f / 16777216.0f); }          // 0..1
};

// A smooth random wander in -1..1: moves (cosine-shaped) from one random point to the next at `hz`.
struct Wander
{
    float a = 0.0f, b = 0.0f, ph = 0.0f;
    Rng rng;
    explicit Wander(uint32_t seed = 99u) : rng(seed) { b = rng.bi(); }
    void reset() { a = b = rng.bi(); ph = 0.0f; }
    float next(float hz, float sr)
    {
        ph += hz / sr;
        if (ph >= 1.0f) { ph -= std::floor(ph); a = b; b = rng.bi(); }
        const float t = ph * ph * (3.0f - 2.0f * ph);       // smoothstep: no corners
        return a + (b - a) * t;
    }
};

// ---------------------------------------------------------------------------- delay line
// Power-of-two ring buffer with a fractional (linear) read. Memory only in alloc().
struct Line
{
    std::vector<float> b;
    int w = 0, mask = 0, size = 0;
    void alloc(int samples) { int s = 16; while (s < samples + 8) s <<= 1; b.assign((size_t)s, 0.0f); mask = s - 1; size = s; w = 0; }
    void clear() { std::fill(b.begin(), b.end(), 0.0f); w = 0; }
    void write(float x) { b[(size_t)w] = x; w = (w + 1) & mask; }
    float tap(int d) const { return b[(size_t)((w - d) & mask)]; }         // d >= 1
    float read(float d) const                                                // d samples ago, 1 = newest
    {
        d = clamp(d, 1.0f, (float)(size - 4));
        const float p = (float)w - d;
        const int i = (int)std::floor(p);
        const float f = p - (float)i;
        const float x0 = b[(size_t)(i & mask)], x1 = b[(size_t)((i + 1) & mask)];
        return x0 + f * (x1 - x0);
    }
    // read at an absolute (unwrapped) position
    float readAbs(double pos) const
    {
        const double fl = std::floor(pos);
        const int i = (int)(int64_t)fl;
        const float f = (float)(pos - fl);
        const float x0 = b[(size_t)(i & mask)], x1 = b[(size_t)((i + 1) & mask)];
        return x0 + f * (x1 - x0);
    }
};

// ---------------------------------------------------------------------------- tone helpers
// Tilt EQ around a pivot: t = -1 (dark) .. +1 (bright), +-maxDb at the extremes.
struct TiltEq
{
    OnePoleLP lp;
    float gl = 1.0f, gh = 1.0f;
    void setup(float pivotHz, float sr) { lp.setCutoff(pivotHz, sr); }
    void set(float t, float maxDb) { gl = dbToGain(-t * maxDb); gh = dbToGain(t * maxDb); }
    void reset() { lp.reset(); }
    float process(float x) { const float l = lp.process(x); return l * gl + (x - l) * gh; }
};

// Peak / RMS-ish envelope follower with separate attack and release.
struct Env
{
    float e = 0.0f, ca = 1.0f, cr = 1.0f;
    void set(float attMs, float relMs, float sr) { ca = onePoleCoeff(attMs, sr); cr = onePoleCoeff(relMs, sr); }
    void reset() { e = 0.0f; }
    float process(float x) { const float a = std::fabs(x); e += (a > e ? ca : cr) * (a - e); return e; }
};

// Feed-forward compressor, gain computed every 8 samples (pow is expensive on the MPC).
struct Comp
{
    Env env;
    float thr = 0.25f, ratio = 2.0f, gain = 1.0f, target = 1.0f, step = 0.0f;
    int count = 0;
    void setup(float attMs, float relMs, float sr) { env.set(attMs, relMs, sr); }
    void reset() { env.reset(); gain = target = 1.0f; step = 0.0f; count = 0; }
    // detector = level to follow (usually max(|L|,|R|)); returns the gain to apply
    float process(float detector)
    {
        const float e = env.process(detector);
        if (count-- <= 0)
        {
            count = 7;
            float t = 1.0f;
            if (e > thr && thr > 1.0e-6f) t = std::pow(e / thr, 1.0f / ratio - 1.0f);
            step = (t - gain) * 0.125f;
            target = t;
        }
        gain += step;
        return gain;
    }
};

// Linear per-sample ramp towards a per-block target (click-free parameter changes).
struct Ramp
{
    float v = 0.0f, inc = 0.0f; int left = 0; bool init = false;
    void set(float target, int frames)
    {
        if (!init) { v = target; inc = 0.0f; left = 0; init = true; return; }
        if (frames < 1) frames = 1;
        inc = (target - v) / (float)frames; left = frames;
    }
    void jump(float target) { v = target; inc = 0.0f; left = 0; init = true; }
    float next() { if (left > 0) { v += inc; --left; } return v; }
};

// Smooth control value updated once per block (about `ms` to settle).
struct BlockSmooth
{
    float v = 0.0f; bool init = false;
    float update(float target, float ms, int frames, float sr)
    {
        if (!init) { v = target; init = true; return v; }
        const float c = 1.0f - std::exp(-(float)frames / (ms * 0.001f * sr));
        v += c * (target - v);
        return v;
    }
};

// Knee clip: transparent below `knee`, smoothly bends to a ceiling of 1.0.
inline float kneeClip(float x, float knee = 0.9f)
{
    const float a = std::fabs(x);
    if (a <= knee) return x;
    const float r = 1.0f - knee;
    const float y = knee + r * fastTanh((a - knee) / r);
    return x < 0.0f ? -y : y;
}

// Sine of a phase in 0..1, accurate enough for audio-rate pitch/amplitude modulation.
inline float sin01(float ph) { return fastSin01(ph); }

// Raised-cosine (Hann) window value for t in 0..1, without trig: 4 t (1 - t) shaped by a smoothstep.
inline float hann01(float t)
{
    t = clamp(t, 0.0f, 1.0f);
    const float s = 0.5f - 0.5f * fastSin01(t + 0.25f);   // = 0.5 - 0.5 cos(2 pi t)
    return s;
}

// Everything a module needs from the outside for one block.
struct Ctl
{
    float sr = 44100.0f;
    double bpm = 120.0, ppq = 0.0;
    bool playing = false, ppqValid = false;
    float cal = 1.0f;          // input level calibration: how hard the saturating stages are hit
    // module knobs (0..1 unless noted)
    float main = 0.5f;         // Tilt (-1..1) / Rate / Time / unused for Texture
    float amt = 0.0f;
    float second = 0.0f;       // Sensitivity (-1..1) / Drift
    float vol = 1.0f;          // linear
    bool  sync = false;
    float divBeats = 1.0f;
    float spread = 0.5f, duck = 0.0f, tone = 0.0f;
    bool  pingpong = false;
    int   filterStyle = 1;
};

} // namespace chroma

#endif
