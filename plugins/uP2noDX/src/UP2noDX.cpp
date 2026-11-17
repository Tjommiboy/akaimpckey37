// uP2noDX - a 6-operator FM synth for Akai MPC standalones by uP2noGood, compatible with DX7
// voice data. "DX7" is a trademark of Yamaha Corporation; this project is not affiliated with
// Yamaha. No Yamaha ROM data or patches are included.
//
// Sound engine: msfa (Apache License 2.0, see msfa/README.md), the DX7-accurate engine also used
// by Dexed. This file (MIT) is the MPC glue: parameters, voices, MIDI, cartridge loading.
//
// Signal flow:
//   MIDI -> voice allocation (16 voices, or mono) -> per voice: 6 operators wired by one of 32
//   algorithms, each with its own 4-rate/4-level envelope, keyboard level/rate scaling and
//   velocity sensitivity; pitch EG and LFO shared -> sum -> optional "Mk I" DAC stage -> Volume.
//
// Parameters map 1:1 onto the 155-byte DX7 voice format (plus operator on/off), so cartridge
// voices (.syx) load exactly. Operators are numbered 1-6 like the DX7 (stored OP6 first in the
// voice data, as in the DX7 itself).
//
// Cartridges: put DX7 32-voice bank files (*.syx, 4104 bytes, or raw 4096-byte banks) in the
// plugin's Content/Cartridges/ folder. Every voice becomes a program in the MPC preset list,
// after the built-in uP2noGood presets.

#include "mpc_plugin.h"
#include "mpc_resources.h"
#include "dsp/math.h"

#include "msfa/synth.h"
#include "msfa/dx7note.h"
#include "msfa/lfo.h"
#include "msfa/freqlut.h"
#include "msfa/sin.h"
#include "msfa/exp2.h"
#include "msfa/pitchenv.h"
#include "msfa/porta.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Parameter layout. ORDER AND IDS ARE PERMANENT: only ever append at the end.
//   0            Volume
//   1 .. 126     operators 1-6, 21 parameters each, in DX7 order
//   127 .. 144   voice parameters (pitch EG, algorithm, feedback, LFO, transpose)
//   145 .. 150   operator on/off
//   151 ..       performance settings (not part of a DX7 voice; programs leave them alone)
// ---------------------------------------------------------------------------
constexpr int kVolume = 0;
constexpr int kOpBase = 1;          // + (op-1)*21 + field
constexpr int kOpFields = 21;
enum OpField { R1, R2, R3, R4, L1, L2, L3, L4, BRK, LDEP, RDEP, LCUR, RCUR, RSCL, AMS, VEL, OUT, MODE, COARSE, FINE, DETUNE };
constexpr int kVoiceBase = kOpBase + 6 * kOpFields;   // 127
enum VoiceField { PR1, PR2, PR3, PR4, PL1, PL2, PL3, PL4, ALG, FDBK, OSYNC, LSPEED, LDELAY, LPMD, LAMD, LSYNC, LWAVE, PMS, TRANSP, kNumVoiceFields };
constexpr int kOpOnBase = kVoiceBase + kNumVoiceFields;  // 146
constexpr int kPerfBase = kOpOnBase + 6;                  // 152
enum PerfField { POLY, BEND, PORTA_ON, PORTA_TIME, WHL_RANGE, WHL_PITCH, WHL_AMP, WHL_EG, AT_RANGE, AT_PITCH, AT_AMP, AT_EG, MK1, kNumPerfFields };
constexpr int kNumParams = kPerfBase + kNumPerfFields;    // 165
constexpr int kNumVoiceParams = kPerfBase;                // programs set parameters 0 .. kPerfBase-1

constexpr int opParam(int op, int field) { return kOpBase + (op - 1) * kOpFields + field; }   // op = 1..6
constexpr int opByte(int op, int field) { return (6 - op) * kOpFields + field; }             // DX7 stores OP6 first

constexpr int kMaxVoices = 16;

// A DX7 voice in the 155-byte unpacked format, plus byte 155 = operator on/off bits (msfa).
struct Voice155 { uint8_t b[156]; char name[11]; };

// ---- built-in voices --------------------------------------------------------
struct OpSpec { int r[4], l[4], out, coarse, fine, det, vel, rs, bp, ld, rd, lc, rc, ams; };
constexpr OpSpec OFF = { {99, 99, 99, 99}, {99, 99, 99, 0}, 0, 1, 0, 0, 0, 0, 39, 0, 0, 0, 0, 0 };

