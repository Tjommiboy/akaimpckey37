// Movement.h - the MOVEMENT module: Doubler, Vibrato, Phaser, Tremolo, Pitch.
//
// SPDX-License-Identifier: MIT
// Original code by uP2noGood.
//
// Knobs: Rate (0..1; Doubler = doubling time, Pitch = interval, others = LFO speed), Amount, Drift.
// SETUP: Sync + Division (Vibrato, Phaser, Tremolo follow the MPC tempo), Spread (stereo width).

#ifndef UP2NOCHROMA_MOVEMENT_H
#define UP2NOCHROMA_MOVEMENT_H

#include "Common.h"

namespace chroma {

enum MoveFx { mDoubler = 0, mVibrato, mPhaser, mTremolo, mPitch, kNumMoveFx };

// Rate knob (0..1) -> LFO frequency, 0.05 .. 20 Hz.
inline float rateToHz(float r) { return 0.05f * std::pow(400.0f, clamp(r, 0.0f, 1.0f)); }
// Rate knob -> doubling time in ms, 8 .. 110 ms.
inline float rateToDoublerMs(float r) { return 8.0f * std::pow(13.75f, clamp(r, 0.0f, 1.0f)); }
// Rate knob -> pitch interval in semitones, -12 .. +12 (whole semitones).
inline int rateToSemis(float r) { return (int)std::lround((clamp(r, 0.0f, 1.0f) * 2.0f - 1.0f) * 12.0f); }

struct Movement
{
    float sr = 44100.0f;
    Line buf[2];
    float ph = 0.0f;                    // LFO phase 0..1
    Wander wSlow{11u}, wSlowR{12u}, wFast{13u}, wFastR{14u}, wLfo{15u}, wLfoR{16u};
    // phaser
    static constexpr int kMaxStages = 12;
    float apZ[2][kMaxStages] = {};
    float phFb[2] = {0.0f, 0.0f};
    float apA[2] = {0.0f, 0.0f};
    int apCount = 0;
    // pitch
    float pPh[2] = {0.0f, 0.25f};
    float holdV[2] = {0.0f, 0.0f}; float holdC = 0.0f;

    void prepare(float sampleRate)
    {
        sr = sampleRate;
        for (auto& b : buf) b.alloc((int)(0.25f * sr));
        reset();
    }
    void reset()
    {
        for (auto& b : buf) b.clear();
        ph = 0.0f;
        wSlow.reset(); wSlowR.reset(); wFast.reset(); wFastR.reset(); wLfo.reset(); wLfoR.reset();
        for (auto& z : apZ) for (float& v : z) v = 0.0f;
        phFb[0] = phFb[1] = 0.0f; apCount = 0;
        pPh[0] = 0.0f; pPh[1] = 0.25f; holdV[0] = holdV[1] = 0.0f; holdC = 0.0f;
    }

    // LFO frequency for this block; with Sync on and the MPC playing, the phase is locked to the song.
    float lfoHz(const Ctl& c, bool syncable)
    {
        if (syncable && c.sync)
        {
            const float beats = std::max(c.divBeats, 0.0625f);
            if (c.playing && c.ppqValid) ph = ppqPhase(c.ppq, beats);
            return beatsToHz(beats, c.bpm);
        }
        return rateToHz(c.main);
    }

