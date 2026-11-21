// Bass Brutalizer - an MPC insert effect plugin by comrade fuzz
//
// A bass destroyer: keep the sub clean (or not), and wreck everything else.
//
// Signal flow (per channel):
//
//   dry --+--> 2x lowpass @ Split --> low (clean sub) --- * Sub -------------------+
//         |      (Split On off or Split = 0 -> low = 0: whole signal is "body")    |
//         |                                                                        |
//         +--> body = dry - low                                                    |
//                |                                                                |
//                +--> Pre Tilt (low/high shelf)                                   |
//                +--> [Wobble filter, "Pre"]                                      |
//                +--> * Drive + Bias --> waveshaper (Mode)                        |
//                |      (Multiband: mids and highs shaped separately,             |
//                |       highs get Hi Drive extra)                                |
//                +--> DC blocker --> Ring mod --> Bitcrush / Decimate             |
//                +--> [Wobble filter, "Post"] --> [Vowel stage, "Post"]           |
//                +--> Tone (2 poles) --> comp --> (+)                            |
//                                                                                 |
//   dry --> octave-down generator (zero-cross flip-flop) --> * Oct Sub ----------(+)
//                                                                                 |
//   out = dry + (wet * Output - dry) * Mix   <-----------------------------------+
//
// Vowel stage: three parallel bandpass filters on the formant frequencies of a vowel
// (OO OH UH AH AE EH EE), morphing between two vowels by hand, by an LFO, by the input
// level ("mouth follow") or by a four-step WORD (WOW, YAY, YO, YEAH ...) in the MPC tempo.
// It needs harmonics to work on, so "Post" (after the distortion) is the normal place.
// Gender shifts all formants, Choir stacks three detuned singers, Whisper uses a noise voice,
// Robot snaps between the two vowels.
//
// Wobble: a resonant lowpass (SVF) whose cutoff is swept by an LFO, free-running
// (Hz) or synced to the MPC tempo (Wob Sync). Place it before the shaper ("Pre")
// for a talking, formant-like growl or after the crusher ("Post") for classic
// dubstep wobble.
//
// CPU: a few dozen multiplies per sample per channel; transcendental functions
// run per block or every 16 samples, never per sample. No oversampling, so very
// high drive, crushing or ring mod on bright material aliases (often wanted).

#include "mpc_plugin.h"

#include "dsp/lfo.h"
#include "dsp/math.h"
#include "dsp/onepole.h"
#include "dsp/smoother.h"
#include "dsp/svf.h"
#include "dsp/tempo.h"

#include <chrono>
#include <cmath>
#include <cstdint>

namespace {

// Parameter 0 is toggled by the MPC on every load, so it is the harmless one.
// Parameters are only ever appended: order and ids are permanent.
enum ParamIndex
{
    kOutput = 0,   // dB
    kDrive,        // dB
    kMode,         // choice
    kBias,         // % (asymmetry -> even harmonics)
    kSplit,        // Hz (crossover between clean sub and distorted body; 0 = Off)
    kSub,          // % level of the clean low band
    kTone,         // Hz (post-distortion lowpass)
    kMix,          // % wet
    kSplitOn,      // bool: enables the split chain
    kBits,         // bit depth (16 = off)
    kDecimate,     // sample-rate divider (1 = off)
    kRing,         // % ring modulation
    kRingHz,       // Hz ring modulator frequency
    kOctSub,       // % octave-down sub generator
    kTilt,         // dB tilt before the shaper (+ brighter, - bassier)
    kMultiband,    // bool
    kHiSplit,      // Hz: mid/high crossover for Multiband
    kHiDrive,      // dB extra drive for the highs in Multiband
    kWobPos,       // choice: Off / Pre / Post
    kWobRate,      // Hz (when Wob Sync = Free)
    kWobDepth,     // % sweep depth (100% = +-4 octaves)
    kWobFreq,      // Hz centre frequency of the wobble filter
    kWobReso,      // % resonance
    kWobShape,     // choice: Sine / Tri / Saw / Square
    kWobSync,      // choice: Free + note divisions
    kCrushOn,      // bool: Bits / Decimate section on
    kRingOn,       // bool: ring modulation on
    kWobOn,        // bool: wobble section on (Wob Pos keeps its setting)
    kRedButton,    // momentary: a rising edge replaces every sound setting with a random patch
    kSanity,       // choice: Sober / Drunkish / Bat Shit Crazy (how wild the random patch may be)
    kVowOn,        // bool: vowel section on
    kVowPos,       // choice: Off / Pre / Post
    kVowA,         // choice: first vowel
    kVowB,         // choice: second vowel
    kVowMorph,     // % position between A and B
    kVowMode,      // choice: Manual / LFO / Env / Step
    kVowRate,      // Hz (LFO speed or step speed when Vow Sync = Free)
    kVowSync,      // choice: Free + note divisions
    kVowDepth,     // % modulation depth (LFO / Env)
    kVowGlide,     // ms smoothing of vowel changes
    kVowWord,      // choice: WOW, YAY, YO ... (Step mode)
    kGender,       // semitones: shifts every formant
    kVowQ,         // % sharpness of the vowel
    kVowMix,       // % wet/dry of the vowel stage
    kVowChar,      // choice: Normal / Choir / Whisper / Robot
    kVowTrig,      // choice: Loop / Hit (Word mode: repeat, or play once per hit)
    kVowS1,        // choice: key vowel 1 of the custom word
    kVowS2,
    kVowS3,
    kVowS4,
    kVowAtk,       // ms ADSR attack (Vow Mode = ADSR)
    kVowDec,       // ms ADSR decay
    kVowSus,       // % ADSR sustain
    kVowRel,       // ms ADSR release
    kVowSens,      // % hit detector sensitivity (ADSR and Word / Hit)
    kVowHold,      // ms ADSR note length; 0 = until the level has fallen about 18 dB
    kVowEnv,       // bool: Word / Hit also uses the ADSR, as the amount of the vowel effect
    kVowGap,       // ms minimum time between two hits (retrigger mask)
    kNumParams
};

enum Mode { kSoft = 0, kHard, kFold, kRect };
enum WobPos { kWobOff = 0, kWobPre, kWobPost };

constexpr int   kMaxChannels = 2;
constexpr float kSplitOffHz  = 10.0f;   // Split cutoff below this = Off (same as the switch off)
constexpr int   kWobUpdate   = 16;      // wobble filter coefficients refresh every N samples

// Triangle wavefolder: identity on [-1, 1], reflects everything beyond.
inline float fold(float x)
{
    const float t = 0.25f * x + 0.25f;
    return 4.0f * std::fabs(t - std::floor(t + 0.5f)) - 1.0f;
}

// Waveshaper selector. Input is already driven; output is roughly within +-1.
inline float shape(int mode, float x)
{
    switch (mode)
    {
        case kHard: return mpc::dsp::hardClip(x);
        case kFold: return fold(x);
        case kRect: return mpc::dsp::fastTanh(std::fabs(x)) * 2.0f - 1.0f;
        case kSoft:
        default:    return mpc::dsp::fastTanh(x);
    }
}

// Vowel formants (F1, F2, F3 in Hz; average adult male values) and the four-step words.
struct Formant { float f1, f2, f3; };
// 15 vowels. The first seven keep their indices (words and presets refer to them); the rest were appended.
constexpr int kNumVowels = 15;   // OO OH UH AH AE EH EE | IH AW ER UE OE YI W L
constexpr Formant kVowels[kNumVowels] = {
    {300, 870, 2240}, {500, 830, 2500}, {640, 1190, 2390}, {730, 1090, 2440},
    {660, 1720, 2410}, {530, 1840, 2480}, {270, 2290, 3010},
    {390, 1990, 2550},   // IH  as in "bit"
    {620,  920, 2640},   // AW  as in "saw"
    {490, 1350, 1690},   // ER  r-coloured, the growl vowel
    {290, 1700, 2300},   // UE  German u-umlaut
    {450, 1500, 2500},   // OE  German o-umlaut
    {330, 1600, 2450},   // YI  Russian "y" (ы)
    {290,  610, 2150},   // W   a very dark, rounded oo
    {360, 1300, 2900}};  // L   a dark, forward l
constexpr float kFormGain[3] = {1.0f, 0.9f, 0.6f};
constexpr int kNumWords  = 11;
constexpr int kCustomWord = 10;   // the word built from Step 1..4
// A word is a contour through four key vowels (the filter glides smoothly from key to key).
constexpr int kWords[kNumWords][4] = {
    {0, 3, 1, 0},   // WOW      oo ah oh oo
    {6, 5, 4, 6},   // YAY      ee eh ae ee
    {6, 2, 1, 1},   // YO       ee uh oh oh
    {6, 1, 1, 6},   // YOY      ee oh oh ee
    {6, 5, 4, 3},   // YEAH     ee eh ae ah
    {1, 1, 5, 6},   // YOI      oh oh eh ee
    {0, 2, 3, 3},   // WAH      oo uh ah ah
    {0, 0, 3, 3},   // OOH-AAH  oo oo ah ah
    {5, 1, 0, 0},   // EOW      eh oh oo oo
    {3, 0, 3, 0},   // MAMA     ah oo ah oo
    {0, 3, 1, 0}};  // CUSTOM   (replaced by Step 1..4)
enum VowMode { kVMManual = 0, kVMLfo, kVMAdsr, kVMWord };
// Makeup gain per character (Normal, Choir, Whisper, Robot), measured so that the vowel stage is about
// as loud as the signal it replaces (tests/vowel_test.cpp prints the levels).
constexpr float kVowelMakeup[4] = {6.0f, 3.7f, 7.5f, 9.0f};

struct Channel
{
    // split
    mpc::dsp::OnePoleLP lowA, lowB;          // 12 dB/oct split lowpass
    // pre tilt
    mpc::dsp::OnePoleLP tilt;
    // multiband
    mpc::dsp::OnePoleLP midA, midB;
    // after the shaper
    mpc::dsp::DCBlocker dc;
    float               decAcc = 0.0f, decHold = 0.0f;
    mpc::dsp::SVF       wob;
    mpc::dsp::SVF       vow[9];              // vowel stage: 3 formants x up to 3 singers
    mpc::dsp::OnePoleLP toneA, toneB;        // 12 dB/oct tone lowpass
    // octave-down generator
    mpc::dsp::OnePoleLP octDetA, octDetB;    // detector lowpass
    mpc::dsp::OnePoleLP octOutA, octOutB;    // rounds the square wave
    float               octEnv = 0.0f, octFlip = 1.0f;
    bool                octPos = false;