Voice155 makeVoice(const char* name, int alg, int fb, const OpSpec ops[6], int lfoSpeed = 35, int lfoDelay = 0,
                   int pmd = 0, int amd = 0, int lfoWave = 0, int pms = 3, const int* peg = nullptr)
{
    Voice155 v; std::memset(&v, 0, sizeof(v));
    for (int op = 1; op <= 6; ++op)
    {
        const OpSpec& s = ops[op - 1];
        uint8_t* o = v.b + opByte(op, 0);
        for (int i = 0; i < 4; ++i) { o[R1 + i] = (uint8_t)s.r[i]; o[L1 + i] = (uint8_t)s.l[i]; }
        o[BRK] = (uint8_t)s.bp; o[LDEP] = (uint8_t)s.ld; o[RDEP] = (uint8_t)s.rd; o[LCUR] = (uint8_t)s.lc; o[RCUR] = (uint8_t)s.rc;
        o[RSCL] = (uint8_t)s.rs; o[AMS] = (uint8_t)s.ams; o[VEL] = (uint8_t)s.vel; o[OUT] = (uint8_t)s.out;
        o[MODE] = 0; o[COARSE] = (uint8_t)s.coarse; o[FINE] = (uint8_t)s.fine; o[DETUNE] = (uint8_t)(s.det + 7);
    }
    const int defPeg[8] = { 99, 99, 99, 99, 50, 50, 50, 50 };
    const int* pg = peg ? peg : defPeg;
    for (int i = 0; i < 8; ++i) v.b[126 + i] = (uint8_t)pg[i];
    v.b[134] = (uint8_t)(alg - 1); v.b[135] = (uint8_t)fb; v.b[136] = 1;
    v.b[137] = (uint8_t)lfoSpeed; v.b[138] = (uint8_t)lfoDelay; v.b[139] = (uint8_t)pmd; v.b[140] = (uint8_t)amd;
    v.b[141] = 1; v.b[142] = (uint8_t)lfoWave; v.b[143] = (uint8_t)pms; v.b[144] = 24;
    std::snprintf(v.name, sizeof(v.name), "%-10s", name);
    std::memcpy(v.b + 145, v.name, 10);
    v.b[155] = 0x3f;
    return v;
}

