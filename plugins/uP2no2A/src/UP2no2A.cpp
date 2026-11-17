// uP2no2A - an electro-optical leveling amplifier for the MPC, by uP2noGood
//
// SPDX-License-Identifier: MIT
//
// In the spirit of the classic two-knob tube/optical leveler of the 1960s. Original DSP, written
// for this plugin from published descriptions of how such a unit behaves (no code or measurements
// from any other product).
//
// Signal path (per channel):
//
//   in -> [optical attenuator: gain g] -> tube make-up stage (Gain) -> DC blocker -> Output trim -> out
//               ^                  |
//               |                  v  (feedback: the side chain listens AFTER the attenuator)
//          optical cell <- light panel <- side-chain amp (Peak Reduction) <- Emphasis filter
//
//   * Peak Reduction sets how hard the side chain drives the light panel. The threshold of the
//     cell itself is fixed, exactly like the hardware: more side-chain gain = lower threshold
//     (0 = about 0 dBFS, 100 = about -40 dBFS).
//   * The light panel is square-law above its threshold. In a feedback loop that gives a soft
//     knee and roughly 3:1 to 4:1 (Compress). Limit adds a much steeper term (about 20:1).
//   * The cell has no attack/release knobs. Attack is about 10 ms. Release is two-stage: about
//     half of the gain reduction is gone in 60 ms, the rest takes 0.5 to several seconds, and it
//     takes longer the longer and harder the cell has been lit ("memory").
//   * Gain is the tube make-up stage after the attenuator. It saturates softly when pushed and is
//     in the path even with Peak Reduction at 0.
//   * Emphasis (the hardware's rear-panel R37 trim): 100 = flat side chain. Turned down, the side
//     chain hears less bass, so the leveler reacts more to high frequencies.
//   * Stereo Link: one cell for both channels (image stays put) or one cell per channel.
//
// The cell runs at 1/8 of the sample rate (log/pow only there); the gain is interpolated per sample.
//
// Parameters (order and ids are permanent):
//   0 Output      dB trim after everything (parameter 0 is nudged by the MPC on load: harmless)
//   1 Peak Red    0..100  amount of leveling
//   2 Gain        0..100  make-up (40 = unity, 100 = +30 dB)
//   3 Mode        Compress / Limit
//   4 Meter       +10 / GR / +4   what the VU meter shows
//   5 Emphasis    0..100  side-chain HF emphasis (100 = flat)
//   6 Stereo Link on/off
//   7 Power       off = bypass
//   8 VU          written by the plugin: needle position for the meter on screen (do not automate)

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/onepole.h"
#include "dsp/smoother.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

using namespace mpc::dsp;

namespace {

enum ParamIndex { kOutput = 0, kPeakRed, kGain, kMode, kMeter, kEmphasis, kLink, kPower, kVU, kNumParams };

constexpr int   kCtl      = 8;        // control-rate divider for the optical cell
constexpr float kCellK    = 20.0f;    // light -> conductance scale (knee sharpness)
constexpr float kLimitL   = 2000.0f;  // weight of the steep term in Limit
constexpr float kVuTop    = 1.41254f; // +3 VU as a voltage ratio: full-scale needle position

// The T4-style optical cell: turns "light" (already scaled) into gain reduction in dB.
struct OptoCell
{
    float fast = 0.0f, slow = 0.0f, mem = 0.0f, gr = 0.0f;
    float aAtk = 0.02f, aRelFast = 0.002f, aSlowAtk = 0.006f, aMemUp = 1e-4f, aMemDn = 5e-5f, ctlRate = 5512.5f;

    void prepare(float controlRate)
    {
        ctlRate  = controlRate;
        aAtk     = 1.0f - std::exp(-1.0f / (0.010f * controlRate));    // ~10 ms attack
        aRelFast = 1.0f - std::exp(-1.0f / (0.087f * controlRate));    // 50 % gone in ~60 ms
        aSlowAtk = 1.0f - std::exp(-1.0f / (0.030f * controlRate));
        aMemUp   = 1.0f - std::exp(-1.0f / (2.0f * controlRate));
        aMemDn   = 1.0f - std::exp(-1.0f / (5.0f * controlRate));
    }
    void reset() { fast = slow = mem = gr = 0.0f; }

    // u = mean light over the last control period. Returns gain reduction in dB (>= 0).
    float update(float u)
    {
        const float target = 20.0f * std::log10(1.0f + u);
        fast += (target > fast ? aAtk : aRelFast) * (target - fast);
        // slow stage: sits at half of the fast stage, lets go slowly; slower with more "memory"
        const float half = 0.5f * fast;
        if (half > slow) slow += aSlowAtk * (half - slow);
        else
        {
            const float tau = 0.25f + 2.2f * mem;                      // seconds
            slow += (half - slow) / (tau * ctlRate);
        }
        gr = fast > slow ? fast : slow;
        const float mt = mpc::clamp(gr * 0.05f, 0.0f, 1.0f);           // 20 dB of reduction = fully lit
        mem += (mt > mem ? aMemUp : aMemDn) * (mt - mem);
        if (gr < 1.0e-5f) { gr = 0.0f; if (fast < 1.0e-5f) fast = 0.0f; if (slow < 1.0e-5f) slow = 0.0f; }
        return gr;
    }
};

} // namespace