    void reset()
    {
        lowA.reset(); lowB.reset(); tilt.reset(); midA.reset(); midB.reset();
        dc.reset(); decAcc = 0.0f; decHold = 0.0f; wob.reset(); toneA.reset(); toneB.reset();
        for (mpc::dsp::SVF& f : vow) f.reset();
        octDetA.reset(); octDetB.reset(); octOutA.reset(); octOutB.reset();
        octEnv = 0.0f; octFlip = 1.0f; octPos = false;
    }
};

// Small xorshift random generator for the Red Button (no allocation, no locks).
struct Rng
{
    uint32_t s = 2463534242u;
    float next01() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (float)(s >> 8) * (1.0f / 16777216.0f); }
    float range(float a, float b) { return a + (b - a) * next01(); }
    float logRange(float a, float b) { return a * std::pow(b / a, next01()); }
    bool  chance(float p) { return next01() < p; }
    int   pick(const float* w, int n)   // weighted index
    {
        float sum = 0.0f; for (int i = 0; i < n; ++i) sum += w[i];
        float r = next01() * sum;
        for (int i = 0; i < n; ++i) { r -= w[i]; if (r < 0.0f) return i; }
        return n - 1;
    }
    int   irange(int a, int b) { return a + (int)(next01() * (float)(b - a + 1)) % (b - a + 1); }
};

// Preset builder: every field defaults to the parameter default, so a preset
// only names what it changes. Field order = parameter order.
struct PS
{
    float out = 0, drv = 18, mode = kSoft, bias = 0, split = 100, sub = 100, tone = 6000, mix = 100, splitOn = 1;
    float bits = 16, dec = 1, ring = 0, ringHz = 150, oct = 0, tilt = 0, multi = 0, hiSplit = 3000, hiDrv = 0;
    float wobPos = kWobOff, wobRate = 2, wobDepth = 50, wobFreq = 600, wobReso = 40, wobShape = 0, wobSync = 0;
    float crushOn = 1, ringOn = 1, wobOn = 1;
    float vowOn = 0, vowPos = 2, vowA = 0, vowB = 3, vowMorph = 0, vowMode = 3, vowRate = 2, vowSync = 0, vowDepth = 70,
          vowGlide = 25, vowWord = 0, gender = 0, vowQ = 70, vowMix = 100, vowChar = 0;
    float vowTrig = 0, s1 = 0, s2 = 3, s3 = 1, s4 = 0;
    float vowAtk = 120, vowDec = 300, vowSus = 70, vowRel = 200, vowSens = 50, vowHold = 0, vowEnv = 0, vowGap = 150;

    // vowel stage: pos 0 Off / 1 Pre / 2 Post; mode 0 Manual / 1 LFO / 2 Env / 3 Step
    PS& Vow(float pos, float modeIdx, float word, float syncIdx, float rate = 2)
    { vowOn = 1; vowPos = pos; vowMode = modeIdx; vowWord = word; vowSync = syncIdx; vowRate = rate; return *this; }   // a preset that calls Vow() turns the vowel stage on
    PS& Adsr(float a, float d, float sus, float r) { vowAtk = a; vowDec = d; vowSus = sus; vowRel = r; return *this; }
    PS& Gap(float ms) { vowGap = ms; return *this; }   // minimum time between two hits
    PS& WordEnv() { vowEnv = 1; return *this; }   // a Hit word is also shaped by the ADSR
    PS& Trigger(float sensPct, float holdMs) { vowSens = sensPct; vowHold = holdMs; return *this; }
    PS& Hit() { vowTrig = 1; return *this; }   // Word mode: play the word once per hit
    PS& Custom(float a, float b, float c, float d) { vowWord = kCustomWord; s1 = a; s2 = b; s3 = c; s4 = d; return *this; }
    PS& VowAB(float a, float b, float morph, float depth) { vowA = a; vowB = b; vowMorph = morph; vowDepth = depth; return *this; }
    PS& Voice(float genderSt, float q, float mixPct, float charIdx, float glideMs = 25)
    { gender = genderSt; vowQ = q; vowMix = mixPct; vowChar = charIdx; vowGlide = glideMs; return *this; }

    PS& Out(float v)      { out = v; return *this; }
    PS& Drv(float v)      { drv = v; return *this; }
    PS& Mode(float v)     { mode = v; return *this; }
    PS& Bias(float v)     { bias = v; return *this; }
    PS& Split(float v)    { split = v; return *this; }
    PS& Sub(float v)      { sub = v; return *this; }
    PS& Tone(float v)     { tone = v; return *this; }
    PS& Mix(float v)      { mix = v; return *this; }
    PS& SplitOff()        { splitOn = 0; return *this; }
    PS& Bits(float v)     { bits = v; return *this; }
    PS& Dec(float v)      { dec = v; return *this; }
    PS& Ring(float amt, float hz) { ring = amt; ringHz = hz; return *this; }
    PS& Oct(float v)      { oct = v; return *this; }
    PS& Tilt(float v)     { tilt = v; return *this; }
    PS& Multi(float hiSplitHz, float hiDrive) { multi = 1; hiSplit = hiSplitHz; hiDrv = hiDrive; return *this; }
    PS& Wob(float pos, float freq, float depth, float reso, float shapeIdx, float syncIdx, float rate = 2)
    { wobPos = pos; wobFreq = freq; wobDepth = depth; wobReso = reso; wobShape = shapeIdx; wobSync = syncIdx; wobRate = rate; return *this; }
};

// Wob Sync choice index = 1 + division index in dsp/tempo.h.
constexpr float kSync_1_16 = 5, kSync_1_8T = 6, kSync_1_8 = 8, kSync_1_4 = 11;

} // namespace