std::vector<Voice155> builtInVoices()
{
    std::vector<Voice155> out;
    {   // INIT VOICE: one sine (operator 1)
        OpSpec o[6] = { { {99, 99, 99, 99}, {99, 99, 99, 0}, 99, 1, 0, 0, 0, 0, 39, 0, 0, 0, 0, 0 }, OFF, OFF, OFF, OFF, OFF };
        out.push_back(makeVoice("INIT VOICE", 1, 0, o));
    }
    {   // Tine electric piano: three modulator->carrier pairs (algorithm 5); op 4 at ratio 14 is the tine.
        OpSpec o[6] = {
            { {96, 25, 25, 67}, {99, 75, 0, 0}, 99, 1, 0, 3, 2, 3, 41, 0, 19, 0, 0, 0 },     // 1 carrier
            { {95, 50, 35, 78}, {99, 75, 0, 0}, 58, 1, 0, 0, 7, 3, 41, 0, 0, 0, 0, 0 },      // 2 modulator (body)
            { {95, 20, 20, 50}, {99, 95, 0, 0}, 99, 1, 0, -3, 2, 3, 41, 0, 0, 0, 0, 0 },     // 3 carrier
            { {95, 29, 20, 50}, {99, 95, 0, 0}, 78, 14, 0, 0, 6, 3, 41, 0, 30, 0, 0, 0 },    // 4 tine
            { {95, 20, 20, 50}, {99, 95, 0, 0}, 79, 1, 0, 0, 2, 3, 41, 0, 0, 0, 0, 0 },      // 5 carrier (soft)
            { {95, 29, 20, 50}, {99, 95, 0, 0}, 70, 1, 0, 0, 6, 3, 41, 0, 0, 0, 0, 0 },      // 6 modulator
        };
        out.push_back(makeVoice("uP TINES", 5, 6, o, 34, 33, 0, 0, 4, 0));
    }
    {   // Solid FM bass: stacked modulators into one carrier (algorithm 16 style), fast decays.
        OpSpec o[6] = {
            { {99, 55, 25, 70}, {99, 80, 70, 0}, 99, 0, 0, 0, 2, 2, 39, 0, 0, 0, 0, 0 },     // 1 carrier (ratio 0.5)
            { {99, 60, 30, 70}, {99, 70, 0, 0}, 82, 1, 0, 0, 5, 2, 39, 0, 0, 0, 0, 0 },      // 2
            { {99, 70, 30, 70}, {99, 60, 0, 0}, 70, 1, 0, 2, 5, 2, 39, 0, 0, 0, 0, 0 },      // 3
            { {99, 60, 30, 70}, {99, 50, 0, 0}, 74, 3, 0, 0, 6, 2, 39, 0, 0, 0, 0, 0 },      // 4
            { {99, 80, 30, 70}, {99, 0, 0, 0}, 60, 5, 0, 0, 6, 2, 39, 0, 0, 0, 0, 0 },       // 5
            { {99, 80, 30, 70}, {99, 0, 0, 0}, 72, 1, 0, 0, 3, 2, 39, 0, 0, 0, 0, 0 },       // 6 (feedback)
        };
        out.push_back(makeVoice("uP BASS", 16, 5, o));
    }
    {   // Bells: inharmonic ratios, long decays (algorithm 5).
        OpSpec o[6] = {
            { {95, 30, 22, 35}, {99, 70, 0, 0}, 99, 1, 0, 0, 3, 2, 50, 0, 20, 0, 0, 0 },
            { {95, 35, 22, 35}, {99, 60, 0, 0}, 78, 3, 50, 0, 5, 2, 50, 0, 0, 0, 0, 0 },     // 3.5
            { {95, 28, 20, 35}, {99, 70, 0, 0}, 92, 2, 0, 3, 3, 2, 50, 0, 20, 0, 0, 0 },
            { {95, 40, 20, 35}, {99, 50, 0, 0}, 72, 7, 4, 0, 5, 2, 50, 0, 0, 0, 0, 0 },      // ~7.28
            { {95, 25, 18, 35}, {99, 70, 0, 0}, 85, 4, 0, -3, 3, 2, 50, 0, 20, 0, 0, 0 },
            { {95, 45, 20, 35}, {99, 40, 0, 0}, 66, 11, 0, 0, 5, 2, 50, 0, 0, 0, 0, 0 },
        };
        out.push_back(makeVoice("uP BELLS", 5, 0, o));
    }
    {   // Brass: three-modulator stack with slow attack on the modulators (algorithm 22).
        OpSpec o[6] = {
            { {72, 76, 99, 71}, {99, 88, 96, 0}, 98, 1, 0, 0, 0, 2, 39, 0, 0, 0, 0, 0 },     // carrier
            { {62, 51, 29, 71}, {82, 95, 96, 0}, 86, 1, 0, 0, 3, 2, 39, 0, 0, 0, 0, 0 },     // modulator
            { {72, 76, 99, 71}, {99, 88, 96, 0}, 98, 1, 0, 2, 0, 2, 39, 0, 0, 0, 0, 0 },
            { {72, 76, 99, 71}, {99, 88, 96, 0}, 98, 1, 0, -2, 0, 2, 39, 0, 0, 0, 0, 0 },
            { {72, 76, 99, 71}, {99, 88, 96, 0}, 96, 1, 0, 1, 0, 2, 39, 0, 0, 0, 0, 0 },
            { {53, 41, 30, 71}, {99, 80, 85, 0}, 80, 1, 0, 0, 3, 2, 39, 0, 0, 0, 0, 0 },     // feedback mod
        };
        int peg[8] = { 84, 95, 95, 60, 49, 50, 50, 50 };   // small pitch rise into each note
        out.push_back(makeVoice("uP BRASS", 22, 7, o, 37, 0, 5, 0, 4, 3, peg));
    }
    {   // Marimba: short percussive pairs (algorithm 5).
        OpSpec o[6] = {
            { {99, 40, 25, 45}, {99, 0, 0, 0}, 99, 1, 0, 0, 2, 3, 39, 0, 15, 0, 0, 0 },
            { {99, 75, 25, 45}, {99, 0, 0, 0}, 70, 4, 0, 0, 6, 3, 39, 0, 0, 0, 0, 0 },
            { {99, 45, 25, 45}, {99, 0, 0, 0}, 85, 1, 0, 2, 2, 3, 39, 0, 15, 0, 0, 0 },
            { {99, 85, 25, 45}, {99, 0, 0, 0}, 60, 10, 0, 0, 6, 3, 39, 0, 0, 0, 0, 0 },
            { {99, 50, 25, 45}, {99, 0, 0, 0}, 0, 1, 0, 0, 0, 3, 39, 0, 0, 0, 0, 0 },
            { {99, 50, 25, 45}, {99, 0, 0, 0}, 0, 1, 0, 0, 0, 3, 39, 0, 0, 0, 0, 0 },
        };
        out.push_back(makeVoice("uP MARIMBA", 5, 0, o));
    }
    {   // Warm pad: six carriers, slow envelopes, gentle detune (algorithm 32).
        OpSpec o[6] = {
            { {45, 30, 20, 40}, {99, 90, 85, 0}, 92, 1, 0, 0, 1, 0, 39, 0, 0, 0, 0, 1 },
            { {42, 30, 20, 40}, {99, 90, 85, 0}, 90, 1, 0, 3, 1, 0, 39, 0, 0, 0, 0, 1 },
            { {40, 30, 20, 40}, {99, 90, 85, 0}, 86, 2, 0, -3, 1, 0, 39, 0, 0, 0, 0, 1 },
            { {44, 30, 20, 40}, {99, 90, 85, 0}, 80, 2, 0, 4, 1, 0, 39, 0, 0, 0, 0, 1 },
            { {38, 30, 20, 40}, {99, 90, 85, 0}, 74, 3, 0, -2, 1, 0, 39, 0, 0, 0, 0, 1 },
            { {46, 30, 20, 40}, {99, 90, 85, 0}, 72, 1, 0, -5, 1, 0, 39, 0, 0, 0, 0, 1 },
        };
        out.push_back(makeVoice("uP PAD", 32, 2, o, 30, 40, 4, 10, 4, 2));
    }
    return out;
}

