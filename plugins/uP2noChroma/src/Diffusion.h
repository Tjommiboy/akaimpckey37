// Diffusion.h - the DIFFUSION module: Cascade, Reels, Space, Collage, Reverse.
//
// SPDX-License-Identifier: MIT
// Original code by uP2noGood.
//
// Knobs: Time (0..1; delay time / reverb size / slice length / reverse speed), Amount, Drift.
// Effect Vol scales the wet signal only. SETUP: Sync + Division (Cascade, Reels, Collage times and
// the Reverse segment follow the MPC tempo), Ping-Pong (Cascade, Reels, Collage), Duck (the wet
// signal dips while you play).
//
// All delays share one 4 s stereo buffer (only one effect runs at a time; it is cleared on a change).
// Space is an 8-line feedback delay network with input diffusion; the TIME knob glides through five
// characters (chamber, room, hall, vast, cloud) by scaling size, decay and damping together.

#ifndef UP2NOCHROMA_DIFFUSION_H
#define UP2NOCHROMA_DIFFUSION_H

#include "Common.h"

namespace chroma {

enum DiffFx { dCascade = 0, dReels, dSpace, dCollage, dReverse, kNumDiffFx };

inline float cascadeMs(float t) { return 30.0f * std::pow(20.0f, clamp(t, 0.0f, 1.0f)); }     // 30 .. 600 ms
inline float reelsMs(float t)   { return 50.0f * std::pow(16.0f, clamp(t, 0.0f, 1.0f)); }     // 50 .. 800 ms
inline float collageMs(float t) { return 25.0f * std::pow(60.0f, clamp(t, 0.0f, 1.0f)); }     // 25 .. 1500 ms
inline float spaceRt60(float t) { return 0.35f * std::pow(50.0f, clamp(t, 0.0f, 1.0f)); }     // 0.35 .. 17.5 s
inline float reverseSpeed(float t)
{
    t = clamp(t, 0.0f, 1.0f);
    if (std::fabs(t - 0.5f) < 0.04f) return 1.0f;
    return std::pow(2.0f, (t - 0.5f) * 2.0f);                                                   // 0.5x .. 2x
}
inline const char* spaceName(float t)
{
    static const char* names[] = { "Chamber", "Room", "Hall", "Vast", "Cloud" };
    return names[clamp((int)(clamp(t, 0.0f, 1.0f) * 4.999f), 0, 4)];
}

struct Diffusion
{
    static constexpr float kBufSec = 4.0f;
    static constexpr int kLines = 8;
    float sr = 44100.0f;
    Line buf[2];
    // delays
    float dCur[2] = {0.0f, 0.0f};
    bool dInit = false;
    OnePoleLP loopLp[2]; OnePoleHP loopHp[2];
    float wowPh = 0.0f, flutPh = 0.0f;
    Wander wDrift{31u}, wDriftR{32u}, wDrop{33u};
    Rng rng{777u};
    float dropG = 1.0f, dropT = 1.0f; int dropLeft = 0;
    // space
    Line fdn[kLines], diffuse[2][4];
    OnePoleLP damp[kLines];
    float spLfo = 0.0f;
    // collage
    double slStart = 0.0, slPos = 0.0; double absW = 0.0;
    float slLen = 0.0f, slSpeed = 1.0f, slPitch = 0.0f; int slRepeats = 0; bool slActive = false;
    // reverse
    float rvPh = 0.0f; double rvStart[2] = {0.0, 0.0};
    // shared
    Env duckEnv;
    Ramp vol;
    float fbState[2] = {0.0f, 0.0f};

    void prepare(float sampleRate)
    {
        sr = sampleRate;
        for (auto& b : buf) b.alloc((int)(kBufSec * sr) + 64);
        for (auto& l : fdn) l.alloc((int)(0.215f * sr));
        for (auto& c : diffuse) for (auto& l : c) l.alloc((int)(0.03f * sr));
        duckEnv.set(4.0f, 260.0f, sr);
        reset();
    }
    void reset()
    {
        for (auto& b : buf) b.clear();
        for (auto& l : fdn) l.clear();
        for (auto& c : diffuse) for (auto& l : c) l.clear();
        for (auto& d : damp) d.reset();
        for (int k = 0; k < 2; ++k) { loopLp[k].reset(); loopHp[k].reset(); fbState[k] = 0.0f; }
        dInit = false; wowPh = flutPh = spLfo = 0.0f; dropG = dropT = 1.0f; dropLeft = 0;
        wDrift.reset(); wDriftR.reset(); wDrop.reset();
        slStart = slPos = absW = 0.0; slLen = 0.0f; slSpeed = 1.0f; slPitch = 0.0f; slRepeats = 0; slActive = false;
        rvPh = 0.0f; rvStart[0] = rvStart[1] = 0.0;
        duckEnv.reset();
    }