class UP2no2A : public mpc::Plugin
{
public:
    UP2no2A() : mpc::Plugin(makeInfo())
    {
        addParam(mpc::floatParam ("output",  "Output",      -12.0f, 12.0f, 0.0f, "dB", 1));
        addParam(mpc::floatParam ("peakred", "Peak Red",    0.0f, 100.0f, 35.0f, "", 0));
        addParam(mpc::floatParam ("gain",    "Gain",        0.0f, 100.0f, 40.0f, "", 0));
        addParam(mpc::choiceParam("mode",    "Mode",        {"Compress", "Limit"}, 0));
        addParam(mpc::choiceParam("meter",   "Meter",       {"+10", "GR", "+4"}, 1));
        addParam(mpc::floatParam ("emph",    "Emphasis",    0.0f, 100.0f, 100.0f, "", 0));
        addParam(mpc::boolParam  ("link",    "Stereo Link", true));
        addParam(mpc::boolParam  ("power",   "Power",       true));
        addParam(mpc::floatParam ("vu",      "VU",          0.0f, 1.0f, 1.0f / kVuTop, "", 2));   // display only
        if (numParams() != kNumParams) mpc::log("uP2no2A: parameter count mismatch");

        const float rest = 1.0f / kVuTop;
        //                          Output PeakRed Gain  Mode Meter Emph  Link Power VU
        addProgram("Default",       {0.0f, 35.0f, 40.0f, 0.0f, 1.0f, 100.0f, 1.0f, 1.0f, rest});
        addProgram("Vocal Leveler", {0.0f, 50.0f, 52.0f, 0.0f, 1.0f, 100.0f, 1.0f, 1.0f, rest});
        addProgram("Bass Glue",     {0.0f, 45.0f, 50.0f, 0.0f, 1.0f, 55.0f,  1.0f, 1.0f, rest});
        addProgram("Drum Bus Limit",{0.0f, 40.0f, 48.0f, 1.0f, 1.0f, 70.0f,  1.0f, 1.0f, rest});
        addProgram("Heavy Squeeze", {-2.0f, 75.0f, 66.0f, 1.0f, 1.0f, 100.0f, 1.0f, 1.0f, rest});
        addProgram("Just Tubes",    {-4.0f, 0.0f, 62.0f, 0.0f, 2.0f, 100.0f, 1.0f, 1.0f, rest});
    }

