// Character.h - the CHARACTER module: Drive, Sweeten, Fuzz, Howl, Swell.
//
// SPDX-License-Identifier: MIT
// Original code by uP2noGood.
//
// Knobs: Tilt (-1..1, tone; on Fuzz also bias, on Howl sustain <-> stab), Amount, Sensitivity (-1..1).
// Gain staging: the signal is multiplied by s = calibration x Sensitivity before every nonlinearity
// and the output is level-compensated for the total gain, so Input Level / Sensitivity change how
// hard it breaks up, not (much) how loud it is. Swell uses Sensitivity as its trigger threshold instead.

#ifndef UP2NOCHROMA_CHARACTER_H
#define UP2NOCHROMA_CHARACTER_H

#include "Common.h"

namespace chroma {

enum CharFx { cDrive = 0, cSweeten, cFuzz, cHowl, cSwell, kNumCharFx };

struct Character
{
    float sr = 44100.0f;
    OnePoleHP preHp[2];
    OnePoleLP postLp[2], swLow[2], swHigh[2];
    TiltEq tilt[2];
    DCBlocker dc[2];
    Comp comp;
    SVF howlF[2];
    Env howlSlow, howlFast, swFast, swSlow;
    float swGain = 0.0f; int swHold = 0; bool swOpen = false, swDip = false;
    int howlCount = 0;

    void prepare(float sampleRate)
    {
        sr = sampleRate;
        for (int c = 0; c < 2; ++c) { tilt[c].setup(900.0f, sr); dc[c].prepare(sr, 12.0f); howlF[c].setSampleRate(sr); }
        comp.setup(8.0f, 160.0f, sr);
        howlFast.set(2.0f, 80.0f, sr);
        swFast.set(1.0f, 25.0f, sr);
        swSlow.set(40.0f, 350.0f, sr);
        reset();
    }
    void reset()
    {
        for (int c = 0; c < 2; ++c)
        {
            preHp[c].reset(); postLp[c].reset(); swLow[c].reset(); swHigh[c].reset();
            tilt[c].reset(); dc[c].reset(); howlF[c].reset();
        }
        comp.reset(); howlSlow.reset(); howlFast.reset(); swFast.reset(); swSlow.reset();
        swGain = 0.0f; swHold = 0; swOpen = false; swDip = false; howlCount = 0;
    }

    // level compensation for a tanh stage with gain g, referenced to a -10 dBFS signal
    static float makeup(float g, float r = 0.2f) { return r / fastTanh(r * g); }