    float maxDelay() const { return (float)(buf[0].size - 64); }

    // delay time in samples for this block (free or synced)
    float delaySamples(float ms, const Ctl& c) const
    {
        float d = c.sync ? beatsToSamples(c.divBeats, c.bpm, c.sr) : ms * 0.001f * sr;
        return clamp(d, 16.0f, maxDelay() - 8.0f);
    }

    void process(int fx, float* L, float* R, int n, const Ctl& c)
    {
        const float a = clamp(c.amt, 0.0f, 1.0f), drift = clamp(c.second, 0.0f, 1.0f), t = clamp(c.main, 0.0f, 1.0f);
        vol.set(c.vol, n);
        switch (fx)
        {
        case dCascade: echo(false, L, R, n, c, a, drift, t); break;
        case dReels:   echo(true, L, R, n, c, a, drift, t); break;
        case dSpace:   space(L, R, n, c, a, drift, t); break;
        case dCollage: collage(L, R, n, c, a, drift, t); break;
        case dReverse:
        default:       reverse(L, R, n, c, a, drift, t); break;
        }
    }

    float duckGain(float xl, float xr, const Ctl& c)
    {
        const float e = duckEnv.process(std::max(std::fabs(xl), std::fabs(xr)));
        return 1.0f - c.duck * clamp(e * 4.0f * c.cal, 0.0f, 1.0f);
    }

    // ------------------------------------------------------------------ Cascade (BBD) and Reels (tape)
    void echo(bool tape, float* L, float* R, int n, const Ctl& c, float a, float drift, float t)
    {
        const float target = delaySamples(tape ? reelsMs(t) : cascadeMs(t), c);
        if (!dInit) { dCur[0] = dCur[1] = target; dInit = true; }
        const float glide = onePoleCoeff(tape ? 220.0f : 90.0f, sr);
        const float fb = a * (tape ? 1.08f : 1.12f);                 // > 1 near the top: runaway, tamed by saturation
        float lpHz = tape ? 4200.0f : 2400.0f + 3600.0f * (1.0f - t);   // BBD: longer = darker, like a slower clock
        lpHz *= 1.0f - 0.55f * drift;
        for (int k = 0; k < 2; ++k) { loopLp[k].setCutoff(lpHz, sr); loopHp[k].setCutoff(tape ? 110.0f : 60.0f, sr); }
        const float wowD = (tape ? 0.0016f : 0.0008f) * (1.0f + 5.0f * drift);
        const float flutD = tape ? 0.00025f * (1.0f + 3.0f * drift) : 0.0f;
        const float rnd = drift * (tape ? 0.012f : 0.02f);
        const float noise = (tape ? 2.0e-4f : 1.2e-4f) * (1.0f + 4.0f * drift);
        const float rOff = c.pingpong ? 1.0f : 1.013f;                 // tiny L/R difference keeps it wide
        for (int i = 0; i < n; ++i)
        {
            dCur[0] += glide * (target - dCur[0]);
            wowPh += (tape ? 0.55f : 0.35f) / sr; if (wowPh >= 1.0f) wowPh -= 1.0f;
            flutPh += 7.3f / sr; if (flutPh >= 1.0f) flutPh -= 1.0f;
            const float m = 1.0f + wowD * sin01(wowPh) + flutD * sin01(flutPh) + rnd * wDrift.next(1.7f, sr);
            const float mR = 1.0f + wowD * sin01(wowPh + 0.2f) + flutD * sin01(flutPh + 0.3f) + rnd * wDriftR.next(1.5f, sr);
            // tape dropouts with Drift: brief dips in level
            if (tape && drift > 0.05f)
            {
                if (dropLeft <= 0 && rng.uni() < drift * drift * 2.0f / sr) { dropLeft = (int)(sr * (0.03f + 0.12f * rng.uni())); dropT = 0.15f + 0.4f * rng.uni(); }
                const float goal = dropLeft-- > 0 ? dropT : 1.0f;
                dropG += 0.004f * (goal - dropG);
            }
            else dropG = 1.0f;

            const float xl = L[i], xr = R[i];
            float yl = buf[0].read(dCur[0] * m), yr = buf[1].read(dCur[0] * rOff * mR);
            yl = loopLp[0].process(loopHp[0].process(yl)) * dropG;
            yr = loopLp[1].process(loopHp[1].process(yr)) * dropG;
            const float sat = tape ? 1.5f : 1.15f;
            const float fl = fastTanh(yl * fb * sat) / sat, fr = fastTanh(yr * fb * sat) / sat;
            float inL, inR;
            if (c.pingpong) { inL = 0.5f * (xl + xr) + fr; inR = fl; }
            else            { inL = xl + fl; inR = xr + fr; }
            buf[0].write(inL + noise * rng.bi());
            buf[1].write(inR + noise * rng.bi());
            const float g = vol.next() * duckGain(xl, xr, c) * 0.85f;
            L[i] = xl + yl * g;
            R[i] = xr + yr * g;
        }
    }