class BassBrutalizer : public mpc::Plugin
{
public:
    BassBrutalizer() : mpc::Plugin(makeInfo())
    {
        // Order must match ParamIndex. Ids are permanent once released.
        addParam(mpc::floatParam ("output", "Output", -24.0f, 12.0f,    0.0f,    "dB", 1));
        addParam(mpc::floatParam ("drive",  "Drive",    0.0f, 48.0f,   18.0f,    "dB", 1));
        addParam(mpc::choiceParam("mode",   "Mode",   {"Soft", "Hard", "Fold", "Rect"}, 0));
        addParam(mpc::floatParam ("bias",   "Bias",   -50.0f, 50.0f,    0.0f,    "%",  0));
        // Cutoff 0 (below kSplitOffHz) shows "Off": same as the Split On switch off,
        // the whole signal is distorted. Skew 0.5 gives fine resolution at the low end.
        addParam(mpc::floatParam ("split",  "Split",    0.0f, 1000.0f, 100.0f,   "Hz", 0, 0.5f));
        addParam(mpc::floatParam ("sub",    "Sub",      0.0f, 100.0f, 100.0f,    "%",  0));
        addParam(mpc::logParam   ("tone",   "Tone",   500.0f, 20000.0f, 6000.0f, "Hz", 0));
        addParam(mpc::floatParam ("mix",    "Mix",      0.0f, 100.0f, 100.0f,    "%",  0));
        addParam(mpc::boolParam  ("spliton", "Split On", true));

        addParam(mpc::intParam   ("bits",   "Bits",     1, 16, 16, "bit"));
        addParam(mpc::floatParam ("decimate", "Decimate", 1.0f, 40.0f, 1.0f,     "x",  1, 0.5f));
        addParam(mpc::floatParam ("ring",   "Ring",     0.0f, 100.0f,   0.0f,    "%",  0));
        addParam(mpc::logParam   ("ringhz", "Ring Hz",  20.0f, 2000.0f, 150.0f,  "Hz", 0));
        addParam(mpc::floatParam ("octsub", "Oct Sub",  0.0f, 100.0f,   0.0f,    "%",  0));
        addParam(mpc::floatParam ("tilt",   "Pre Tilt", -12.0f, 12.0f,  0.0f,    "dB", 1));
        addParam(mpc::boolParam  ("multiband", "Multiband", false));
        addParam(mpc::logParam   ("hisplit", "Hi Split", 1000.0f, 8000.0f, 3000.0f, "Hz", 0));
        addParam(mpc::floatParam ("hidrive", "Hi Drive", -24.0f, 24.0f, 0.0f,    "dB", 1));
        addParam(mpc::choiceParam("wobpos", "Wob Pos",  {"Off", "Pre", "Post"}, 0));
        addParam(mpc::logParam   ("wobrate", "Wob Rate", 0.1f, 20.0f,   2.0f,    "Hz", 2));
        addParam(mpc::floatParam ("wobdepth", "Wob Depth", 0.0f, 100.0f, 50.0f,  "%",  0));
        addParam(mpc::logParam   ("wobfreq", "Wob Freq", 40.0f, 8000.0f, 600.0f, "Hz", 0));
        addParam(mpc::floatParam ("wobreso", "Wob Reso", 0.0f, 100.0f,  40.0f,   "%",  0));
        addParam(mpc::choiceParam("wobshape", "Wob Shape", {"Sine", "Tri", "Saw", "Square"}, 0));
        {
            std::vector<std::string> names{"Free"};
            for (const std::string& n : mpc::dsp::divisionNames()) names.push_back(n);
            addParam(mpc::choiceParam("wobsync", "Wob Sync", names, 0));
        }
        addParam(mpc::boolParam  ("crushon", "Crush On", true));
        addParam(mpc::boolParam  ("ringon",  "Ring On",  true));
        addParam(mpc::boolParam  ("wobon",   "Wobble On", true));
        {
            // The red button is a trigger, not a setting: keep it out of automation.
            mpc::Param red = mpc::boolParam("redbutton", "Red Button", false);
            red.automatable = false;
            addParam(red);
        }
        addParam(mpc::choiceParam("sanity",  "Sanity", {"Sober", "Drunkish", "Bat Shit Crazy"}, 1));

        // ---- vowel stage ----
        const std::vector<std::string> vowelNames{"OO", "OH", "UH", "AH", "AE", "EH", "EE", "IH", "AW", "ER", "UE", "OE", "YI", "W", "L"};
        addParam(mpc::boolParam  ("vowon",    "Vowel On",  false));
        addParam(mpc::choiceParam("vowpos",   "Vow Pos",   {"Off", "Pre", "Post"}, 2));
        addParam(mpc::choiceParam("vowa",     "Vowel A",   vowelNames, 0));
        addParam(mpc::choiceParam("vowb",     "Vowel B",   vowelNames, 3));
        addParam(mpc::floatParam ("vowmorph", "Morph",     0.0f, 100.0f, 0.0f,  "%",  0));
        addParam(mpc::choiceParam("vowmode",  "Vow Mode",  {"Manual", "LFO", "ADSR", "Word"}, 3));
        addParam(mpc::logParam   ("vowrate",  "Vow Rate",  0.1f, 20.0f,  2.0f,  "Hz", 2));
        {
            std::vector<std::string> names{"Hz"};   // first entry: speed from Vow Rate in Hz
            for (const std::string& n : mpc::dsp::divisionNames()) names.push_back(n);
            addParam(mpc::choiceParam("vowsync", "Vow Sync", names, 0));
        }
        addParam(mpc::floatParam ("vowdepth", "Vow Depth", 0.0f, 100.0f, 70.0f, "%",  0));
        addParam(mpc::logParam   ("vowglide", "Vow Glide", 1.0f, 300.0f, 25.0f, "ms", 0));
        addParam(mpc::choiceParam("vowword",  "Vow Word",
                                  {"WOW", "YAY", "YO", "YOY", "YEAH", "YOI", "WAH", "OOH-AAH", "EOW", "MAMA", "CUSTOM"}, 0));
        addParam(mpc::floatParam ("gender",   "Gender",    -12.0f, 12.0f, 0.0f, "st", 1));
        addParam(mpc::floatParam ("vowq",     "Vow Q",     0.0f, 100.0f, 70.0f, "%",  0));
        addParam(mpc::floatParam ("vowmix",   "Vow Mix",   0.0f, 100.0f, 100.0f, "%", 0));
        addParam(mpc::choiceParam("vowchar",  "Vow Char",  {"Normal", "Choir", "Whisper", "Robot"}, 0));
        addParam(mpc::choiceParam("vowtrig",  "Vow Trig",  {"Loop", "Hit"}, 0));
        addParam(mpc::choiceParam("vows1",    "Step 1",    vowelNames, 0));
        addParam(mpc::choiceParam("vows2",    "Step 2",    vowelNames, 3));
        addParam(mpc::choiceParam("vows3",    "Step 3",    vowelNames, 1));
        addParam(mpc::choiceParam("vows4",    "Step 4",    vowelNames, 0));
        addParam(mpc::logParam   ("vowatk",   "Attack",     1.0f, 2000.0f, 120.0f,  "ms", 0));
        addParam(mpc::logParam   ("vowdec",   "Decay",      5.0f, 3000.0f, 300.0f, "ms", 0));
        addParam(mpc::floatParam ("vowsus",   "Sustain",    0.0f, 100.0f, 70.0f,  "%",  0));
        addParam(mpc::logParam   ("vowrel",   "Release",    5.0f, 3000.0f, 200.0f, "ms", 0));
        addParam(mpc::floatParam ("vowsens",  "Sens",       0.0f, 100.0f, 50.0f,  "%",  0));
        addParam(mpc::floatParam ("vowhold",  "Hold",       0.0f, 2000.0f, 0.0f,  "ms", 0, 0.5f));
        addParam(mpc::boolParam  ("vowenv",   "Word Env",  false));
        addParam(mpc::logParam   ("vowgap",   "Gap",       30.0f, 1500.0f, 150.0f, "ms", 0));

        // ---- Built-in programs (see PS above: only what differs from the defaults) ----
        // 808 and sub bass
        addProgram2("Init",           PS());
        addProgram2("808 Warm",       PS().Out(-2).Drv(14).Bias(8).Split(60).Tone(3500).Tilt(-2));
        addProgram2("808 Crunch",     PS().Out(-6.5).Drv(26).Mode(kHard).Bias(5).Split(70).Tone(5000));
        addProgram2("808 Octave",     PS().Out(-3).Drv(18).Split(70).Sub(80).Tone(3000).Oct(70));
        addProgram2("808 Destroyed",  PS().Out(-6).Drv(32).Mode(kFold).Bias(15).SplitOff().Tone(6000).Bits(12));
        addProgram2("808 Dirty Crush",PS().Out(-5).Drv(30).Mode(kHard).Split(80).Tone(6000).Bits(8).Dec(3));
        addProgram2("Sub Saturator",  PS().Out(3).Drv(8).Split(80).Tone(2500).Mix(60));
        // mid growl and fuzz
        addProgram2("Growl Mid",      PS().Out(-2.5).Drv(40).Mode(kFold).Split(200).Sub(0).Tone(4500).Tilt(3));
        addProgram2("Reese Rot",      PS().Out(-5).Drv(34).Bias(25).Split(150).Sub(80).Tone(3000).Multi(2500, -12));
        addProgram2("Octave Fizz",    PS().Out(-3.5).Drv(40).Mode(kRect).Split(120).Tone(6000));
        addProgram2("Acid Bite",      PS().Out(-4).Drv(28).Bias(30).Split(300).Sub(40).Tone(8000).Wob(kWobPre, 900, 30, 70, 0, 0, 0.5f));
        addProgram2("Talking Bass",   PS().Out(-3.5).Drv(32).Mode(kFold).Split(90).Sub(60).Tone(7000).Wob(kWobPre, 700, 60, 80, 0, 0, 3.0f));
        // dubstep wobble (tempo synced)
        addProgram2("Wobble 1/4",     PS().Out(-6).Drv(36).Mode(kHard).Split(90).Sub(0).Tone(9000).Wob(kWobPost, 500, 70, 60, 0, kSync_1_4));
        addProgram2("Wobble 1/8 Tri", PS().Out(-2).Drv(38).Mode(kFold).Split(90).Sub(0).Tone(9000).Wob(kWobPost, 450, 75, 65, 1, kSync_1_8));
        addProgram2("Wobble Square",  PS().Out(-6).Drv(34).Mode(kHard).Split(100).Sub(20).Tone(8000).Wob(kWobPost, 400, 55, 55, 3, kSync_1_16));
        addProgram2("Triplet Wub",    PS().Out(-8.5).Drv(40).Mode(kHard).Split(80).Sub(0).Tone(9000).Bits(10).Wob(kWobPost, 520, 80, 70, 0, kSync_1_8T));
        addProgram2("Dub Digital",    PS().Out(-6).Drv(42).Mode(kHard).Split(80).Sub(0).Tone(8000).Bits(9).Dec(2));
        // metallic
        addProgram2("Robot Ring",     PS().Out(-3).Drv(28).Mode(kHard).Split(90).Sub(80).Tone(5000).Ring(70, 220));
        addProgram2("Metal Bass",     PS().Out(-1).Drv(38).Mode(kFold).Split(110).Sub(60).Tone(7000).Ring(100, 440));
        // extreme
        addProgram2("Parallel Smash", PS().Out(-3).Drv(48).Mode(kHard).Split(100).Tone(9000).Mix(45));
        addProgram2("Broken Speaker", PS().Out(-8).Drv(36).Mode(kHard).Bias(40).Split(250).Sub(70).Tone(2000).Bits(12).Tilt(-6));
        addProgram2("Bright Fold",    PS().Out(-3.5).Drv(30).Mode(kFold).Split(100).Tone(9000).Tilt(8));
        addProgram2("Chainsaw",       PS().Out(-8).Drv(40).Mode(kHard).Split(120).Sub(30).Tone(7000).Wob(kWobPre, 800, 80, 85, 2, 0, 8.0f));
        addProgram2("Lo-Fi Grit",     PS().Out(-2).Drv(20).Mode(kRect).Bias(10).Split(90).Tone(1500).Mix(70).Bits(8).Dec(6));
        addProgram2("Wall Of Noise",  PS().Out(-3).Drv(36).Mode(kFold).Bias(10).Split(90).Sub(100).Tone(7000).Bits(8).Dec(3).Ring(25, 330));
        // --- vowels: Vow(pos 2 = Post, mode 3 = Step, word, sync) / VowAB / Voice ---
        // sync choice indices: 1/16 = 5, 1/8T = 6, 1/8 = 8, 1/4 = 11, 1/2 = 14
        addProgram2("Wow Bass",       PS().Out(-5.5).Drv(30).Mode(kHard).Split(80).Sub(60).Tone(9000).Vow(2, 3, 0, 11));
        addProgram2("Yay Growl",      PS().Out(-11).Drv(34).Mode(kFold).SplitOff().Tone(9000).Vow(2, 3, 1, 11));
        addProgram2("Yo-Yo",          PS().Out(-7).Drv(32).Mode(kHard).Split(80).Sub(0).Tone(9000).Vow(2, 3, 2, 8));
        addProgram2("Yeah Bass",      PS().Out(-3).Drv(28).Split(90).Sub(80).Tone(8000).Vow(2, 3, 4, 14).Voice(0, 70, 100, 0, 60));
        addProgram2("Yoi Yoi",        PS().Out(-7).Drv(40).Mode(kHard).Split(80).Sub(0).Tone(9000).Vow(2, 3, 5, 8).Voice(0, 75, 100, 0, 15));
        addProgram2("Talking LFO",    PS().Out(-4).Drv(30).Mode(kHard).Split(80).Sub(40).Tone(9000).Vow(2, 1, 0, 11).VowAB(0, 3, 50, 100));
        addProgram2("Hit Opens",     PS().Out(-7).Drv(30).Mode(kHard).Split(90).Sub(30).Tone(9000).Vow(2, 2, 0, 0).VowAB(0, 6, 0, 100).Adsr(90, 300, 60, 250).Voice(0, 70, 100, 0, 10));
        addProgram2("Choir Bass",     PS().Out(5).Drv(22).Split(90).Sub(60).Tone(7000).Vow(2, 1, 0, 14).VowAB(1, 5, 50, 70).Voice(0, 55, 80, 1, 60));
        addProgram2("Whisper Wub",    PS().Out(-4).Drv(30).Mode(kHard).Split(80).Sub(0).Tone(9000).Vow(2, 3, 9, 11).Voice(0, 60, 100, 2, 30));
        addProgram2("Chipmunk",       PS().Out(0).Drv(26).Mode(kHard).Split(80).Sub(40).Tone(10000).Vow(2, 3, 1, 8).Voice(9, 60, 100, 0, 15));
        addProgram2("Giant Talk",     PS().Out(-8).Drv(30).Split(80).Sub(60).Tone(7000).Vow(2, 3, 6, 14).Voice(-9, 60, 100, 0, 60));
        addProgram2("Robot Vowel",    PS().Out(-5).Drv(28).Mode(kHard).Split(80).Sub(50).Tone(8000).Vow(2, 3, 7, 11).Voice(0, 60, 100, 3, 5));
        addProgram2("Alien Eow",      PS().Out(-10).Drv(34).Mode(kFold).Split(80).Sub(30).Tone(9000).Vow(2, 3, 8, 11).Voice(5, 80, 100, 0, 40));
        addProgram2("Pre Growl",      PS().Out(-6).Drv(36).Mode(kHard).Split(80).Sub(40).Tone(8000).Vow(1, 1, 0, 11).VowAB(0, 3, 50, 100));
        // one word per hit (Vow Trig = Hit): the word length is the Vow Sync division
        addProgram2("YOY Hit",        PS().Out(-6).Drv(34).Mode(kHard).Split(80).Sub(30).Tone(9000).Vow(2, 3, 3, 8).Hit().Voice(0, 75, 100, 0, 20));
        addProgram2("YOI Hit",        PS().Out(-6).Drv(34).Mode(kHard).Split(80).Sub(30).Tone(9000).Vow(2, 3, 5, 8).Hit().WordEnv().Adsr(8, 250, 60, 200).Voice(0, 75, 100, 0, 20));

        // --- more vocal presets. Vow(pos 2 = Post, mode 0 Manual / 1 LFO / 2 ADSR / 3 Word, word, sync); sync: 1/16 = 5,
        //     1/8T = 6, 1/8 = 8, 1/4T = 9, 1/4 = 11, 1/2 = 14, 1/1 = 16. Vowels: OO0 OH1 UH2 AH3 AE4 EH5 EE6 IH7 AW8 ER9 UE10 OE11 YI12 W13 L14.
        // one-shot words, one per hit
        addProgram2("Yeah Hit",       PS().Out(-3).Drv(32).Mode(kHard).Split(80).Sub(40).Tone(9000).Vow(2, 3, 4, 8).Hit().WordEnv().Adsr(10, 300, 70, 250).Voice(0, 75, 100, 0, 20));
        addProgram2("Laser Yoi",      PS().Out(-8.5).Drv(36).Mode(kHard).Split(80).Sub(30).Tone(10000).Vow(2, 3, 10, 5).Custom(6, 9, 1, 6).Hit().Voice(0, 80, 100, 0, 8));
        addProgram2("Hit And Hold",   PS().Out(-0.5).Drv(30).Split(80).Sub(50).Tone(8000).Vow(2, 3, 8, 16).Hit().Voice(0, 70, 100, 0, 60));
        addProgram2("Chipmunk Hit",   PS().Out(-0.5).Drv(26).Mode(kHard).Split(80).Sub(30).Tone(10000).Vow(2, 3, 1, 8).Hit().Voice(10, 65, 100, 0, 15));
        addProgram2("Robot Hit",      PS().Out(-8.5).Drv(28).Mode(kHard).Split(80).Sub(40).Tone(8000).Vow(2, 3, 3, 8).Hit().Voice(0, 70, 100, 3, 5));
        // looping words
        addProgram2("Dr Wub",         PS().Out(-11).Drv(38).Mode(kFold).Split(70).Sub(20).Tone(9000).Vow(2, 3, 10, 8).Custom(9, 6, 9, 0).Voice(0, 75, 100, 0, 20));
        addProgram2("Vowel Run",      PS().Out(-5).Drv(28).Split(80).Sub(50).Tone(8000).Vow(2, 3, 10, 11).Custom(13, 0, 3, 6).Voice(0, 65, 100, 0, 40));
        addProgram2("Whisper Talk",   PS().Out(-6).Drv(30).Mode(kHard).Split(80).Sub(0).Tone(9000).Vow(2, 3, 0, 11).Voice(0, 55, 100, 2, 40));
        addProgram2("Giant Yeah",     PS().Out(-7.5).Drv(30).Split(70).Sub(60).Tone(6000).Vow(2, 3, 4, 14).Voice(-10, 60, 100, 0, 80));
        addProgram2("Pre Talk",       PS().Out(-5).Drv(34).Mode(kHard).Split(80).Sub(40).Tone(8000).Vow(1, 3, 6, 8).Voice(0, 70, 100, 0, 30));
        // LFO sweeps between two vowels
        addProgram2("Wah Pedal",      PS().Out(-7).Drv(30).Mode(kHard).Split(80).Sub(40).Tone(9000).Vow(2, 1, 0, 9).VowAB(13, 3, 50, 100).Voice(0, 60, 100, 0, 40));
        addProgram2("Growl ER",       PS().Out(-11.5).Drv(38).Mode(kFold).Split(80).Sub(20).Tone(9000).Vow(2, 1, 0, 8).VowAB(9, 6, 50, 100).Voice(0, 70, 100, 0, 20));
        addProgram2("U-Umlaut Wobble",PS().Out(-4.5).Drv(34).Mode(kHard).Split(80).Sub(30).Tone(9000).Vow(2, 1, 0, 6).VowAB(10, 11, 50, 100).Voice(0, 70, 100, 0, 30));
        addProgram2("Russian Yi",     PS().Out(-3).Drv(30).Split(80).Sub(50).Tone(8000).Vow(2, 1, 0, 14).VowAB(12, 3, 50, 80).Voice(0, 65, 100, 0, 50));
        addProgram2("Alien Choir",    PS().Out(-1).Drv(24).Split(90).Sub(50).Tone(7000).Vow(2, 1, 0, 14).VowAB(10, 13, 50, 90).Voice(4, 55, 85, 1, 70));
        addProgram2("Choir Ahh",      PS().Out(2.5).Drv(22).Split(90).Sub(60).Tone(6000).Vow(2, 0, 0, 0).VowAB(3, 3, 0, 0).Voice(-3, 50, 75, 1, 40));
        // ADSR: each hit opens (or closes) the mouth
        addProgram2("Vowel Bounce",   PS().Out(-7).Drv(30).Mode(kHard).Split(80).Sub(40).Tone(9000).Vow(2, 2, 0, 0).VowAB(0, 6, 0, 100).Adsr(40, 250, 20, 200).Voice(0, 70, 100, 0, 10));
        addProgram2("Chomp",          PS().Out(-6).Drv(32).Mode(kHard).Split(80).Sub(40).Tone(9000).Vow(2, 2, 0, 0).VowAB(0, 3, 0, 100).Adsr(15, 180, 0, 100).Trigger(55, 180).Voice(0, 70, 100, 0, 8));
        addProgram2("Mouth Pump",     PS().Out(-7.5).Drv(34).Mode(kHard).Split(80).Sub(30).Tone(9000).Vow(2, 2, 0, 0).VowAB(13, 3, 0, 100).Adsr(5, 120, 0, 80).Trigger(70, 0).Gap(220).Voice(0, 75, 100, 0, 6));
        addProgram2("Closing Door",   PS().Out(-4.5).Drv(28).Split(80).Sub(50).Tone(8000).Vow(2, 2, 0, 0).VowAB(6, 0, 0, 100).Adsr(250, 400, 50, 400).Voice(0, 65, 100, 0, 40));
        addProgram2("Wow Hit",        PS().Out(-5).Drv(30).Mode(kHard).Split(80).Sub(50).Tone(9000).Vow(2, 3, 0, 11).Hit().Gap(250).Voice(0, 70, 100, 0, 20));
        addProgram2("Yo Hit",         PS().Out(-4).Drv(32).Split(80).Sub(40).Tone(9000).Vow(2, 3, 2, 8).Hit().Voice(0, 75, 100, 0, 20));

        // --- found by the Red Button (Bat Shit Crazy) and kept; values copied from the user's saved presets ---
        {
            PS p;
            p.out = -10.4f; p.drv = 28.6f; p.mode = kFold; p.bias = 2.6f; p.split = 40.5f; p.sub = 21.4f; p.tone = 3661.0f; p.mix = 79.8f;
            p.ring = 43.8f; p.ringHz = 334.8f; p.tilt = 0.8f; p.hiSplit = 1393.7f; p.hiDrv = 9.1f;
            p.wobPos = kWobPost; p.wobRate = 0.586f; p.wobDepth = 83.7f; p.wobFreq = 1364.7f; p.wobReso = 77.2f; p.wobShape = 0; p.wobSync = 11;
            p.crushOn = 0;   // Bits and Decimate are inactive in this one
            addProgram2("Moving Harmonics", p);
        }
        {
            PS p;
            p.out = -10.2f; p.drv = 26.9f; p.mode = kSoft; p.bias = 24.6f; p.split = 34.5f; p.sub = 25.8f; p.tone = 2955.0f; p.mix = 96.6f;
            p.bits = 2; p.dec = 2.99f; p.ring = 14.6f; p.ringHz = 662.6f; p.oct = 77.5f; p.tilt = 1.8f; p.hiSplit = 2216.9f; p.hiDrv = 8.3f;
            p.wobPos = kWobPre; p.wobRate = 0.108f; p.wobDepth = 73.9f; p.wobFreq = 168.4f; p.wobReso = 57.2f; p.wobShape = 1; p.wobSync = 16;
            addProgram2("Total Filth", p);
        }

        // --- two more Red Button finds (Bat Shit Crazy), values copied from the user's saved presets ---
        {
            PS p;
            p.out = -7.784f; p.drv = 43.75f; p.mode = 2; p.bias = -29.67f;
            p.split = 20.46f; p.sub = 84.5f; p.tone = 2574.0f; p.mix = 89.98f;
            p.splitOn = 0; p.bits = 1; p.dec = 1; p.ring = 0;
            p.ringHz = 456.4f; p.oct = 74.7f; p.tilt = -3.648f; p.multi = 1;
            p.hiSplit = 1810.0f; p.hiDrv = 6.403f; p.wobPos = 1; p.wobRate = 1.741f;
            p.wobDepth = 26.41f; p.wobFreq = 2253.0f; p.wobReso = 93.04f; p.wobShape = 3;
            p.wobSync = 8; p.crushOn = 0; p.ringOn = 0; p.wobOn = 1;
            p.vowOn = 1; p.vowPos = 2; p.vowA = 3; p.vowB = 9;
            p.vowMorph = 55.18f; p.vowMode = 3; p.vowRate = 0.5172f; p.vowSync = 12;
            p.vowDepth = 72.48f; p.vowGlide = 52.57f; p.vowWord = 1; p.gender = -0.4406f;
            p.vowQ = 50.33f; p.vowMix = 88.8f; p.vowChar = 2; p.vowTrig = 0;
            p.s1 = 5; p.s2 = 8; p.s3 = 13; p.s4 = 0;
            p.vowAtk = 190.2f; p.vowDec = 46.85f; p.vowSus = 38.9f; p.vowRel = 267.8f;
            p.vowSens = 66.93f; p.vowHold = 643.1f; p.vowEnv = 1;
            addProgram2("Dancing Dervish", p);
        }
        {
            PS p;
            p.out = -8.208f; p.drv = 44.22f; p.mode = 0; p.bias = -33.78f;
            p.split = 227.4f; p.sub = 82.82f; p.tone = 2593.0f; p.mix = 71.03f;
            p.splitOn = 1; p.bits = 16; p.dec = 8.722f; p.ring = 28.28f;
            p.ringHz = 136.1f; p.oct = 71.88f; p.tilt = -11.69f; p.multi = 0;
            p.hiSplit = 2034.0f; p.hiDrv = 15.37f; p.wobPos = 2; p.wobRate = 0.1266f;
            p.wobDepth = 47.69f; p.wobFreq = 1464.0f; p.wobReso = 27.82f; p.wobShape = 1;
            p.wobSync = 5; p.crushOn = 1; p.ringOn = 1; p.wobOn = 1;
            p.vowOn = 1; p.vowPos = 1; p.vowA = 9; p.vowB = 10;
            p.vowMorph = 25.33f; p.vowMode = 3; p.vowRate = 0.4741f; p.vowSync = 15;
            p.vowDepth = 7.967f; p.vowGlide = 100.9f; p.vowWord = 7; p.gender = -7.343f;
            p.vowQ = 80.44f; p.vowMix = 83.12f; p.vowChar = 1; p.vowTrig = 0;
            p.s1 = 13; p.s2 = 0; p.s3 = 13; p.s4 = 7;
            p.vowAtk = 25.46f; p.vowDec = 145.9f; p.vowSus = 15.05f; p.vowRel = 625.0f;
            p.vowSens = 47.33f; p.vowHold = 120.4f; p.vowEnv = 0;
            addProgram2("Resonant Wobble", p);
        }

        // --- more Red Button finds (Bat Shit Crazy), values copied from the user's saved presets ---
        {
            PS p;
            p.out = -8.956f; p.drv = 25.73f; p.mode = 1; p.bias = -6.383f;
            p.split = 20.55f; p.sub = 17.39f; p.tone = 882.9f; p.mix = 98.96f;
            p.splitOn = 0; p.bits = 1; p.dec = 6.069f; p.ring = 18.92f;
            p.ringHz = 209.2f; p.oct = 18.44f; p.tilt = 1.808f; p.multi = 1;
            p.hiSplit = 2542.0f; p.hiDrv = -0.9842f; p.wobPos = 1; p.wobRate = 4.528f;
            p.wobDepth = 67.97f; p.wobFreq = 672.4f; p.wobReso = 61.4f; p.wobShape = 0;
            p.wobSync = 16; p.crushOn = 1; p.ringOn = 1; p.wobOn = 1;
            p.vowOn = 1; p.vowPos = 2; p.vowA = 8; p.vowB = 2;
            p.vowMorph = 10.47f; p.vowMode = 3; p.vowRate = 0.6745f; p.vowSync = 1;
            p.vowDepth = 96.28f; p.vowGlide = 2.826f; p.vowWord = 5; p.gender = -3.917f;
            p.vowQ = 43.3f; p.vowMix = 51.03f; p.vowChar = 2; p.vowTrig = 0;
            p.s1 = 8; p.s2 = 12; p.s3 = 10; p.s4 = 13;
            p.vowAtk = 9.806f; p.vowDec = 964.1f; p.vowSus = 13.55f; p.vowRel = 527.9f;
            p.vowSens = 61.67f; p.vowHold = 0; p.vowEnv = 0;
            addProgram2("Doomsday Radio", p);
        }
        {
            PS p;
            p.out = -8.237f; p.drv = 27.55f; p.mode = 1; p.bias = -49.0f;
            p.split = 49.25f; p.sub = 23.98f; p.tone = 1323.0f; p.mix = 71.34f;
            p.splitOn = 0; p.bits = 9; p.dec = 1; p.ring = 23.48f;
            p.ringHz = 31.26f; p.oct = 17.36f; p.tilt = 0.3423f; p.multi = 0;
            p.hiSplit = 1424.0f; p.hiDrv = 3.218f; p.wobPos = 1; p.wobRate = 0.631f;
            p.wobDepth = 92.93f; p.wobFreq = 697.1f; p.wobReso = 91.04f; p.wobShape = 3;
            p.wobSync = 4; p.crushOn = 1; p.ringOn = 1; p.wobOn = 1;
            p.vowOn = 1; p.vowPos = 1; p.vowA = 12; p.vowB = 2;
            p.vowMorph = 81.74f; p.vowMode = 3; p.vowRate = 2.871f; p.vowSync = 5;
            p.vowDepth = 46.26f; p.vowGlide = 20.92f; p.vowWord = 7; p.gender = 9.73f;
            p.vowQ = 7.699f; p.vowMix = 59.37f; p.vowChar = 2; p.vowTrig = 1;
            p.s1 = 7; p.s2 = 2; p.s3 = 2; p.s4 = 10;
            p.vowAtk = 5.264f; p.vowDec = 85.68f; p.vowSus = 67.42f; p.vowRel = 1301.0f;
            p.vowSens = 50.41f; p.vowHold = 0; p.vowEnv = 0;
            addProgram2("Enough", p);
        }
        {
            PS p;
            p.out = -9.611f; p.drv = 41.75f; p.mode = 1; p.bias = -0.06546f;
            p.split = 453.9f; p.sub = 86.05f; p.tone = 1307.0f; p.mix = 98.01f;
            p.splitOn = 0; p.bits = 12; p.dec = 1; p.ring = 17.35f;
            p.ringHz = 196.5f; p.oct = 56.89f; p.tilt = -5.447f; p.multi = 1;
            p.hiSplit = 1340.0f; p.hiDrv = 9.322f; p.wobPos = 2; p.wobRate = 5.069f;
            p.wobDepth = 82.24f; p.wobFreq = 69.24f; p.wobReso = 91.13f; p.wobShape = 0;
            p.wobSync = 7; p.crushOn = 1; p.ringOn = 1; p.wobOn = 1;
            p.vowOn = 1; p.vowPos = 2; p.vowA = 4; p.vowB = 3;
            p.vowMorph = 48.63f; p.vowMode = 2; p.vowRate = 0.7667f; p.vowSync = 2;
            p.vowDepth = 30.83f; p.vowGlide = 37.2f; p.vowWord = 4; p.gender = -0.2601f;
            p.vowQ = 89.53f; p.vowMix = 89.43f; p.vowChar = 2; p.vowTrig = 0;
            p.s1 = 4; p.s2 = 11; p.s3 = 4; p.s4 = 0;
            p.vowAtk = 43.29f; p.vowDec = 962.1f; p.vowSus = 12.81f; p.vowRel = 1107.0f;
            p.vowSens = 71.74f; p.vowHold = 113.6f; p.vowEnv = 0;
            addProgram2("RLY", p);
        }
    }