    void process(int fx, float* L, float* R, int n, const Ctl& c)
    {
        float* ch[2] = { L, R };
        const float a = clamp(c.amt, 0.0f, 1.0f), drift = clamp(c.second, 0.0f, 1.0f);
        const float spread = clamp(c.spread, 0.0f, 1.0f);

        switch (fx)
        {
        case mDoubler:
        {
            // two short, slowly wandering delays (different left and right) added to the dry sound;
            // Drift adds sudden little pitch lurches (fast wander of the delay time)
            const float base = rateToDoublerMs(c.main) * 0.001f * sr;
            const float wob = 0.0007f * sr, lurch = drift * 0.0045f * sr;
            const float norm = 1.0f / (1.0f + 0.45f * a);
            for (int i = 0; i < n; ++i)
            {
                const float dL = base + wob * (1.0f + wSlow.next(0.5f, sr)) + lurch * (1.0f + wFast.next(2.5f, sr));
                const float dR = base * 1.37f + wob * (1.0f + wSlowR.next(0.43f, sr)) + lurch * (1.0f + wFastR.next(2.1f, sr));
                const float xl = L[i], xr = R[i];
                buf[0].write(xl); buf[1].write(xr);
                float wl = buf[0].read(dL), wr = buf[1].read(dR);
                const float m = 0.5f * (wl + wr);
                wl = m + (wl - m) * spread * 2.0f; wr = m + (wr - m) * spread * 2.0f;
                // the doubles sit a little to the opposite side of their source
                L[i] = (xl + a * (0.75f * wr + 0.25f * wl)) * norm;
                R[i] = (xr + a * (0.75f * wl + 0.25f * wr)) * norm;
            }
            break;
        }
        case mVibrato:
        {
            // 100 % wet modulated delay. Drift morphs the LFO from sine towards random warble and
            // pulls the two sides apart.
            const float hz = lfoHz(c, true);
            const float depthMs = std::pow(a, 1.5f) * (1.0f + 4.5f / (1.0f + hz));
            const float depth = depthMs * 0.001f * sr, center = depth + 0.0015f * sr;
            const float off = 0.25f * spread + 0.25f * drift;
            const float inc = hz / sr;
            for (int i = 0; i < n; ++i)
            {
                ph += inc; if (ph >= 1.0f) ph -= 1.0f;
                const float rl = wLfo.next(hz * 1.3f + 0.2f, sr), rr = wLfoR.next(hz * 1.3f + 0.2f, sr);
                const float ml = sin01(ph) * (1.0f - drift) + rl * drift;
                const float mr = sin01(ph + off) * (1.0f - drift) + rr * drift;
                buf[0].write(L[i]); buf[1].write(R[i]);
                L[i] = buf[0].read(center + depth * ml);
                R[i] = buf[1].read(center + depth * mr);
            }
            break;
        }
        case mPhaser:
        {
            // 2..12 first-order allpass stages (more with Amount), swept 150 Hz .. 3.5 kHz,
            // with feedback; Drift roughens the sweep with random movement.
            const int stages = 2 + 2 * std::min(5, (int)(a * 6.0f));
            const float fb = 0.12f + 0.5f * a;
            const float hz = lfoHz(c, true), inc = hz / sr;
            const float off = 0.5f * spread;
            for (int i = 0; i < n; ++i)
            {
                ph += inc; if (ph >= 1.0f) ph -= 1.0f;
                if (apCount-- <= 0)
                {
                    apCount = 15;
                    for (int k = 0; k < 2; ++k)
                    {
                        const float s = sin01(ph + (k ? off : 0.0f)) * (1.0f - drift)
                                      + (k ? wLfoR : wLfo).next(hz * 2.0f + 0.3f, sr / 16.0f) * drift;
                        const float f = 150.0f * std::exp2((0.5f + 0.5f * s) * 4.5236f);       // 150 Hz .. 3.45 kHz
                        const float x = kPi * f / sr, x2 = x * x;                              // x <= ~0.25 rad
                        const float tn = x * (1.0f + x2 * (0.33333f + x2 * 0.13333f));         // tan series
                        apA[k] = (tn - 1.0f) / (tn + 1.0f);
                    }
                }
                for (int k = 0; k < 2; ++k)
                {
                    const float x = ch[k][i];
                    float v = x + fb * phFb[k];
                    for (int s = 0; s < stages; ++s)
                    {
                        const float y = apA[k] * v + apZ[k][s];
                        apZ[k][s] = v - apA[k] * y;
                        v = y;
                    }
                    phFb[k] = flushDenormal(clamp(v, -2.0f, 2.0f));
                    ch[k][i] = 0.5f * (x + v);
                }
            }
            break;
        }
        case mTremolo:
        {
            // amplitude modulation; high Amount goes deeper and squarer (chop).
            // Drift wobbles rate and depth. Spread = phase between sides (full = auto-pan).
            const float hz = lfoHz(c, true);
            const float depth = std::min(1.0f, a * 1.35f);
            const float sq = clamp((a - 0.45f) * 2.2f, 0.0f, 1.0f), sharp = 1.0f + 14.0f * sq;
            const float sharpNorm = 1.0f / fastTanh(sharp);
            const float off = 0.5f * spread;
            for (int i = 0; i < n; ++i)
            {
                const float jit = 1.0f + drift * 0.6f * wSlow.next(0.7f, sr);
                ph += hz * jit / sr; if (ph >= 1.0f) ph -= std::floor(ph);
                const float dj = depth * (1.0f - drift * 0.4f * (0.5f + 0.5f * wFast.next(1.3f, sr)));
                for (int k = 0; k < 2; ++k)
                {
                    const float w = fastTanh(sin01(ph + (k ? off : 0.0f)) * sharp) * sharpNorm;   // -1..1
                    ch[k][i] *= 1.0f - dj * (0.5f - 0.5f * w);
                }
            }
            break;
        }
        case mPitch:
        default:
        {
            // two-grain delay-line pitch shifter (+-1 octave in semitones), Amount = wet mix.
            // Drift lowers the resolution (sample-and-hold + fewer bits) for a lo-fi shift.
            const int semis = rateToSemis(c.main);
            const float ratio = std::pow(2.0f, (float)semis / 12.0f);
            const float W = 0.045f * sr, inc = (1.0f - ratio) / W;
            const float holdN = 1.0f + drift * 11.0f;
            const float q = drift > 0.01f ? std::pow(2.0f, 15.0f - 10.0f * drift) : 0.0f;
            for (int i = 0; i < n; ++i)
            {
                buf[0].write(L[i]); buf[1].write(R[i]);
                holdC += 1.0f;
                const bool take = holdC >= holdN;
                if (take) holdC -= holdN;
                for (int k = 0; k < 2; ++k)
                {
                    float& p = pPh[k];
                    p += inc; p -= std::floor(p);
                    const float p2 = p + 0.5f - std::floor(p + 0.5f);
                    const float g1 = hann01(p), g2 = hann01(p2);
                    float wet = buf[k].read(2.0f + p * W) * g1 + buf[k].read(2.0f + p2 * W) * g2;
                    if (drift > 0.01f)
                    {
                        if (take) holdV[k] = std::round(wet * q) / q;
                        wet = holdV[k];
                    }
                    ch[k][i] = ch[k][i] * (1.0f - a) + wet * a;
                }
            }
            break;
        }
        }
    }
};

} // namespace chroma

#endif