    // ------------------------------------------------------------------ Space (FDN reverb)
    void space(float* L, float* R, int n, const Ctl& c, float a, float drift, float t)
    {
        static const float kMs[kLines] = { 31.3f, 37.9f, 41.1f, 47.3f, 53.9f, 59.1f, 67.7f, 73.1f };
        static const float kDiffMs[4] = { 4.7f, 3.6f, 12.7f, 9.3f };
        const float sc = 0.35f + 2.4f * std::pow(t, 1.4f);
        const float rt = spaceRt60(t);
        const float dampHz = 11000.0f - 7000.0f * t;
        float len[kLines], g[kLines];
        for (int k = 0; k < kLines; ++k)
        {
            len[k] = kMs[k] * sc * 0.001f * sr;
            g[k] = std::pow(10.0f, -3.0f * len[k] / (rt * sr));
            damp[k].setCutoff(dampHz * (k & 1 ? 0.92f : 1.0f), sr);
        }
        const float dsc = std::min(1.5f, 0.6f + 0.4f * sc);
        int dTap[2][4], fTap[kLines];
        for (int ch = 0; ch < 2; ++ch)
            for (int d = 0; d < 4; ++d) dTap[ch][d] = std::max(1, (int)(kDiffMs[d] * dsc * 0.001f * sr * (ch ? 1.07f : 1.0f)));
        for (int k = 0; k < kLines; ++k) fTap[k] = std::max(1, (int)len[k]);
        const float pre = (0.004f + 0.03f * t) * sr;
        const float modD = (0.0004f + 0.003f * drift) * sr;
        const float dryG = std::cos(a * kHalfPi), wetG = std::sin(a * kHalfPi) * std::pow(rt / 2.5f, -0.3f);
        for (int i = 0; i < n; ++i)
        {
            const float xl = L[i], xr = R[i];
            buf[0].write(xl); buf[1].write(xr);
            float in[2] = { buf[0].read(pre), buf[1].read(pre) };
            for (int ch = 0; ch < 2; ++ch)
                for (int d = 0; d < 4; ++d)
                {
                    Line& ln = diffuse[ch][d];
                    const float z = ln.tap(dTap[ch][d]), v = in[ch] - 0.6f * z;
                    ln.write(v); in[ch] = z + 0.6f * v;
                }
            spLfo += 0.71f / sr; if (spLfo >= 1.0f) spLfo -= 1.0f;
            const float m1 = sin01(spLfo), m2 = sin01(spLfo + 0.33f);
            const float r1 = drift > 0.0f ? wDrift.next(0.9f, sr) * drift : 0.0f;
            float y[kLines];
            for (int k = 0; k < kLines; ++k)
            {
                float v;
                if (k == 1) v = fdn[k].read(len[k] + modD * (1.0f + m1));
                else if (k == 5) v = fdn[k].read(len[k] + modD * (1.0f + m2));
                else if ((k == 2 || k == 6) && drift > 0.0f) v = fdn[k].read(len[k] + 3.0f * modD * (1.0f + r1));
                else v = fdn[k].tap(fTap[k]);
                y[k] = damp[k].process(v) * g[k];
            }
            // 8-point fast Hadamard transform (lossless mixing), scaled by 1/sqrt(8)
            float h[kLines];
            for (int k = 0; k < kLines; ++k) h[k] = y[k];
            for (int span = 1; span < kLines; span <<= 1)
                for (int k = 0; k < kLines; k += span << 1)
                    for (int j = k; j < k + span; ++j) { const float p = h[j], q = h[j + span]; h[j] = p + q; h[j + span] = p - q; }
            const float hs = 0.35355339f;
            for (int k = 0; k < kLines; ++k)
                fdn[k].write(flushDenormal(h[k] * hs + (k < 4 ? in[0] : in[1]) * 0.5f));
            const float wl = (y[0] - y[2] + y[4] - y[6] + 0.5f * (y[1] - y[7])) * 0.6f;
            const float wr = (y[1] - y[3] + y[5] - y[7] + 0.5f * (y[0] - y[6])) * 0.6f;
            const float gw = wetG * vol.next() * duckGain(xl, xr, c);
            L[i] = xl * dryG + wl * gw;
            R[i] = xr * dryG + wr * gw;
        }
    }