// ---- cartridges ----------------------------------------------------------------
// Unpack one 128-byte packed DX7 voice (bank format) into 155 bytes. Values are clamped, so a
// damaged bank cannot produce out-of-range parameters.
void unpackVoice(const uint8_t* p, Voice155& v)
{
    std::memset(&v, 0, sizeof(v));
    for (int op = 0; op < 6; ++op)          // stored OP6 first, same as unpacked
    {
        const uint8_t* s = p + op * 17;
        uint8_t* d = v.b + op * 21;
        for (int i = 0; i < 11; ++i) d[i] = (uint8_t)std::min<int>(s[i], 99);
        d[LCUR]   = s[11] & 3;
        d[RCUR]   = (s[11] >> 2) & 3;
        d[RSCL]   = s[12] & 7;
        d[DETUNE] = (uint8_t)std::min<int>((s[12] >> 3) & 15, 14);
        d[AMS]    = s[13] & 3;
        d[VEL]    = (s[13] >> 2) & 7;
        d[OUT]    = (uint8_t)std::min<int>(s[14], 99);
        d[MODE]   = s[15] & 1;
        d[COARSE] = (s[15] >> 1) & 31;
        d[FINE]   = (uint8_t)std::min<int>(s[16], 99);
    }
    for (int i = 0; i < 8; ++i) v.b[126 + i] = (uint8_t)std::min<int>(p[102 + i], 99);
    v.b[134] = p[110] & 31;
    v.b[135] = p[111] & 7;
    v.b[136] = (p[111] >> 3) & 1;
    for (int i = 0; i < 4; ++i) v.b[137 + i] = (uint8_t)std::min<int>(p[112 + i], 99);
    v.b[141] = p[116] & 1;
    v.b[142] = (uint8_t)std::min<int>((p[116] >> 1) & 7, 5);
    v.b[143] = (p[116] >> 4) & 7;
    v.b[144] = (uint8_t)std::min<int>(p[117], 48);
    for (int i = 0; i < 10; ++i)
    {
        char c = (char)(p[118 + i] & 0x7f);
        v.name[i] = (c >= 32 && c < 127) ? c : ' ';
        v.b[145 + i] = (uint8_t)v.name[i];
    }
    v.name[10] = 0;
    v.b[155] = 0x3f;
}

// Reads every *.syx in dir (sorted by name). Returns voices; bank names go into bankOf.
void loadCartridges(const std::string& dir, std::vector<Voice155>& voices, std::vector<std::string>& bankOf)
{
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> files;
    while (dirent* e = readdir(d))
    {
        std::string n = e->d_name;
        if (n.size() > 4 && n[0] != '.')
        {
            std::string ext = n.substr(n.size() - 4);
            for (char& c : ext) c = (char)std::tolower((unsigned char)c);
            if (ext == ".syx") files.push_back(n);
        }
    }
    closedir(d);
    std::sort(files.begin(), files.end());
    for (const std::string& f : files)
    {
        FILE* fp = std::fopen((dir + "/" + f).c_str(), "rb");
        if (!fp) continue;
        std::vector<uint8_t> data(8192);
        size_t n = std::fread(data.data(), 1, data.size(), fp);
        std::fclose(fp);
        const uint8_t* bank = nullptr;
        if (n >= 4104 && data[0] == 0xF0 && data[1] == 0x43 && data[3] == 0x09) bank = data.data() + 6;   // 32-voice bulk dump
        else if (n == 4096) bank = data.data();                                                           // raw bank
        if (!bank) continue;
        std::string stem = f.substr(0, f.size() - 4);
        for (int i = 0; i < 32; ++i)
        {
            Voice155 v;
            unpackVoice(bank + i * 128, v);
            voices.push_back(v);
            bankOf.push_back(stem);
        }
    }
}

std::string trimName(const char* n)
{
    std::string s(n);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(0, 1);
    return s.empty() ? "--" : s;
}

const char* kNoteNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

} // namespace