    void prepare(double sampleRate, int maxBlockSize) override
    {
        (void)maxBlockSize;
        sampleRate_ = sampleRate;
        for (Channel& c : ch_)
        {
            c.dc.prepare(sampleRate, 10.0f);
            c.tilt.setCutoff(800.0f, sampleRate);
            c.octDetA.setCutoff(150.0f, sampleRate); c.octDetB.setCutoff(150.0f, sampleRate);
            c.octOutA.setCutoff(90.0f, sampleRate);  c.octOutB.setCutoff(90.0f, sampleRate);
            c.wob.setSampleRate(sampleRate);
            for (mpc::dsp::SVF& f : c.vow) f.setSampleRate(sampleRate);
        }
        envAtk_ = 1.0f - std::exp(-1.0f / (0.005f * (float)sampleRate));
        envRel_ = 1.0f - std::exp(-1.0f / (0.12f * (float)sampleRate));
        peakDecay_ = std::exp(-1.0f / (4.0f * (float)sampleRate));   // the peak reference forgets over about 4 s
        slowCoef_ = 1.0f - std::exp(-1.0f / (0.15f * (float)sampleRate));
        fastCoef_ = 1.0f - std::exp(-1.0f / (0.008f * (float)sampleRate));   // slow level, the reference for hit detection
        hitCooldownSamples_ = (int)(0.08f * (float)sampleRate);
        hitRetriggerSamples_ = (int)(0.35f * (float)sampleRate);   // a steady tone may re-trigger after this long
        lfoV_.setSampleRate(sampleRate);
        octDecay_ = std::exp(-1.0f / (0.05f * (float)sampleRate));
        lfo_.setSampleRate(sampleRate);
        drive_.setTime(15.0f, sampleRate);
        bias_.setTime(15.0f, sampleRate);
        sub_.setTime(15.0f, sampleRate);
        out_.setTime(15.0f, sampleRate);
        mix_.setTime(15.0f, sampleRate);
        oct_.setTime(15.0f, sampleRate);
        reset();
    }

