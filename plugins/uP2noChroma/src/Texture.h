// Texture.h - the TEXTURE module: Filter, Squash, Cassette, Broken, Interference.
//
// SPDX-License-Identifier: MIT
// Original code by uP2noGood.
//
// Knob: Amount. CONSOLE also has Filter Style (LPF / Tilt / HPF) and Tex Drift (new: random
// movement). SETUP has Tex Tone (new: resonance on Filter, a tilt EQ on the others).

#ifndef UP2NOCHROMA_TEXTURE_H
#define UP2NOCHROMA_TEXTURE_H

#include "Common.h"

namespace chroma {

enum TexFx { tFilter = 0, tSquash, tCassette, tBroken, tInterference, kNumTexFx };
enum FilterStyle { fsLPF = 0, fsTilt, fsHPF };

// Filter cutoff for display and DSP. Returns <0 for "open" (no filtering).
inline float filterHz(float a, int style, bool& isHp)
{
    a = clamp(a, 0.0f, 1.0f);
    isHp = false;
    if (style == fsLPF) return 60.0f * std::pow(20000.0f / 60.0f, a);
    if (style == fsHPF) { isHp = true; return 20.0f * std::pow(400.0f, a); }        // 20 Hz .. 8 kHz
    if (std::fabs(a - 0.5f) < 0.02f) return -1.0f;
    if (a < 0.5f) return 20000.0f * std::pow(150.0f / 20000.0f, (0.5f - a) * 2.0f);
    isHp = true; return 20.0f * std::pow(300.0f, (a - 0.5f) * 2.0f);                // 20 Hz .. 6 kHz
}

struct Texture
{
    float sr = 44100.0f;
    SVF svf[2];
    int count = 0;
    Wander wCut{41u}, wWow{42u}, wWowR{43u}, wJit{44u}, wFade{45u};
    Rng rng{4242u};
    Comp comp;
    TiltEq tone[2];
    OnePoleLP lp[2], lp2[2], hissLp; OnePoleHP hp[2];
    Line line[2];
    float wowPh = 0.0f, flutPh = 0.0f;
    // broken
    int nextDrop = 0, dropPos = 0, dropLen = 1; float dropDepth = 0.0f;
    // interference
    int glitchLeft = 0, stutLeft = 0, stutLen = 1, stutPos = 0; float crackle = 0.0f;
    OnePoleLP crLp; OnePoleHP crHp, telHp[2]; OnePoleLP telLp[2];
    float holdV[2] = {0.0f, 0.0f}; float holdC = 0.0f, humPh = 0.0f;
    float cutSm = 1000.0f;

    void prepare(float sampleRate)
    {
        sr = sampleRate;
        for (int k = 0; k < 2; ++k) { svf[k].setSampleRate(sr); tone[k].setup(1200.0f, sr); line[k].alloc((int)(0.5f * sr)); }
        comp.setup(1.5f, 110.0f, sr);
        reset();
    }
    void reset()
    {
        for (int k = 0; k < 2; ++k)
        {
            svf[k].reset(); tone[k].reset(); lp[k].reset(); lp2[k].reset(); hp[k].reset(); line[k].clear();
            telHp[k].reset(); telLp[k].reset(); holdV[k] = 0.0f;
        }
        comp.reset(); hissLp.reset(); crLp.reset(); crHp.reset();
        wCut.reset(); wWow.reset(); wWowR.reset(); wJit.reset(); wFade.reset();
        count = 0; wowPh = flutPh = 0.0f; nextDrop = (int)(0.5f * sr); dropPos = 0; dropLen = 1; dropDepth = 0.0f;
        glitchLeft = stutLeft = stutPos = 0; stutLen = 1; crackle = 0.0f; holdC = 0.0f; humPh = 0.0f; cutSm = 1000.0f;
    }