class UP2noDX : public mpc::Plugin
{
public:
    UP2noDX() : mpc::Plugin(makeInfo())
    {
        addParam(mpc::floatParam("volume", "Volume", -40.0f, 6.0f, -6.0f, "dB", 1));
        const std::vector<std::string> curves = { "-LIN", "-EXP", "+EXP", "+LIN" };
        for (int op = 1; op <= 6; ++op)
        {
            auto id = [op](const char* s) { return "op" + std::to_string(op) + "_" + s; };
            auto nm = [op](const char* s) { return "OP" + std::to_string(op) + " " + s; };
            for (int i = 0; i < 4; ++i)
                addParam(mpc::intParam(id(("r" + std::to_string(i + 1)).c_str()).c_str(), nm(("R" + std::to_string(i + 1)).c_str()).c_str(), 0, 99, 99));
            for (int i = 0; i < 4; ++i)
                addParam(mpc::intParam(id(("l" + std::to_string(i + 1)).c_str()).c_str(), nm(("L" + std::to_string(i + 1)).c_str()).c_str(), 0, 99, i == 3 ? 0 : 99));
            addParam(mpc::intParam(id("break").c_str(), nm("BrkPt").c_str(), 0, 99, 39));
            addParam(mpc::intParam(id("ldepth").c_str(), nm("L Depth").c_str(), 0, 99, 0));
            addParam(mpc::intParam(id("rdepth").c_str(), nm("R Depth").c_str(), 0, 99, 0));
            addParam(mpc::choiceParam(id("lcurve").c_str(), nm("L Curve").c_str(), curves, 0));
            addParam(mpc::choiceParam(id("rcurve").c_str(), nm("R Curve").c_str(), curves, 0));
            addParam(mpc::intParam(id("ratescl").c_str(), nm("RateScl").c_str(), 0, 7, 0));
            addParam(mpc::intParam(id("ams").c_str(), nm("AMS").c_str(), 0, 3, 0));
            addParam(mpc::intParam(id("velo").c_str(), nm("Velo").c_str(), 0, 7, 0));
            addParam(mpc::intParam(id("level").c_str(), nm("Level").c_str(), 0, 99, op == 1 ? 99 : 0));
            addParam(mpc::choiceParam(id("mode").c_str(), nm("Mode").c_str(), { "Ratio", "Fixed" }, 0));
            addParam(mpc::intParam(id("coarse").c_str(), nm("Coarse").c_str(), 0, 31, 1));
            addParam(mpc::intParam(id("fine").c_str(), nm("Fine").c_str(), 0, 99, 0));
            addParam(mpc::intParam(id("detune").c_str(), nm("Detune").c_str(), -7, 7, 0));
        }
        const char* pegR[4] = { "peg_r1", "peg_r2", "peg_r3", "peg_r4" };
        const char* pegL[4] = { "peg_l1", "peg_l2", "peg_l3", "peg_l4" };
        const char* pegRN[4] = { "PEG R1", "PEG R2", "PEG R3", "PEG R4" };
        const char* pegLN[4] = { "PEG L1", "PEG L2", "PEG L3", "PEG L4" };
        for (int i = 0; i < 4; ++i) addParam(mpc::intParam(pegR[i], pegRN[i], 0, 99, 99));
        for (int i = 0; i < 4; ++i) addParam(mpc::intParam(pegL[i], pegLN[i], 0, 99, 50));
        addParam(mpc::intParam("algorithm", "Algorithm", 1, 32, 1));
        addParam(mpc::intParam("feedback", "Feedback", 0, 7, 0));
        addParam(mpc::boolParam("osc_sync", "Osc Sync", true));
        addParam(mpc::intParam("lfo_speed", "LFO Speed", 0, 99, 35));
        addParam(mpc::intParam("lfo_delay", "LFO Delay", 0, 99, 0));
        addParam(mpc::intParam("lfo_pmd", "LFO PMD", 0, 99, 0));
        addParam(mpc::intParam("lfo_amd", "LFO AMD", 0, 99, 0));
        addParam(mpc::boolParam("lfo_sync", "LFO Sync", true));
        addParam(mpc::choiceParam("lfo_wave", "LFO Wave", { "Triangle", "Saw Down", "Saw Up", "Square", "Sine", "S/Hold" }, 0));
        addParam(mpc::intParam("pms", "Pitch Sens", 0, 7, 3));
        addParam(mpc::intParam("transpose", "Transpose", -24, 24, 0, "st"));
        for (int op = 1; op <= 6; ++op)
            addParam(mpc::boolParam(("op" + std::to_string(op) + "_on").c_str(), ("OP" + std::to_string(op) + " On").c_str(), true));
        addParam(mpc::choiceParam("poly", "Poly/Mono", { "Poly", "Mono" }, 0));
        addParam(mpc::intParam("bend", "Bend Range", 0, 12, 2, "st"));
        addParam(mpc::boolParam("porta_on", "Porta On", false));
        addParam(mpc::intParam("porta_time", "Porta Time", 0, 99, 30));
        addParam(mpc::intParam("whl_range", "Wheel Range", 0, 99, 50));
        addParam(mpc::boolParam("whl_pitch", "Wheel>Pitch", true));
        addParam(mpc::boolParam("whl_amp", "Wheel>Amp", false));
        addParam(mpc::boolParam("whl_eg", "Wheel>EG", false));
        addParam(mpc::intParam("at_range", "AT Range", 0, 99, 0));
        addParam(mpc::boolParam("at_pitch", "AT>Pitch", false));
        addParam(mpc::boolParam("at_amp", "AT>Amp", false));
        addParam(mpc::boolParam("at_eg", "AT>EG", false));
        addParam(mpc::boolParam("mk1", "Mk I", false));

        // Programs: built-in voices, then every voice of every cartridge in Content/Cartridges.
        std::vector<Voice155> voices = builtInVoices();
        std::vector<std::string> bankOf(voices.size(), "");
        loadCartridges(mpc::contentPath("Cartridges"), voices, bankOf);
        for (size_t i = 0; i < voices.size(); ++i)
        {
            std::string name = trimName(voices[i].name);
            if (!bankOf[i].empty()) name = bankOf[i].substr(0, 10) + " " + name;
            addProgram(name.c_str(), voiceToPlain(voices[i]));
        }

        tuning_ = createStandardTuning();
        for (int i = 0; i < kMaxVoices; ++i) notes_[i] = std::make_unique<Dx7Note>(tuning_, nullptr);
    }