    void reset() override
    {
        for (Channel& c : ch_) c.reset();
        ringPhase_ = 0.0f;
        wobCount_ = 0;
        lfo_.reset(0.0f);
        lfoV_.reset(0.0f);
        vEnv_ = 0.0f; vFast_ = 0.0f; vSlow_ = 0.0f; vPeak_ = 0.0f; vCount_ = 0; wordPhase_ = 1.0f; wordPlaying_ = false; hitCooldown_ = 0; hitArmed_ = true; hitPeak_ = 0.0f; sinceHit_ = 0; adsr_ = 0.0f; notePeak_ = 0.0f; adsrStage_ = 0; vSnap_ = true;
        drive_.snap(mpc::dsp::dbToGain(get(kDrive)));
        bias_.snap(get(kBias) * 0.01f);
        sub_.snap(get(kSub) * 0.01f);
        out_.snap(mpc::dsp::dbToGain(get(kOutput)));
        mix_.snap(get(kMix) * 0.01f);
        oct_.snap(get(kOctSub) * 0.01f);
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int numFrames   = ctx.numFrames;
        const int numChannels = mpc::clamp(ctx.numOutputs, 0, kMaxChannels);
        const float nyq = 0.45f * (float)sampleRate_;

        if (redResetPending_.exchange(false)) set(kRedButton, 0.0f);   // release the Red Button

        // ---- per-block work: coefficients and targets ----
        const float splitRaw = get(kSplit);
        const bool  splitOff = !getBool(kSplitOn) || splitRaw < kSplitOffHz;
        const float split = mpc::clamp(splitRaw, kSplitOffHz, nyq);
        const float tone  = mpc::clamp(get(kTone), 200.0f, nyq);
        const int   mode  = getInt(kMode);

        const bool  multi   = getBool(kMultiband);
        const float hiSplit = mpc::clamp(get(kHiSplit), 200.0f, nyq);
        const float hiGain  = mpc::dsp::dbToGain(get(kHiDrive));

        const float tiltDb = get(kTilt);
        const float tiltLo = tiltDb < 0.0f ? mpc::dsp::dbToGain(-tiltDb) : 1.0f;
        const float tiltHi = tiltDb > 0.0f ? mpc::dsp::dbToGain(tiltDb)  : 1.0f;

        const int   bits     = getInt(kBits);
        const bool  crushOn  = getBool(kCrushOn);
        const bool  crushBit = crushOn && bits < 16;
        const float levels   = std::exp2((float)bits - 1.0f);
        const float decRatio = get(kDecimate);
        const bool  crushDec = crushOn && decRatio > 1.05f;

        const float ringAmt = getBool(kRingOn) ? get(kRing) * 0.01f : 0.0f;
        const float ringInc = mpc::clamp(get(kRingHz), 1.0f, nyq) / (float)sampleRate_;

        const bool  octOn = get(kOctSub) > 0.1f;

        const int   wobPos = getBool(kWobOn) ? getInt(kWobPos) : (int)kWobOff;   // Wobble On off = bypass
        const bool  wobOn  = wobPos != kWobOff;
        const float wobDepth = get(kWobDepth) * 0.01f * 4.0f;       // octaves
        const float wobFreq  = get(kWobFreq);
        const float wobReso  = get(kWobReso) * 0.0095f;             // 0..0.95
        const int   wobSync  = getInt(kWobSync);
        lfo_.setShape(getInt(kWobShape));
        if (wobSync > 0)
        {
            lfo_.setTempoSync(ctx.transport.bpm, mpc::dsp::divisionBeats(wobSync - 1));
            if (ctx.transport.playing && ctx.transport.ppqValid) lfo_.syncToPpq(ctx.transport.ppqPos);
        }
        else lfo_.setFrequency(get(kWobRate));

        // ---- vowel stage: per-block settings ----
        const int   vowPos     = getBool(kVowOn) ? getInt(kVowPos) : 0;    // Vowel On off = bypass
        const bool  vowActive  = vowPos != 0;
        const int   vowModeSel = getInt(kVowMode);
        const int   vowA       = mpc::clamp(getInt(kVowA), 0, kNumVowels - 1);
        const int   vowB       = mpc::clamp(getInt(kVowB), 0, kNumVowels - 1);
        const float vowMorph   = get(kVowMorph) * 0.01f;
        const float vowDepth   = get(kVowDepth) * 0.01f;
        const float vowMix     = get(kVowMix) * 0.01f;
        const int   vowChar    = getInt(kVowChar);   // 0 Normal, 1 Choir, 2 Whisper, 3 Robot
        const int   vowWord    = mpc::clamp(getInt(kVowWord), 0, kNumWords - 1);
        const float genderMul  = std::exp2(get(kGender) / 12.0f);
        const float qNorm      = std::min(1.0f, get(kVowQ) * 0.01f + (vowChar == 3 ? 0.25f : 0.0f));
        const float vowRes     = 0.80f + 0.18f * qNorm;                  // SVF resonance (Q about 2.4 .. 17)
        const float vowK       = 2.0f - 1.98f * vowRes;                  // 1/Q: scales a bandpass to unity peak
        const int   nSingers   = vowChar == 1 ? 3 : 1;
        const float singerGain = nSingers == 3 ? 0.6f : 1.0f;
        const float glideCoef  = 1.0f - std::exp(-(float)kWobUpdate / (std::max(1.0f, get(kVowGlide)) * 0.001f * (float)sampleRate_));
        const int   vowTrig    = getInt(kVowTrig);   // 0 Loop, 1 Hit
        const bool  wordEnvOn  = getBool(kVowEnv) && vowModeSel == kVMWord && vowTrig == 1;   // ADSR as the amount of the effect
        int wordKeys[4];
        for (int k = 0; k < 4; ++k)
            wordKeys[k] = vowWord == kCustomWord ? mpc::clamp(getInt(kVowS1 + k), 0, kNumVowels - 1) : kWords[vowWord][k];
        // One cycle (an LFO period, or the length of one word) lasts Vow Sync beats, or 1 / Vow Rate seconds
        // when Vow Sync is "Hz". Vow Rate has no effect while a note division is chosen.
        const int   vowSync    = getInt(kVowSync);
        float cycleSamples;
        if (vowSync > 0)
        {
            const float beats = mpc::dsp::divisionBeats(vowSync - 1);
            cycleSamples = mpc::dsp::beatsToSamples(beats, ctx.transport.bpm, sampleRate_);
            lfoV_.setTempoSync(ctx.transport.bpm, beats);
            if (vowActive && ctx.transport.playing && ctx.transport.ppqValid)
            {
                if (vowModeSel == kVMWord && vowTrig == 0)
                {
                    const double pos = ctx.transport.ppqPos / (double)beats;
                    wordPhase_ = (float)(pos - std::floor(pos));   // loop words stay locked to the song position
                }
                lfoV_.syncToPpq(ctx.transport.ppqPos);
            }
        }
        else
        {
            const float rate = get(kVowRate);
            cycleSamples = (float)sampleRate_ / rate;
            lfoV_.setFrequency(rate);
        }
        cycleSamples = std::max(cycleSamples, 64.0f);
        const float wordInc = 1.0f / cycleSamples;
        const float adsrAtkInc  = 1.0f / std::max(1.0f, get(kVowAtk) * 0.001f * (float)sampleRate_);
        const float adsrDecCoef = 1.0f - std::exp(-1.0f / std::max(1.0f, get(kVowDec) * 0.001f * (float)sampleRate_));
        const float adsrRelCoef = 1.0f - std::exp(-1.0f / std::max(1.0f, get(kVowRel) * 0.001f * (float)sampleRate_));
        const float adsrSus     = get(kVowSus) * 0.01f;
        // Hit detector: Sens 50% is the old fixed setting (fast level 1.8 x the slow level); higher = more sensitive.
        const float sens01      = get(kVowSens) * 0.01f;
        const float hitRatio    = 2.6f - 1.3f * sens01;
        const float hitFloor    = 0.04f - 0.036f * sens01;
        const float hitRearm    = 1.0f + 0.4f * (hitRatio - 1.0f);
        const int   gapSamples  = (int)(get(kVowGap) * 0.001f * (float)sampleRate_);   // no new hit for this long after a hit
        const int   holdSamples = (int)(get(kVowHold) * 0.001f * (float)sampleRate_);   // 0 = follow the level
        lfoV_.setShape(0);   // sine
        if (vowActive && !vowWasActive_) vSnap_ = true;
        vowWasActive_ = vowActive;

        for (Channel& c : ch_)
        {
            c.lowA.setCutoff(split, sampleRate_);  c.lowB.setCutoff(split, sampleRate_);
            c.midA.setCutoff(hiSplit, sampleRate_); c.midB.setCutoff(hiSplit, sampleRate_);
            c.toneA.setCutoff(tone, sampleRate_);  c.toneB.setCutoff(tone, sampleRate_);
        }
        drive_.setTarget(mpc::dsp::dbToGain(get(kDrive)));
        bias_.setTarget(get(kBias) * 0.01f);
        sub_.setTarget(get(kSub) * 0.01f);
        out_.setTarget(mpc::dsp::dbToGain(get(kOutput)));
        mix_.setTarget(get(kMix) * 0.01f);
        oct_.setTarget(get(kOctSub) * 0.01f);

        for (int i = 0; i < numFrames; ++i)
        {
            const float drive = drive_.process();
            const float bias  = bias_.process();
            const float sub   = sub_.process();
            const float out   = out_.process();
            const float mix   = mix_.process();
            const float oct   = oct_.process();

            // Bias shifts the waveform before shaping; the DC blocker removes the offset after.
            const float biasOffset = bias * 2.0f;
            // What the shaper outputs for a silent input (Rect gives -1, any mode gives a DC
            // value once Bias is set). Subtracting it keeps silence silent and stops Mode / Bias
            // changes from kicking the DC blocker into a thump.
            const float shapeAtZero = shape(mode, biasOffset);

            // Wobble LFO runs every sample; filter coefficients refresh every kWobUpdate samples.
            if (wobOn)
            {
                const float l = lfo_.process();
                if ((wobCount_++ % kWobUpdate) == 0)
                {
                    const float cutoff = mpc::clamp(wobFreq * std::exp2(wobDepth * l), 20.0f, nyq);
                    for (Channel& c : ch_) c.wob.set(cutoff, wobReso);
                }
            }

            // Vowel control (shared by both channels): level followers, hit detector, word clock and,
            // every kWobUpdate samples, the smoothed formant targets and filter coefficients.
            float vowNoise = 0.0f;
            if (vowActive)
            {
                float a = 0.0f;
                for (int c = 0; c < numChannels; ++c)
                {
                    const float v = (ctx.inputs != nullptr && c < ctx.numInputs && ctx.inputs[c] != nullptr) ? ctx.inputs[c][i] : 0.0f;
                    a += std::fabs(v);
                }
                a *= numChannels > 0 ? 1.0f / (float)numChannels : 0.0f;
                vEnv_  += (a > vEnv_ ? envAtk_ : envRel_) * (a - vEnv_);
                vSlow_ += slowCoef_ * (a - vSlow_);
                vFast_ += fastCoef_ * (a - vFast_);   // same kind of smoothing as vSlow_, only faster: their ratio is 1 on a steady tone
                vPeak_  = std::max(vEnv_, vPeak_ * peakDecay_);

                // Hit detector (used by Word / Hit and by ADSR): a sudden rise of the fast level over the slow one.
                // It re-arms only once the fast level has settled back near the slow one, so a sustained note
                // triggers once, not repeatedly while the slow level catches up.
                bool onset = false;
                if (vowModeSel == kVMAdsr || (vowModeSel == kVMWord && vowTrig == 1))
                {
                    if (hitCooldown_ > 0) --hitCooldown_;
                    // After a hit the detector re-arms only when the level has settled near the slow level AND either
                    // fallen well below that hit's own peak (a bump inside a decaying note is not a new hit) or a
                    // minimum time has passed (so a steady tone that gets louder can still trigger).
                    if (!hitArmed_)
                    {
                        hitPeak_ = std::max(hitPeak_, vFast_);
                        ++sinceHit_;
                        const bool settled = vFast_ < hitRearm * vSlow_ + 0.003f;
                        if (settled && (vFast_ < 0.6f * hitPeak_ || sinceHit_ > hitRetriggerSamples_)) hitArmed_ = true;
                    }
                    if (hitArmed_ && hitCooldown_ == 0 && vFast_ > hitFloor && vFast_ > hitRatio * vSlow_ + 0.005f)
                    { onset = true; hitCooldown_ = gapSamples; hitArmed_ = false; hitPeak_ = vFast_; sinceHit_ = 0; }
                }

                if (vowModeSel == kVMWord)
                {
                    if (vowTrig == 1)
                    {
                        if (onset) { wordPhase_ = 0.0f; wordPlaying_ = true; }
                        if (wordPlaying_)
                        {
                            wordPhase_ += wordInc;
                            if (wordPhase_ >= 1.0f) { wordPhase_ = 1.0f; wordPlaying_ = false; }   // hold the last vowel
                        }
                    }
                    else
                    {
                        wordPhase_ += wordInc;
                        if (wordPhase_ >= 1.0f) wordPhase_ -= 1.0f;
                    }
                }
                if (vowModeSel == kVMAdsr || wordEnvOn)
                {
                    // The note is "held" from the hit until its level has fallen about 18 dB below its own peak.
                    if (onset) { adsrStage_ = 1; notePeak_ = vEnv_; holdCount_ = holdSamples; }
                    if (adsrStage_ != 0 && adsrStage_ != 4)
                    {
                        notePeak_ = std::max(notePeak_, vEnv_);
                        if (holdSamples > 0)
                        {
                            if (--holdCount_ <= 0) adsrStage_ = 4;   // Hold: the note lasts a fixed time
                        }
                        else if (vEnv_ < 0.125f * notePeak_ || vEnv_ < 0.004f) adsrStage_ = 4;   // gate off: release
                    }
                    switch (adsrStage_)
                    {
                        case 1: adsr_ += adsrAtkInc; if (adsr_ >= 1.0f) { adsr_ = 1.0f; adsrStage_ = 2; } break;
                        case 2: adsr_ += (adsrSus - adsr_) * adsrDecCoef; if (std::fabs(adsr_ - adsrSus) < 0.002f) adsrStage_ = 3; break;
                        case 3: adsr_ = adsrSus; break;
                        case 4: adsr_ -= adsr_ * adsrRelCoef; if (adsr_ < 0.002f) { adsr_ = 0.0f; adsrStage_ = 0; } break;
                        default: adsr_ = 0.0f; break;
                    }
                }
                const float lv = vowModeSel == kVMLfo ? lfoV_.process() : 0.0f;
                vNoiseState_ ^= vNoiseState_ << 13; vNoiseState_ ^= vNoiseState_ >> 17; vNoiseState_ ^= vNoiseState_ << 5;
                vowNoise = (float)(vNoiseState_ >> 8) * (2.0f / 16777216.0f) - 1.0f;

                if ((vCount_++ % kWobUpdate) == 0)
                {
                    float t[3];
                    if (vowModeSel == kVMWord)
                    {
                        // Glide through the four key vowels with eased segments, so a word is one smooth contour.
                        const float pw = mpc::clamp(wordPhase_, 0.0f, 1.0f) * 3.0f;
                        const int   sg = std::min((int)pw, 2);
                        float u = pw - (float)sg;
                        u = u * u * (3.0f - 2.0f * u);
                        const Formant& fa = kVowels[wordKeys[sg]];
                        const Formant& fb = kVowels[wordKeys[sg + 1]];
                        t[0] = fa.f1 * std::pow(fb.f1 / fa.f1, u);
                        t[1] = fa.f2 * std::pow(fb.f2 / fa.f2, u);
                        t[2] = fa.f3 * std::pow(fb.f3 / fa.f3, u);
                    }
                    else
                    {
                        float m = vowMorph;
                        if (vowModeSel == kVMLfo)      m += vowDepth * lv;
                        else if (vowModeSel == kVMAdsr) m += vowDepth * adsr_;   // each hit opens A toward B, then it closes
                        m = mpc::clamp(m, 0.0f, 1.0f);
                        if (vowChar == 3) m = m < 0.5f ? 0.0f : 1.0f;   // Robot: snap to a vowel
                        const Formant& fa = kVowels[vowA];
                        const Formant& fb = kVowels[vowB];
                        t[0] = fa.f1 * std::pow(fb.f1 / fa.f1, m);
                        t[1] = fa.f2 * std::pow(fb.f2 / fa.f2, m);
                        t[2] = fa.f3 * std::pow(fb.f3 / fa.f3, m);
                    }
                    for (int k = 0; k < 3; ++k)
                        vFsm_[k] = vSnap_ ? t[k] : vFsm_[k] + glideCoef * (t[k] - vFsm_[k]);
                    vSnap_ = false;
                    static const float kSingerMul[3] = {1.0f, 1.035f, 0.965f};
                    for (Channel& c : ch_)
                        for (int sg = 0; sg < nSingers; ++sg)
                            for (int k = 0; k < 3; ++k)
                                c.vow[sg * 3 + k].set(mpc::clamp(vFsm_[k] * genderMul * kSingerMul[sg], 40.0f, nyq), vowRes);
                }
            }

            // With Word Env the ADSR scales the amount of the vowel effect on a one-shot word.
            const float mixNow = wordEnvOn ? vowMix * adsr_ : vowMix;

            // One pass of the vowel filters; the result is mixed with the input by Vow Mix.
            auto vowelStage = [&](Channel& ch, float x) -> float {
                float src = x;
                if (vowChar == 2)   // Whisper: a noise voice that follows the input level
                    src = 0.3f * x + 0.9f * vowNoise * mpc::clamp(vEnv_ * 3.0f, 0.0f, 1.0f);
                float sum = 0.0f;
                for (int sg = 0; sg < nSingers; ++sg)
                    for (int k = 0; k < 3; ++k)
                    {
                        float lp, bp, hp;
                        ch.vow[sg * 3 + k].processAll(src, lp, bp, hp);
                        sum += bp * vowK * kFormGain[k] * singerGain;
                    }
                const float voiced = mpc::dsp::sanitize(sum * kVowelMakeup[mpc::clamp(vowChar, 0, 3)]);
                return x + (voiced - x) * mixNow;
            };

            // Ring modulator carrier (shared by both channels).
            float carrier = 0.0f;
            if (ringAmt > 0.0f)
            {
                carrier = mpc::dsp::fastSin01(ringPhase_);
                ringPhase_ += ringInc;
                if (ringPhase_ >= 1.0f) ringPhase_ -= 1.0f;
            }

            for (int c = 0; c < numChannels; ++c)
            {
                // Inputs can be absent: treat as silence.
                const float dry = (ctx.inputs != nullptr && c < ctx.numInputs && ctx.inputs[c] != nullptr)
                                      ? ctx.inputs[c][i] : 0.0f;
                Channel& ch = ch_[c];

                // Split into clean low band and the rest (filters keep running when Split is Off).
                const float lowF = ch.lowB.process(ch.lowA.process(dry));
                const float low  = splitOff ? 0.0f : lowF;
                float body = dry - low;

                // Pre tilt: boost highs (+) or lows (-) of the body before it hits the shaper.
                const float tl = ch.tilt.process(body);
                body = tl * tiltLo + (body - tl) * tiltHi;

                // Wobble filter before the shaper.
                if (wobPos == kWobPre) body = ch.wob.process(body, mpc::dsp::SVF::Mode::Lowpass);
                if (vowPos == 1) body = vowelStage(ch, body);

                // Distort the body.
                float d;
                if (multi)
                {
                    const float mid = ch.midB.process(ch.midA.process(body));
                    const float hi  = body - mid;
                    d = 0.7f * (shape(mode, mid * drive + biasOffset)
                              + shape(mode, hi * drive * hiGain + biasOffset) - 2.0f * shapeAtZero);
                }
                else
                {
                    d = shape(mode, body * drive + biasOffset) - shapeAtZero;
                }
                d = ch.dc.process(d);

                // Ring modulation.
                if (ringAmt > 0.0f) d += ringAmt * (d * carrier - d);

                // Bitcrush and sample-rate reduction.
                if (crushBit) d = std::round(d * levels) / levels;
                if (crushDec)
                {
                    ch.decAcc += 1.0f;
                    if (ch.decAcc >= decRatio) { ch.decAcc -= decRatio; ch.decHold = d; }
                    d = ch.decHold;
                }

                // Wobble filter after the crusher.
                if (wobPos == kWobPost) d = ch.wob.process(d, mpc::dsp::SVF::Mode::Lowpass);
                if (vowPos == 2) d = vowelStage(ch, d);

                d = ch.toneB.process(ch.toneA.process(d));

                // Level compensation. The shaper output is bounded, so the body stops getting
                // louder once it saturates (around 18 dB of drive). Pull it back only up to
                // that point (about -2.5 dB) and keep it constant above: attenuating further
                // makes extreme drive settings quiet and thin.
                const float comp = 1.0f / (1.0f + 0.045f * (std::min(drive, 8.0f) - 1.0f));

                // Octave-down generator: flip-flop toggled on each rising zero crossing of the
                // low-passed input, scaled by an envelope follower and rounded by a lowpass.
                float octSig = 0.0f;
                if (octOn)
                {
                    const float det = ch.octDetB.process(ch.octDetA.process(dry));
                    const float a   = std::fabs(det);
                    ch.octEnv = a > ch.octEnv ? a : ch.octEnv * octDecay_;
                    const float thr = 0.2f * ch.octEnv + 1.0e-5f;
                    if (!ch.octPos && det > thr) { ch.octPos = true; ch.octFlip = -ch.octFlip; }
                    else if (ch.octPos && det < -thr) ch.octPos = false;
                    octSig = ch.octOutB.process(ch.octOutA.process(ch.octFlip * ch.octEnv)) * oct * 0.9f;
                }

                float wet = (low * sub + d * comp + octSig) * out;
                wet = mpc::clamp(mpc::dsp::sanitize(wet), -4.0f, 4.0f);

                ctx.outputs[c][i] = dry + (wet - dry) * mix;
            }
        }
    }