    void process(int fx, float* L, float* R, int n, const Ctl& c)
    {
        float* ch[2] = { L, R };
        const float a = clamp(c.amt, 0.0f, 1.0f), drift = clamp(c.second, 0.0f, 1.0f), tn = clamp(c.tone, -1.0f, 1.0f);
        for (int k = 0; k < 2; ++k) tone[k].set(tn, 8.0f);
        const bool useTone = fx != tFilter && std::fabs(tn) > 0.01f;

        switch (fx)
        {
        case tFilter:
        {
            bool isHp = false;
            const float hz = filterHz(a, c.filterStyle, isHp);
            if (hz < 0.0f && drift < 0.01f) break;                    // Tilt at centre: open
            const float res = 0.08f + 0.82f * sq(0.5f + 0.5f * tn);
            const SVF::Mode mode = isHp ? SVF::Mode::Highpass : SVF::Mode::Lowpass;
            for (int i = 0; i < n; ++i)
            {
                if (count-- <= 0)
                {
                    count = 15;
                    float f = hz < 0.0f ? 20000.0f : hz;
                    if (drift > 0.0f) f *= std::pow(2.0f, drift * 1.3f * wCut.next(0.35f, sr / 16.0f));
                    cutSm += 0.5f * (f - cutSm);
                    for (int k = 0; k < 2; ++k) svf[k].set(clamp(cutSm, 20.0f, 0.45f * sr), res);
                }
                for (int k = 0; k < 2; ++k) ch[k][i] = svf[k].process(ch[k][i], mode);
            }
            return;
        }
        case tSquash:
        {
            // heavy compressor; the threshold falls with Amount, overdrive joins near the top
            comp.thr = 0.35f * dbToGain(-a * 34.0f) / std::max(c.cal, 0.1f);
            comp.ratio = 4.0f + 16.0f * a;
            const float makeup = std::min(dbToGain(18.0f), std::pow(0.35f / std::max(comp.thr, 1.0e-4f), 1.0f - 1.0f / comp.ratio) * 0.6f);
            const float od = 1.0f + std::max(0.0f, a - 0.72f) * 30.0f, odN = 1.0f / fastTanh(od * 0.5f) * 0.5f;
            for (int i = 0; i < n; ++i)
            {
                const float wob = drift > 0.0f ? dbToGain(drift * 4.0f * wJit.next(3.0f, sr)) : 1.0f;
                const float g = comp.process(std::max(std::fabs(L[i]), std::fabs(R[i]))) * makeup * wob;
                for (int k = 0; k < 2; ++k)
                {
                    float y = ch[k][i] * g;
                    if (od > 1.0f) y = fastTanh(y * od) * odN;
                    ch[k][i] = kneeClip(y, 0.6f) * 0.9f;   // catches the transients the 1.5 ms attack lets through
                }
            }
            break;
        }
        case tCassette:
        {
            // saturation, light compression, band-limiting, wow, flutter, warble and hiss, all
            // growing with Amount; Drift adds extra wow and dropouts. Tone = brighter / darker tape.
            const float sat = 1.0f + 5.0f * a, satN = 1.0f / fastTanh(0.3f * sat) * 0.3f / std::max(c.cal, 0.1f);
            for (int k = 0; k < 2; ++k)
            {
                lp[k].setCutoff(clamp(16000.0f * std::pow(0.22f, a), 1500.0f, 0.45f * sr), sr);
                lp2[k].setCutoff(clamp(14000.0f * std::pow(0.3f, a), 1500.0f, 0.45f * sr), sr);
                hp[k].setCutoff(30.0f + 90.0f * a, sr);
            }
            hissLp.setCutoff(6000.0f, sr);
            comp.thr = 0.3f / std::max(c.cal, 0.1f); comp.ratio = 1.0f + 2.0f * a;
            const float wow = (0.0006f + 0.0035f * a * a) * (1.0f + 2.5f * drift) * sr;
            const float flut = 0.00012f * a * sr;
            const float warb = a * a * 0.0012f * sr;
            const float base = 0.012f * sr;
            const float hiss = 0.0025f * a * a;
            for (int i = 0; i < n; ++i)
            {
                wowPh += 0.6f / sr; if (wowPh >= 1.0f) wowPh -= 1.0f;
                flutPh += 9.1f / sr; if (flutPh >= 1.0f) flutPh -= 1.0f;
                const float wl = wWow.next(1.4f, sr), wr = wWowR.next(1.3f, sr);
                const float dL = base + wow * sin01(wowPh) + flut * sin01(flutPh) + warb * wl;
                const float dR = base + wow * sin01(wowPh + 0.02f) + flut * sin01(flutPh + 0.1f) + warb * wr;
                const float g = comp.process(std::max(std::fabs(L[i]), std::fabs(R[i])));
                float drop = 1.0f;
                if (drift > 0.05f) drop = 1.0f - drift * 0.7f * clamp(wFade.next(0.8f, sr) * 2.0f - 1.0f, 0.0f, 1.0f);
                const float h = hissLp.process(rng.bi()) * hiss;
                for (int k = 0; k < 2; ++k)
                {
                    float x = ch[k][i] * g * c.cal;
                    x = fastTanh(x * sat + 0.05f * a) * satN - fastTanh(0.05f * a) * satN;
                    x = hp[k].process(lp2[k].process(lp[k].process(x)));
                    line[k].write(x);
                    ch[k][i] = line[k].read(k ? dR : dL) * drop + h;
                }
            }
            break;
        }
        case tBroken:
        {
            // a failing motor: every so often the pitch sags and recovers (a delay-line dip) with a
            // matching drop in level, on top of a constant shiver of pitch and level.
            const float meanGap = (2.6f - 2.2f * a) * sr;
            const float maxDip = (0.004f + 0.03f * a) * sr;
            const float base = 0.003f * sr;
            const float jit = (0.0001f + 0.0006f * a) * sr;
            for (int i = 0; i < n; ++i)
            {
                if (--nextDrop <= 0 && dropPos >= dropLen)
                {
                    dropLen = (int)((0.12f + 0.35f * rng.uni()) * sr);
                    dropPos = 0;
                    dropDepth = maxDip * (0.4f + 0.6f * rng.uni());
                    const float spread = 0.5f + drift * 1.4f;
                    nextDrop = (int)(meanGap * std::max(0.15f, 1.0f + spread * rng.bi())) + dropLen;
                }
                float shape = 0.0f;
                if (dropPos < dropLen)
                {
                    const float u = (float)dropPos / (float)dropLen;
                    shape = u < 0.7f ? u / 0.7f : (1.0f - u) / 0.3f;          // slow sag, quick recovery
                    shape = shape * shape * (3.0f - 2.0f * shape);
                    ++dropPos;
                }
                const float j = wJit.next(7.0f, sr);
                const float d = base + dropDepth * shape + jit * (1.0f + j);
                const float am = (1.0f - 0.65f * a * shape) * (1.0f - 0.12f * a * (0.5f + 0.5f * wFade.next(11.0f, sr)));
                for (int k = 0; k < 2; ++k)
                {
                    line[k].write(ch[k][i]);
                    ch[k][i] = line[k].read(d + (k ? 0.3f : 0.0f)) * am;
                }
            }
            break;
        }
        case tInterference:
        default:
        {
            // three kinds of disturbance across Amount, each strongest in its own range:
            //   radio static (crackle, noise that fades in and out), telecom (dropouts, phone band,
            //   codec grit), dissolve (falling sample rate, stutters, hum). Drift = more events.
            const float wStatic = clamp(1.0f - std::fabs(a - 0.22f) / 0.32f, 0.0f, 1.0f);
            const float wTel = clamp(1.0f - std::fabs(a - 0.55f) / 0.3f, 0.0f, 1.0f);
            const float wDis = clamp((a - 0.62f) / 0.3f, 0.0f, 1.0f);
            const float dens = 1.0f + 3.0f * drift;
            crLp.setCutoff(3500.0f, sr); crHp.setCutoff(500.0f, sr);
            for (int k = 0; k < 2; ++k) { telHp[k].setCutoff(350.0f, sr); telLp[k].setCutoff(3200.0f, sr); }
            const float holdN = 1.0f + wDis * 14.0f;
            const float q = std::pow(2.0f, 12.0f - 7.0f * std::max(wTel, wDis));
            const float sc = std::min(1.0f, a * 4.0f);   // fade the whole thing in from zero
            for (int i = 0; i < n; ++i)
            {
                // static: crackle impulses + a noise bed that wanders
                float noise = 0.0f;
                if (wStatic > 0.0f)
                {
                    if (rng.uni() < 0.0016f * wStatic * dens) crackle = rng.bi() * 0.25f * sc;
                    crackle *= 0.92f;
                    const float bed = (0.5f + 0.5f * wFade.next(0.6f, sr)) * 0.03f * wStatic * sc;
                    noise = crHp.process(crLp.process(rng.bi() * bed + crackle));
                }
                // telecom: dropouts
                float gate = 1.0f;
                if (wTel > 0.0f)
                {
                    if (glitchLeft <= 0 && rng.uni() < 2.5f * wTel * dens / sr) glitchLeft = (int)((0.02f + 0.1f * rng.uni()) * sr);
                    if (glitchLeft > 0) { --glitchLeft; gate = 1.0f - 0.95f * wTel; }
                }
                // dissolve: stutter (repeat a short chunk of the past)
                if (wDis > 0.0f && stutLeft <= 0 && rng.uni() < 1.5f * wDis * dens / sr)
                { stutLen = (int)((0.02f + 0.03f * rng.uni()) * sr); stutLeft = stutLen * (2 + (int)(rng.uni() * 4.0f)); stutPos = 0; }
                holdC += 1.0f; const bool take = holdC >= holdN; if (take) holdC -= holdN;
                humPh += 50.0f / sr; if (humPh >= 1.0f) humPh -= 1.0f;
                const float hum = wDis * 0.012f * sin01(humPh) * (0.6f + 0.4f * sin01(humPh * 3.0f));
                for (int k = 0; k < 2; ++k)
                {
                    float x = ch[k][i];
                    line[k].write(x);
                    if (stutLeft > 0) x = line[k].read((float)(stutLen + stutPos - (stutPos % stutLen)));
                    if (wTel > 0.0f) { const float ph = telLp[k].process(telHp[k].process(x)); x += (ph - x) * wTel * 0.85f; }
                    if (wTel > 0.0f || wDis > 0.0f)
                    {
                        if (take) holdV[k] = std::round(x * q) / q;
                        x += (holdV[k] - x) * std::max(wTel * 0.6f, wDis);
                    }
                    ch[k][i] = x * gate + noise + hum;
                }
                if (stutLeft > 0) { --stutLeft; ++stutPos; }
            }
            break;
        }
        }
        if (useTone)
            for (int k = 0; k < 2; ++k)
                for (int i = 0; i < n; ++i) ch[k][i] = tone[k].process(ch[k][i]);
    }
};

} // namespace chroma

#endif