    void prepare(double sampleRate, int maxBlockSize) override
    {
        (void)maxBlockSize;
        // msfa's tables are global; every instance uses the MPC's single sample rate.
        Exp2::init(); Tanh::init(); Sin::init();
        Freqlut::init(sampleRate);
        Lfo::init(sampleRate);
        PitchEnv::init(sampleRate);
        Env::init_sr(sampleRate);
        Porta::init_sr(sampleRate);
        reset();
    }

    void reset() override
    {
        for (int i = 0; i < kMaxVoices; ++i) voice_[i] = VoiceState();   // notes are re-initialised on their next note-on
        numHeld_ = 0; sustain_ = false; counter_ = 0; lastVoice_ = -1;
        bufPos_ = N;
        modwheel_ = 0; aftertouch_ = 0; pitchbend_ = 0x2000;
        buildPatch();
        lfo_.reset(patch_ + 137);
        dirty_.store(false);
        gain_ = dbToGain(get(kVolume));
    }

    void parameterChanged(int index, float plainValue) override
    {
        (void)plainValue;
        if (index > 0) dirty_.store(true, std::memory_order_relaxed);
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        if (index >= kOpBase && index < kVoiceBase)
        {
            const int op = (index - kOpBase) / kOpFields + 1;
            const int field = (index - kOpBase) % kOpFields;
            if (field == BRK)
            {
                const int note = (int)std::lround(plain) + 21;                 // break point 0 = A-1
                out = std::string(kNoteNames[note % 12]) + std::to_string(note / 12 - 2);
                return true;
            }
            if (field == COARSE && getInt(opParam(op, MODE)) == 0)
            {
                const int c = (int)std::lround(plain);
                const float ratio = (c == 0 ? 0.5f : (float)c) * (1.0f + 0.01f * get(opParam(op, FINE)));
                char b[16]; std::snprintf(b, sizeof(b), "x%.2f", ratio); out = b;
                return true;
            }
        }
        return false;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int n = ctx.numFrames;
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        const float targetGain = dbToGain(get(kVolume));
        const bool mk1 = getBool(kPerfBase + MK1);

        int ev = 0;
        for (int i = 0; i < n; ++i)
        {
            if (bufPos_ >= N)
            {
                // MIDI events up to the end of the next 64-sample chunk start it (<= 1.5 ms jitter).
                while (ev < ctx.numMidiEvents && ctx.midi[ev].sampleOffset <= i) handleMidi(ctx.midi[ev++]);
                renderChunk();
                bufPos_ = 0;
            }
            gain_ += 0.002f * (targetGain - gain_);
            float x = buf_[bufPos_++] * kOutScale;
            if (mk1) x = mk1Dac(x);
            // soft limiter: transparent at normal levels, rounds off dense 16-voice chords instead of clipping
            const float y = mpc::dsp::softClip(mpc::dsp::sanitize(x * gain_));
            outL[i] = y;
            if (outR) outR[i] = y;
        }
        while (ev < ctx.numMidiEvents) handleMidi(ctx.midi[ev++]);
    }

private:
    struct VoiceState { int note = -1; int velocity = 0; bool keyDown = false; bool sustained = false; bool live = false; uint32_t age = 0; };