    ~UP2no2A() override
    {
        running_ = false;
        if (notifier_.joinable()) notifier_.join();
    }

    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        sr_ = (float)sampleRate;
        for (int c = 0; c < 2; ++c)
        {
            cell_[c].prepare(sr_ / (float)kCtl);
            scLow_[c].setCutoff(1000.0f, sampleRate);
            dc_[c].prepare(sampleRate, 12.0f);
        }
        makeup_.setTime(20.0f, sampleRate);
        output_.setTime(20.0f, sampleRate);
        power_.setTime(8.0f, sampleRate);
        drive_.setTime(20.0f, sampleRate);
        reset();
        if (!notifier_.joinable()) { running_ = true; notifier_ = std::thread([this] { notifierLoop(); }); }
    }

    void reset() override
    {
        for (int c = 0; c < 2; ++c)
        {
            cell_[c].reset(); scLow_[c].reset(); dc_[c].reset();
            g_[c] = 1.0f; gStep_[c] = 0.0f; uAcc_[c] = 0.0f;
        }
        ctlCount_ = 0;
        makeup_.snap(makeupGain());
        output_.snap(dbToGain(get(kOutput)));
        power_.snap(getBool(kPower) ? 1.0f : 0.0f);
        drive_.snap(scDrive());
        outMs_ = 0.0f;
        needle_ = getNormalised(kVU);
        holdVu_ = (int)(0.3f * sr_);   // leave the restored value alone for a moment after a load/reset
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
        makeup_.setTarget(makeupGain());
        output_.setTarget(dbToGain(get(kOutput)));
        power_.setTarget(getBool(kPower) ? 1.0f : 0.0f);
        drive_.setTarget(scDrive());
        const bool  limit   = getInt(kMode) == 1;
        const bool  link    = getBool(kLink);
        const float lowGain = 0.08f + 0.92f * get(kEmphasis) * 0.01f;   // bass share of the side chain
        const float invCtl  = 1.0f / (float)kCtl;
        float msAcc = 0.0f;

        for (int i = 0; i < n; ++i)
        {
            const float dry[2] = { inL ? inL[i] : 0.0f, inR ? inR[i] : 0.0f };
            const float d  = drive_.process();
            const float mk = makeup_.process();
            const float og = output_.process();
            const float pw = power_.process();
            float wet[2];

            for (int c = 0; c < 2; ++c)
            {
                g_[c] += gStep_[c];
                const float y = dry[c] * g_[c];                         // optical attenuator

                // side chain (feedback): emphasis filter, drive, light panel
                const float lp = scLow_[c].process(y);
                const float sc = (y - lp) + lowGain * lp;
                float e = std::fabs(sc) * d - 1.0f;
                if (e > 0.0f)
                {
                    if (e > 30.0f) e = 30.0f;
                    const float e2 = e * e;
                    uAcc_[c] += limit ? kCellK * e2 * (1.0f + kLimitL * e2 * e2 * e2) : kCellK * e2;
                }

                // tube make-up stage: soft, slightly asymmetric, ceiling about +3.5 dBFS
                float t = mpc::clamp(y * mk * (1.0f / 1.5f), -4.0f, 4.0f);
                t = 1.5f * fastTanh(t + 0.05f * t * t);
                wet[c] = dc_[c].process(t) * og;
            }

            if (++ctlCount_ >= kCtl)                                     // the optical cell, 1/8 rate
            {
                ctlCount_ = 0;
                float u0 = uAcc_[0] * invCtl, u1 = uAcc_[1] * invCtl;
                uAcc_[0] = uAcc_[1] = 0.0f;
                if (link) { u0 = u1 = (u0 > u1 ? u0 : u1); }
                if (u0 > 1.0e9f) u0 = 1.0e9f;
                if (u1 > 1.0e9f) u1 = 1.0e9f;
                const float gr0 = cell_[0].update(u0), gr1 = cell_[1].update(u1);
                gStep_[0] = (std::pow(10.0f, -0.05f * gr0) - g_[0]) * invCtl;
                gStep_[1] = (std::pow(10.0f, -0.05f * gr1) - g_[1]) * invCtl;
            }

            const float l = dry[0] + (wet[0] - dry[0]) * pw;
            const float r = dry[1] + (wet[1] - dry[1]) * pw;
            msAcc += 0.5f * (l * l + r * r);
            outL[i] = sanitize(l);
            if (outR) outR[i] = sanitize(r);
        }
        for (int c = 0; c < 2; ++c) { g_[c] = flushDenormal(g_[c]); }

        // ----- VU meter (display parameter) -----------------------------------
        const float nf = (float)n;
        if (holdVu_ > 0) { holdVu_ -= n; return; }
        outMs_ += (1.0f - std::exp(-nf / (0.15f * sr_))) * (msAcc / nf - outMs_);
        float pos;
        const int meter = getInt(kMeter);
        if (!getBool(kPower)) pos = 0.0f;                                // lamp off, needle at rest left
        else if (meter == 1)                                             // gain reduction: 0 VU = none
        {
            const float gr = cell_[0].gr > cell_[1].gr ? cell_[0].gr : cell_[1].gr;
            pos = std::pow(10.0f, -0.05f * gr) / kVuTop;
        }
        else                                                             // output level: 0 VU = -18 dBFS (+4) or -12 dBFS (+10)
        {
            const float ref = meter == 2 ? 0.12589f : 0.25119f;
            pos = std::sqrt(outMs_) / (ref * kVuTop);
        }
        pos = mpc::clamp(pos, 0.0f, 1.0f);
        needle_ += (1.0f - std::exp(-nf / (0.06f * sr_))) * (pos - needle_);
        setNormalised(kVU, needle_);
    }

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "uP2no2A";
        info.vendor       = "uP2noGood";
        info.uniqueId     = MPC_FOURCC('U', 'J', 'F', 'y');   // == "uid" in plugin.json
        info.version      = 100;                                // 0.1.0.0
        info.isInstrument = false;
        info.numInputs    = 2;
        info.numOutputs   = 2;
        info.tailSamples  = 1;
        return info;
    }

    float makeupGain() const { return dbToGain((get(kGain) - 40.0f) * 0.5f); }        // 0..100 -> -20..+30 dB
    float scDrive() const { return std::pow(10.0f, 0.02f * get(kPeakRed)); }           // 0..100 -> 0..+40 dB

    // The MPC only redraws a control when it is told the parameter moved, so a small thread (never
    // the audio thread) reports the needle about 30 times a second while it is moving.
    void notifierLoop()
    {
        float sent = -1.0f, last = -1.0f;
        while (running_)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
            const float v = getNormalised(kVU);
            const bool moved = std::fabs(v - sent) > (1.0f / 126.0f);
            const bool settled = v == last && v != sent;
            last = v;
            if (!moved && !settled) continue;
            sent = v;
            notifyHostParameterChanged(kVU);
        }
    }

    OptoCell  cell_[2];
    OnePoleLP scLow_[2];
    DCBlocker dc_[2];
    Smoother  makeup_, output_, power_, drive_;
    float g_[2] = {1.0f, 1.0f}, gStep_[2] = {0.0f, 0.0f}, uAcc_[2] = {0.0f, 0.0f};
    int   ctlCount_ = 0, holdVu_ = 0;
    float sr_ = 44100.0f, outMs_ = 0.0f, needle_ = 0.708f;
    std::atomic<bool> running_{false};
    std::thread notifier_;
};

MPC_PLUGIN_ENTRY(UP2no2A)