    // Show "Off" where a control is effectively disabled.
    bool formatValue(int index, float plainValue, std::string& out) override
    {
        if (index == kSplit    && plainValue < kSplitOffHz) { out = "Off"; return true; }
        if (index == kDecimate && plainValue <= 1.05f)      { out = "Off"; return true; }
        if (index == kBits     && plainValue >= 15.5f)      { out = "Off"; return true; }
        return false;
    }

    // The Red Button: every write of 0.5 or more builds a random patch. The button shows "On"
    // until the next audio block, where process() puts it back to 0 so a saved project never
    // contains a pressed button.
    void parameterChanged(int index, float plainValue) override
    {
        if (index != kRedButton || plainValue < 0.5f) return;
        if (randomizing_.exchange(true)) return;
        randomizePatch(getInt(kSanity));
        redResetPending_.store(true);
        requestDisplayUpdate();   // the MPC does not re-read values by itself (see setProgram)
        randomizing_.store(false);
    }

private:
    // level 0 = Sober (musical ranges), 1 = Drunkish (wide and a bit broken), 2 = Bat Shit Crazy (everything).
    // Output is part of the patch and kept lower for wilder levels so a random patch is never a shock.
    void randomizePatch(int level)
    {
        if (rng_.s == 2463534242u)   // first use: seed from the clock and this object's address
            rng_.s = ((uint32_t)(std::chrono::steady_clock::now().time_since_epoch().count()) ^ (uint32_t)(uintptr_t)this) | 1u;
        for (int i = 0; i < 3; ++i) rng_.next01();
        Rng& r = rng_;
        level = mpc::clamp(level, 0, 2);
        const bool sober = level == 0, drunk = level == 1, crazy = level == 2;

        // sync choice indices are 1 + division index (0 = Free)
        static const int kSyncSober[]  = {8, 11, 14};                      // 1/8, 1/4, 1/2
        static const int kSyncDrunk[]  = {5, 6, 8, 9, 11, 14};             // 1/16, 1/8T, 1/8, 1/4T, 1/4, 1/2
        static const float kModeSober[] = {50, 35, 10, 5};
        static const float kModeDrunk[] = {30, 30, 25, 15};
        static const float kModeCrazy[] = {25, 25, 25, 25};
        static const float kShapeSober[] = {60, 40, 0, 0};
        static const float kShapeAny[]   = {25, 25, 25, 25};

        // --- distortion core ---
        set(kOutput, sober ? r.range(-5.0f, -2.0f) : drunk ? r.range(-7.0f, -3.0f) : r.range(-12.0f, -6.0f));
        set(kDrive,  sober ? r.range(6.0f, 22.0f) : drunk ? r.range(14.0f, 34.0f) : r.range(24.0f, 48.0f));
        set(kMode,   (float)r.pick(sober ? kModeSober : drunk ? kModeDrunk : kModeCrazy, 4));
        set(kBias,   sober ? r.range(-8.0f, 8.0f) : drunk ? r.range(-25.0f, 25.0f) : r.range(-50.0f, 50.0f));
        set(kTone,   sober ? r.logRange(3000.0f, 9000.0f) : drunk ? r.logRange(1800.0f, 11000.0f) : r.logRange(500.0f, 20000.0f));
        set(kMix,    sober ? r.range(75.0f, 100.0f) : drunk ? r.range(55.0f, 100.0f) : r.range(60.0f, 100.0f));
        set(kTilt,   sober ? r.range(-3.0f, 3.0f) : drunk ? r.range(-6.0f, 6.0f) : r.range(-12.0f, 12.0f));

        // --- split and sub ---
        const bool splitOn = r.chance(sober ? 0.9f : drunk ? 0.75f : 0.6f);
        set(kSplitOn, splitOn ? 1.0f : 0.0f);
        set(kSplit, sober ? r.logRange(50.0f, 140.0f) : drunk ? r.logRange(35.0f, 250.0f) : r.logRange(15.0f, 1000.0f));
        set(kSub,   sober ? r.range(80.0f, 100.0f) : drunk ? r.range(40.0f, 100.0f) : r.range(0.0f, 100.0f));
        set(kOctSub, r.chance(sober ? 0.3f : drunk ? 0.4f : 0.5f)
                         ? (sober ? r.range(20.0f, 55.0f) : drunk ? r.range(15.0f, 70.0f) : r.range(0.0f, 100.0f)) : 0.0f);

        // --- multiband ---
        set(kMultiband, r.chance(sober ? 0.1f : drunk ? 0.3f : 0.5f) ? 1.0f : 0.0f);
        set(kHiSplit,   sober ? r.logRange(2000.0f, 4000.0f) : drunk ? r.logRange(1500.0f, 5000.0f) : r.logRange(1000.0f, 8000.0f));
        set(kHiDrive,   sober ? r.range(-6.0f, 3.0f) : drunk ? r.range(-10.0f, 8.0f) : r.range(-24.0f, 24.0f));

        // --- crush and ring mod ---
        set(kCrushOn, crazy ? (r.chance(0.8f) ? 1.0f : 0.0f) : 1.0f);
        set(kBits,     r.chance(sober ? 0.0f : drunk ? 0.4f : 0.6f) ? (float)(drunk ? r.irange(7, 12) : r.irange(1, 12)) : 16.0f);
        set(kDecimate, r.chance(sober ? 0.0f : drunk ? 0.3f : 0.6f) ? (drunk ? r.range(1.5f, 4.0f) : r.logRange(1.5f, 40.0f)) : 1.0f);
        set(kRingOn,   crazy ? (r.chance(0.8f) ? 1.0f : 0.0f) : 1.0f);
        set(kRing,     r.chance(sober ? 0.0f : drunk ? 0.3f : 0.6f) ? (drunk ? r.range(10.0f, 45.0f) : r.range(0.0f, 100.0f)) : 0.0f);
        set(kRingHz,   sober ? r.logRange(80.0f, 300.0f) : drunk ? r.logRange(60.0f, 400.0f) : r.logRange(20.0f, 2000.0f));

        // --- wobble ---
        set(kWobOn, 1.0f);
        const bool wob = r.chance(sober ? 0.2f : drunk ? 0.5f : 0.85f);
        set(kWobPos,   wob ? (r.chance(sober ? 0.3f : 0.5f) ? (float)kWobPre : (float)kWobPost) : (float)kWobOff);
        set(kWobFreq,  sober ? r.logRange(300.0f, 900.0f) : drunk ? r.logRange(200.0f, 1500.0f) : r.logRange(40.0f, 8000.0f));
        set(kWobReso,  sober ? r.range(25.0f, 55.0f) : drunk ? r.range(30.0f, 75.0f) : r.range(0.0f, 100.0f));
        set(kWobDepth, sober ? r.range(25.0f, 60.0f) : drunk ? r.range(30.0f, 85.0f) : r.range(0.0f, 100.0f));
        set(kWobShape, (float)r.pick(sober ? kShapeSober : kShapeAny, 4));
        float sync;
        if (sober)      sync = (float)kSyncSober[r.irange(0, 2)];
        else if (drunk) sync = r.chance(0.2f) ? 0.0f : (float)kSyncDrunk[r.irange(0, 5)];
        else            sync = (float)r.irange(0, 17);
        set(kWobSync, sync);
        set(kWobRate, drunk ? r.logRange(0.5f, 8.0f) : r.logRange(sober ? 0.5f : 0.1f, sober ? 4.0f : 20.0f));

        // --- vowel stage ---
        static const float kVModeSober[] = {60, 30, 10, 0};   // Manual, LFO, ADSR, Word
        static const float kVModeDrunk[] = {20, 30, 20, 30};
        static const float kVModeCrazy[] = {10, 25, 25, 40};
        static const float kVCharSober[] = {85, 10, 5, 0};
        static const float kVCharDrunk[] = {55, 20, 15, 10};
        static const float kVCharCrazy[] = {25, 25, 25, 25};
        const bool vow = r.chance(sober ? 0.15f : drunk ? 0.4f : 0.75f);
        set(kVowOn,    vow ? 1.0f : 0.0f);
        set(kVowPos,   r.chance(0.2f) ? 1.0f : 2.0f);   // Pre or (mostly) Post: the vowels need harmonics
        set(kVowA,     (float)r.irange(0, kNumVowels - 1));
        set(kVowB,     (float)r.irange(0, kNumVowels - 1));
        set(kVowMorph, r.range(0.0f, 100.0f));
        set(kVowMode,  (float)r.pick(sober ? kVModeSober : drunk ? kVModeDrunk : kVModeCrazy, 4));
        set(kVowDepth, sober ? r.range(30.0f, 70.0f) : drunk ? r.range(40.0f, 90.0f) : r.range(0.0f, 100.0f));
        set(kVowGlide, sober ? r.logRange(15.0f, 120.0f) : drunk ? r.logRange(8.0f, 80.0f) : r.logRange(1.0f, 300.0f));
        set(kVowWord,  (float)r.irange(0, kNumWords - 1));
        set(kVowTrig,  r.chance(sober ? 0.0f : drunk ? 0.3f : 0.5f) ? 1.0f : 0.0f);
        for (int k = 0; k < 4; ++k) set(kVowS1 + k, (float)r.irange(0, kNumVowels - 1));
        set(kVowAtk, sober ? r.logRange(10.0f, 80.0f) : r.logRange(2.0f, 300.0f));
        set(kVowDec, sober ? r.logRange(150.0f, 600.0f) : r.logRange(40.0f, 1500.0f));
        set(kVowSus, r.range(0.0f, 70.0f));
        set(kVowSens, r.range(35.0f, 75.0f));
        set(kVowHold, r.chance(sober ? 0.3f : 0.5f) ? r.logRange(60.0f, 800.0f) : 0.0f);
        set(kVowEnv,  r.chance(0.3f) ? 1.0f : 0.0f);
        set(kVowGap,  r.logRange(80.0f, 400.0f));
        set(kVowRel, sober ? r.logRange(100.0f, 400.0f) : r.logRange(30.0f, 1500.0f));
        float vsync;
        if (sober)      vsync = (float)kSyncSober[r.irange(0, 2)];
        else if (drunk) vsync = r.chance(0.2f) ? 0.0f : (float)kSyncDrunk[r.irange(0, 5)];
        else            vsync = (float)r.irange(0, 17);
        set(kVowSync, vsync);
        set(kVowRate,  r.logRange(sober ? 0.5f : 0.3f, sober ? 4.0f : 12.0f));
        set(kGender,   sober ? r.range(-2.0f, 2.0f) : drunk ? r.range(-6.0f, 6.0f) : r.range(-12.0f, 12.0f));
        set(kVowQ,     sober ? r.range(40.0f, 70.0f) : drunk ? r.range(35.0f, 85.0f) : r.range(0.0f, 100.0f));
        set(kVowMix,   sober ? r.range(60.0f, 100.0f) : drunk ? r.range(50.0f, 100.0f) : r.range(40.0f, 100.0f));
        set(kVowChar,  (float)r.pick(sober ? kVCharSober : drunk ? kVCharDrunk : kVCharCrazy, 4));
    }