    void process(int fx, float* L, float* R, int n, const Ctl& c)
    {
        float* ch[2] = { L, R };
        const float t = clamp(c.main, -1.0f, 1.0f), a = clamp(c.amt, 0.0f, 1.0f);
        const float s = clamp(c.cal * dbToGain(c.second * 12.0f), 0.05f, 40.0f);
        for (int k = 0; k < 2; ++k) tilt[k].set(t, 6.0f);

        switch (fx)
        {
        case cDrive:
        {
            // asymmetric tube-ish curve; the bias gives even harmonics, always musical
            const float g = 1.0f + 38.0f * a * a, b = 0.12f, tb = fastTanh(b), mk = makeup(g * s);
            for (int k = 0; k < 2; ++k) { preHp[k].setCutoff(70.0f, sr); postLp[k].setCutoff(5500.0f + 9000.0f * (1.0f - a), sr); }
            for (int k = 0; k < 2; ++k)
                for (int i = 0; i < n; ++i)
                {
                    float x = preHp[k].process(ch[k][i]) * s;
                    float y = (fastTanh(x * g + b) - tb) * mk;
                    y = postLp[k].process(dc[k].process(y));
                    ch[k][i] = tilt[k].process(y);
                }
            break;
        }
        case cSweeten:
        {
            // preamp EQ (a little low end and presence) -> gentle compression -> soft saturation
            const float g = 1.0f + 3.0f * a, mk = makeup(g * s);
            comp.thr = 0.18f; comp.ratio = 1.0f + 4.0f * a;
            const float compMk = 1.0f + 0.6f * a;
            for (int k = 0; k < 2; ++k) { swLow[k].setCutoff(130.0f, sr); swHigh[k].setCutoff(3200.0f, sr); }
            const float lowG = 0.15f + 0.2f * a, hiG = 0.18f + 0.22f * a;
            for (int i = 0; i < n; ++i)
            {
                float x[2];
                for (int k = 0; k < 2; ++k)
                {
                    const float v = ch[k][i] * s;
                    const float lo = swLow[k].process(v), hi = v - swHigh[k].process(v);
                    x[k] = v + lowG * lo + hiG * hi;
                }
                const float gr = comp.process(std::max(std::fabs(x[0]), std::fabs(x[1]))) * compMk;
                for (int k = 0; k < 2; ++k)
                {
                    const float y = fastTanh(x[k] * gr * g) * mk;
                    ch[k][i] = tilt[k].process(dc[k].process(y));
                }
            }
            break;
        }
        case cFuzz:
        {
            // high gain, asymmetric clipping, bias from Tilt (left = starved and gated, right = fat),
            // tone from Tilt, and a "blown speaker" rectifier creeping in near full Amount
            const float g = 3.0f + 160.0f * a * a;
            const float bias = 0.28f * t, gate = t < 0.0f ? -t * 0.25f : 0.0f;
            const float blown = a * a * a * 0.7f, outK = 0.165f;
            const float fc = 900.0f * std::pow(10.0f, (t + 1.0f) * 0.5f);   // 900 Hz .. 9 kHz
            for (int k = 0; k < 2; ++k) { preHp[k].setCutoff(110.0f, sr); postLp[k].setCutoff(fc, sr); }
            for (int k = 0; k < 2; ++k)
                for (int i = 0; i < n; ++i)
                {
                    float v = preHp[k].process(ch[k][i]) * s * g;
                    if (gate > 0.0f) { const float av = std::fabs(v); if (av < gate) v *= av / gate; }
                    v += bias;
                    float y = v > 0.0f ? fastTanh(v * 1.4f) : -fastTanh(-v * 0.9f) * 0.8f;
                    y += blown * (std::fabs(y) - 0.45f);
                    y = clamp(y, -1.0f, 1.0f);
                    y = postLp[k].process(dc[k].process(y)) * outK;
                    ch[k][i] = y;
                }
            break;
        }
        case cHowl:
        {
            // fuzz into a resonant low-pass whose cutoff follows the playing.
            // Tilt left: slow, singing sustain. Tilt right: fast sweeps that close (synth stabs).
            const float g = 5.0f + 70.0f * a * a;
            const float res = 0.62f + 0.3f * a;
            const float stab = (t + 1.0f) * 0.5f;
            howlSlow.set(20.0f, 250.0f + 600.0f * (1.0f - stab), sr);
            howlFast.set(1.5f, 60.0f + 160.0f * (1.0f - stab), sr);
            const float outK = 0.2f;
            for (int k = 0; k < 2; ++k) preHp[k].setCutoff(90.0f, sr);
            for (int i = 0; i < n; ++i)
            {
                const float det = std::max(std::fabs(L[i]), std::fabs(R[i])) * s;
                const float es = howlSlow.process(det), ef = howlFast.process(det);
                if (howlCount-- <= 0)
                {
                    howlCount = 15;
                    const float eS = clamp(es * 3.0f, 0.0f, 1.0f), eF = clamp(ef * 3.0f, 0.0f, 1.0f);
                    const float fSus = 280.0f + 1300.0f * eS;
                    const float fStab = 160.0f + 5500.0f * eF * eF;
                    const float fc = fSus + (fStab - fSus) * stab;
                    for (int k = 0; k < 2; ++k) howlF[k].set(fc, res);
                }
                for (int k = 0; k < 2; ++k)
                {
                    const float v = fastTanh(preHp[k].process(ch[k][i]) * s * g);
                    const float y = softClip(howlF[k].process(v, SVF::Mode::Lowpass) * 0.8f);
                    ch[k][i] = dc[k].process(y) * outK;
                }
            }
            break;
        }
        case cSwell:
        default:
        {
            // automatic volume swell: every new note (a jump in level above the threshold) restarts
            // a fade-in. Amount = swell time (40 ms .. 3 s); the fade-out on silence is half of it.
            const float attS = 0.04f * std::pow(75.0f, a);
            const float up = 1.0f / (attS * sr), down = 1.0f / (attS * 0.5f * sr + 1.0f), dip = 1.0f / (0.003f * sr);
            const float thr = 0.012f * dbToGain(-c.second * 18.0f) / std::max(c.cal, 0.1f);
            for (int i = 0; i < n; ++i)
            {
                const float det = std::max(std::fabs(L[i]), std::fabs(R[i]));
                const float f = swFast.process(det), sl = swSlow.process(det);
                if (swHold > 0) --swHold;
                if (f > thr && f > sl * 2.0f && swHold == 0) { swDip = true; swHold = (int)(0.08f * sr); swOpen = true; }
                if (f < thr * 0.5f) swOpen = false;
                if (swDip) { swGain -= dip; if (swGain <= 0.0f) { swGain = 0.0f; swDip = false; } }   // 3 ms dip, no click
                else swGain += swOpen ? up : -down;
                swGain = clamp(swGain, 0.0f, 1.0f);
                const float gg = swGain * swGain * (3.0f - 2.0f * swGain);
                for (int k = 0; k < 2; ++k) ch[k][i] = tilt[k].process(ch[k][i] * gg);
            }
            break;
        }
        }
    }
};

} // namespace chroma

#endif