    // ------------------------------------------------------------------ Collage (looping delay)
    // The input is always recorded. The player grabs the most recent slice (TIME long), repeats it
    // a random number of times (more with Amount), then grabs the next. Its output is fed back into
    // the recording, so the buffer is edited as it plays. Drift: random double-speed slices and pitch wobble.
    void collage(float* L, float* R, int n, const Ctl& c, float a, float drift, float t)
    {
        const float wantLen = delaySamples(collageMs(t), c);
        const float fade = std::min(0.004f * sr, wantLen * 0.25f);
        const float fb = a * 0.92f;
        const double cap = (double)maxDelay() - 16.0;
        for (int i = 0; i < n; ++i)
        {
            if (!slActive || slPos >= (double)slLen)
            {
                if (slActive && slRepeats > 0) { --slRepeats; slPos = 0.0; }
                else
                {
                    slLen = wantLen;
                    slStart = absW - (double)slLen;
                    slPos = 0.0;
                    const int maxRep = std::max(0, (int)(cap / slLen) - 2);
                    slRepeats = std::min(maxRep, (int)(rng.uni() * (1.0f + a * 6.0f)));
                    slSpeed = rng.uni() < drift * 0.45f ? 2.0f : 1.0f;
                    slActive = true;
                }
            }
            const float wob = drift * 0.012f * wDrift.next(2.2f, sr);
            const double pos = slStart + slPos;
            const float edge = (float)std::min(slPos, (double)slLen - slPos);
            const float win = fade > 1.0f ? clamp(edge / fade, 0.0f, 1.0f) : 1.0f;
            // keep the read point inside the recorded history
            const double safe = std::max(pos, absW - cap);
            const float yl = buf[0].readAbs(safe) * win, yr = buf[1].readAbs(safe) * win;
            slPos += (double)(slSpeed * (1.0f + wob));

            const float xl = L[i], xr = R[i];
            float inL, inR;
            const float fl = fastTanh(yl * fb * 1.2f) / 1.2f, fr = fastTanh(yr * fb * 1.2f) / 1.2f;
            if (c.pingpong) { inL = 0.5f * (xl + xr) + fr; inR = fl; }
            else            { inL = xl + fl; inR = xr + fr; }
            buf[0].write(inL); buf[1].write(inR);
            absW += 1.0;
            const float g = vol.next() * duckGain(xl, xr, c) * 0.9f;
            L[i] = xl + yl * g;
            R[i] = xr + yr * g;
        }
        if (absW > 1.0e12) { const double sh = std::floor(absW / buf[0].size) * buf[0].size; absW -= sh; slStart -= sh; }
    }

    // ------------------------------------------------------------------ Reverse
    // Two overlapping grains, each playing the last segment backwards at Speed; a gentle fixed
    // feedback gives a few repeats. TIME = speed (centre = normal, left = half, right = double).
    void reverse(float* L, float* R, int n, const Ctl& c, float a, float drift, float t)
    {
        const float speed0 = reverseSpeed(t);
        float seg = c.sync ? beatsToSamples(c.divBeats, c.bpm, c.sr) : 0.45f * sr;
        // a grain of `seg` input samples played at `speed` must stay inside the buffer
        const float maxSeg = maxDelay() * speed0 / (1.0f + speed0) - 32.0f;
        seg = clamp(seg, 0.04f * sr, maxSeg);
        const float grainLen = seg / speed0;           // output samples per grain
        const float inc = 1.0f / grainLen;
        const float dryG = std::cos(a * kHalfPi), wetG = std::sin(a * kHalfPi);
        const float fb = 0.35f;
        for (int i = 0; i < n; ++i)
        {
            const float sp = speed0 * (1.0f + drift * 0.03f * wDrift.next(1.1f, sr));
            float yl = 0.0f, yr = 0.0f;
            for (int gi = 0; gi < 2; ++gi)
            {
                float p = rvPh + 0.5f * (float)gi; p -= std::floor(p);
                // start of this grain: (re)captured when its phase wraps
                double& st = rvStart[gi];
                if (p < inc) st = absW;                     // newest sample becomes the grain's start
                const double src = st - 1.0 - (double)(p * grainLen * sp);
                const double safe = std::max(src, absW - (double)maxDelay());
                const float w = hann01(p);
                yl += buf[0].readAbs(safe) * w;
                yr += buf[1].readAbs(safe) * w;
            }
            rvPh += inc; if (rvPh >= 1.0f) rvPh -= 1.0f;
            const float xl = L[i], xr = R[i];
            buf[0].write(xl + fb * yl); buf[1].write(xr + fb * yr);
            absW += 1.0;
            const float g = wetG * vol.next() * duckGain(xl, xr, c);
            L[i] = xl * dryG + yl * g;
            R[i] = xr * dryG + yr * g;
        }
    }
};

} // namespace chroma

#endif