    void addProgram2(const char* name, const PS& p)
    {
        addProgram(name, {p.out, p.drv, p.mode, p.bias, p.split, p.sub, p.tone, p.mix, p.splitOn,
                          p.bits, p.dec, p.ring, p.ringHz, p.oct, p.tilt, p.multi, p.hiSplit, p.hiDrv,
                          p.wobPos, p.wobRate, p.wobDepth, p.wobFreq, p.wobReso, p.wobShape, p.wobSync,
                          p.crushOn, p.ringOn, p.wobOn,
                          NAN, NAN,   // Red Button and Sanity are not part of a preset (the framework skips NaN)
                          p.vowOn, p.vowPos, p.vowA, p.vowB, p.vowMorph, p.vowMode, p.vowRate, p.vowSync, p.vowDepth,
                          p.vowGlide, p.vowWord, p.gender, p.vowQ, p.vowMix, p.vowChar,
                          p.vowTrig, p.s1, p.s2, p.s3, p.s4, p.vowAtk, p.vowDec, p.vowSus, p.vowRel, p.vowSens, p.vowHold, p.vowEnv, p.vowGap});
    }

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "Bass Brutalizer";
        info.vendor       = "comrade fuzz";   // must match plugin.json "manufacturer"
        info.uniqueId     = MPC_FOURCC(kUid[0], kUid[1], kUid[2], kUid[3]);   // must match plugin.json "uid"
        info.version      = 1000;             // 1.0.0.0
        info.isInstrument = false;
        info.numInputs    = 2;
        info.numOutputs   = 2;
        info.tailSamples  = 1;                // no tail
        return info;
    }

    static constexpr char kUid[5] = "C4y8";

    Channel            ch_[kMaxChannels];
    mpc::dsp::Smoother drive_, bias_, sub_, out_, mix_, oct_;
    mpc::dsp::LFO      lfo_;
    Rng                rng_;
    std::atomic<bool>  randomizing_{false};
    std::atomic<bool>  redResetPending_{false};
    mpc::dsp::LFO      lfoV_;                 // vowel LFO
    float              vFsm_[3] = {300.0f, 870.0f, 2240.0f};   // smoothed formant frequencies
    float              vFast_ = 0.0f, fastCoef_ = 0.003f;
    float              vEnv_ = 0.0f, vSlow_ = 0.0f, vPeak_ = 0.0f, envAtk_ = 0.01f, envRel_ = 0.0002f, peakDecay_ = 0.99999f, slowCoef_ = 0.0003f;
    float              wordPhase_ = 1.0f;     // 0..1 position inside the word
    bool               wordPlaying_ = false;  // Hit mode: a word is running
    int                hitCooldown_ = 0, hitCooldownSamples_ = 3528;
    bool               hitArmed_ = true;
    float              hitPeak_ = 0.0f;   // loudest fast level since the last hit
    int                sinceHit_ = 0, hitRetriggerSamples_ = 15435;
    float              adsr_ = 0.0f, notePeak_ = 0.0f;   // Vow Mode ADSR: envelope value and loudest level of the current note
    int                holdCount_ = 0;
    int                adsrStage_ = 0;                    // 0 idle, 1 attack, 2 decay, 3 sustain, 4 release
    unsigned           vCount_ = 0;
    bool               vSnap_ = true, vowWasActive_ = false;
    uint32_t           vNoiseState_ = 0x9E3779B9u;
    float              ringPhase_ = 0.0f;
    float              octDecay_  = 0.999f;
    unsigned           wobCount_  = 0;
    double             sampleRate_ = 44100.0;
};

// This macro creates the exported VSTPluginMain entry point. Keep it last.
MPC_PLUGIN_ENTRY(BassBrutalizer)