    static constexpr float kOutScale = 1.0f / (float)(1 << 24) * 0.1f;   // msfa output: Q24 per carrier at full level

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "uP2noDX";
        info.vendor       = "uP2noGood";
        info.uniqueId     = MPC_FOURCC('U', 'T', 'P', 'b');
        info.version      = 100;            // 0.1.0.0
        info.isInstrument = true;
        info.numInputs    = 0;
        info.numOutputs   = 2;
        info.tailSamples  = 0;
        return info;
    }

    static float dbToGain(float dB) { return std::pow(10.0f, dB / 20.0f); }

    // The Mk I's output stage: a 12-bit DAC (sign + 11 bits) behind a x1/x2/x4/x8 analogue
    // scaler, i.e. a float with an 11-bit mantissa and a 2-bit exponent.
    static float mk1Dac(float x)
    {
        const float a = std::fabs(x);
        float scale = 1.0f;                       // pick the smallest range that holds the sample
        if (a < 0.125f) scale = 8.0f; else if (a < 0.25f) scale = 4.0f; else if (a < 0.5f) scale = 2.0f;
        const float q = std::round(mpc::clamp(x * scale, -1.0f, 1.0f) * 2047.0f) / 2047.0f;
        return q / scale;
    }

    // Plain parameter values (0 .. kPerfBase-1) for a voice. Performance settings stay as they are.
    std::vector<float> voiceToPlain(const Voice155& v) const
    {
        std::vector<float> p((size_t)kNumVoiceParams);
        p[kVolume] = param(kVolume).def;
        for (int op = 1; op <= 6; ++op)
            for (int f = 0; f < kOpFields; ++f)
            {
                float val = v.b[opByte(op, f)];
                if (f == DETUNE) val -= 7.0f;
                p[(size_t)opParam(op, f)] = val;
            }
        for (int f = 0; f < kNumVoiceFields; ++f)
        {
            float val = v.b[126 + f];
            if (f == ALG) val += 1.0f;
            if (f == TRANSP) val -= 24.0f;
            p[(size_t)(kVoiceBase + f)] = val;
        }
        for (int op = 1; op <= 6; ++op) p[(size_t)(kOpOnBase + op - 1)] = (v.b[155] >> (6 - op)) & 1 ? 1.0f : 0.0f;
        return p;
    }

    void buildPatch()
    {
        for (int op = 1; op <= 6; ++op)
            for (int f = 0; f < kOpFields; ++f)
            {
                int val = getInt(opParam(op, f));
                if (f == DETUNE) val += 7;
                patch_[opByte(op, f)] = (uint8_t)mpc::clamp(val, 0, 99);
            }
        for (int f = 0; f < kNumVoiceFields; ++f)
        {
            int val = getInt(kVoiceBase + f);
            if (f == ALG) val -= 1;
            if (f == TRANSP) val += 24;
            patch_[126 + f] = (uint8_t)mpc::clamp(val, 0, 99);
        }
        uint8_t sw = 0;
        for (int op = 1; op <= 6; ++op) if (getBool(kOpOnBase + op - 1)) sw |= (uint8_t)(1 << (6 - op));
        patch_[155] = sw;
        transpose_ = getInt(kVoiceBase + TRANSP);

        // performance settings -> msfa controllers
        ctrls_.core = &core_;
        ctrls_.pitch_range_up = ctrls_.pitch_range_dn = getInt(kPerfBase + BEND);
        ctrls_.pitch_step = 0;
        ctrls_.masterTune = 0;
        ctrls_.mpeEnabled = false;
        ctrls_.transpose12AsScale = false;
        ctrls_.portamento_enable_cc = getBool(kPerfBase + PORTA_ON);
        ctrls_.portamento_cc = getInt(kPerfBase + PORTA_TIME) * 127 / 99;
        ctrls_.portamento_gliss_cc = false;
        ctrls_.wheel.range = getInt(kPerfBase + WHL_RANGE);
        ctrls_.wheel.pitch = getBool(kPerfBase + WHL_PITCH);
        ctrls_.wheel.amp = getBool(kPerfBase + WHL_AMP);
        ctrls_.wheel.eg = getBool(kPerfBase + WHL_EG);
        ctrls_.at.range = getInt(kPerfBase + AT_RANGE);
        ctrls_.at.pitch = getBool(kPerfBase + AT_PITCH);
        ctrls_.at.amp = getBool(kPerfBase + AT_AMP);
        ctrls_.at.eg = getBool(kPerfBase + AT_EG);
        ctrls_.foot.range = 0; ctrls_.breath.range = 0;
        ctrls_.breath_cc = 0; ctrls_.foot_cc = 0;
        updateControllers();
    }

    void updateControllers()
    {
        ctrls_.pitch_cc = pitchbend_;
        ctrls_.modwheel_cc = modwheel_;
        ctrls_.aftertouch_cc = aftertouch_;
        ctrls_.refresh();
    }

    void renderChunk()
    {
        if (dirty_.exchange(false, std::memory_order_relaxed))
        {
            buildPatch();
            lfo_.reset(patch_ + 137);
            for (int v = 0; v < kMaxVoices; ++v)
                if (voice_[v].live) notes_[v]->update(patch_, playNote(voice_[v].note), voice_[v].velocity, 1);
        }
        std::memset(buf_, 0, sizeof(buf_));
        const int32_t lfoValue = lfo_.getsample();
        const int32_t lfoDelay = lfo_.getdelay();
        for (int v = 0; v < kMaxVoices; ++v)
        {
            if (!voice_[v].live) continue;
            notes_[v]->compute(buf_, lfoValue, lfoDelay, &ctrls_);
            if (!voice_[v].keyDown && !voice_[v].sustained && !notes_[v]->isPlaying()) voice_[v].live = false;
        }
    }

    int playNote(int note) const { return mpc::clamp(note + transpose_, 0, 127); }

    // ---- MIDI ------------------------------------------------------------------
    void handleMidi(const mpc::MidiEvent& e)
    {
        if (e.isNoteOn()) noteOn(e.note(), e.velocity());
        else if (e.isNoteOff()) noteOff(e.note());
        else if (e.isPitchBend()) { pitchbend_ = e.pitchBend() + 0x2000; updateControllers(); }
        else if (e.isAftertouch()) { aftertouch_ = e.data[1]; updateControllers(); }
        else if (e.isAllNotesOff())
        {
            sustain_ = false; numHeld_ = 0;
            for (int v = 0; v < kMaxVoices; ++v) if (voice_[v].live) { voice_[v].keyDown = voice_[v].sustained = false; notes_[v]->keyup(); }
        }
        else if (e.isController())
        {
            if (e.controller() == 1) { modwheel_ = e.controllerValue(); updateControllers(); }
            else if (e.controller() == 64)
            {
                sustain_ = e.controllerValue() >= 64;
                if (!sustain_)
                    for (int v = 0; v < kMaxVoices; ++v)
                        if (voice_[v].live && voice_[v].sustained) { voice_[v].sustained = false; notes_[v]->keyup(); }
            }
        }
    }

    void startVoice(int v, int note, int velocity, int glideFrom)
    {
        if (dirty_.exchange(false, std::memory_order_relaxed)) { buildPatch(); lfo_.reset(patch_ + 137); }
        notes_[v]->init(patch_, playNote(note), velocity, 1, &ctrls_);
        if (patch_[136]) notes_[v]->oscSync();
        if (glideFrom >= 0 && ctrls_.portamento_enable_cc) notes_[v]->initPortamento(*notes_[glideFrom]);
        voice_[v].note = note; voice_[v].velocity = velocity; voice_[v].keyDown = true;
        voice_[v].sustained = false; voice_[v].live = true; voice_[v].age = ++counter_;
        lfo_.keydown();
        lastVoice_ = v;
    }

    void noteOn(int note, int velocity)
    {
        if (numHeld_ < kMaxHeld) held_[numHeld_++] = note;
        const int glideFrom = lastVoice_;
        if (getInt(kPerfBase + POLY) == 1)
        {
            // Mono: alternate between voices 0 and 1 so the old note can glide into the new one.
            const int v = lastVoice_ == 0 ? 1 : 0;
            if (lastVoice_ >= 0 && voice_[lastVoice_].live) { voice_[lastVoice_].keyDown = false; voice_[lastVoice_].live = false; }
            startVoice(v, note, velocity, glideFrom);
            return;
        }
        // Poly: a free voice, else the oldest released one, else the oldest.
        int best = -1;
        for (int v = 0; v < kMaxVoices; ++v) if (!voice_[v].live) { best = v; break; }
        if (best < 0)
        {
            uint32_t oldest = UINT32_MAX;
            for (int pass = 0; pass < 2 && best < 0; ++pass)
                for (int v = 0; v < kMaxVoices; ++v)
                    if ((pass == 1 || !voice_[v].keyDown) && voice_[v].age < oldest) { oldest = voice_[v].age; best = v; }
        }
        startVoice(best, note, velocity, glideFrom);
    }

    void noteOff(int note)
    {
        for (int i = 0; i < numHeld_; ++i) if (held_[i] == note) { for (int j = i; j < numHeld_ - 1; ++j) held_[j] = held_[j + 1]; --numHeld_; break; }
        if (getInt(kPerfBase + POLY) == 1)
        {
            const int v = lastVoice_;
            if (v < 0 || voice_[v].note != note) return;
            if (numHeld_ > 0)          // fall back to the last key still held
            {
                startVoice(v == 0 ? 1 : 0, held_[numHeld_ - 1], voice_[v].velocity, v);
                voice_[v].live = false;
                return;
            }
        }
        for (int v = 0; v < kMaxVoices; ++v)
        {
            if (voice_[v].live && voice_[v].keyDown && voice_[v].note == note)
            {
                voice_[v].keyDown = false;
                if (sustain_) voice_[v].sustained = true;
                else notes_[v]->keyup();
            }
        }
    }

    static constexpr int kMaxHeld = 32;

    std::shared_ptr<TuningState> tuning_;
    std::unique_ptr<Dx7Note> notes_[kMaxVoices];
    VoiceState voice_[kMaxVoices];
    FmCore core_;
    Controllers ctrls_;
    Lfo lfo_;
    uint8_t patch_[156] = {};
    int32_t buf_[N] = {};
    int bufPos_ = N;
    std::atomic<bool> dirty_ { true };
    float gain_ = 0.5f;
    int transpose_ = 0;
    int held_[kMaxHeld] = {};
    int numHeld_ = 0;
    bool sustain_ = false;
    uint32_t counter_ = 0;
    int lastVoice_ = -1;
    int modwheel_ = 0, aftertouch_ = 0, pitchbend_ = 0x2000;
};

MPC_PLUGIN_ENTRY(UP2noDX)
