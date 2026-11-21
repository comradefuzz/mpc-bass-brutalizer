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
constexpr int kNumVowels = 7;   // OO OH UH AH AE EH EE
constexpr Formant kVowels[kNumVowels] = {
    {300, 870, 2240}, {500, 830, 2500}, {640, 1190, 2390}, {730, 1090, 2440},
    {660, 1720, 2410}, {530, 1840, 2480}, {270, 2290, 3010}};
constexpr float kFormGain[3] = {1.0f, 0.75f, 0.45f};
constexpr int kNumWords = 9;
constexpr int kWords[kNumWords][4] = {
    {0, 3, 1, 0},   // WOW      oo ah oh oo
    {6, 5, 4, 6},   // YAY      ee eh ae ee
    {6, 1, 1, 0},   // YO       ee oh oh oo
    {6, 5, 4, 3},   // YEAH     ee eh ae ah
    {1, 1, 5, 6},   // YOI      oh oh eh ee
    {0, 2, 3, 3},   // WAH      oo uh ah ah
    {0, 0, 3, 3},   // OOH-AAH  oo oo ah ah
    {5, 1, 0, 0},   // EOW      eh oh oo oo
    {3, 0, 3, 0}};  // MAMA     ah oo ah oo
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
    float vowOn = 1, vowPos = 0, vowA = 0, vowB = 3, vowMorph = 0, vowMode = 0, vowRate = 2, vowSync = 0, vowDepth = 70,
          vowGlide = 25, vowWord = 0, gender = 0, vowQ = 60, vowMix = 100, vowChar = 0;

    // vowel stage: pos 0 Off / 1 Pre / 2 Post; mode 0 Manual / 1 LFO / 2 Env / 3 Step
    PS& Vow(float pos, float modeIdx, float word, float syncIdx, float rate = 2)
    { vowPos = pos; vowMode = modeIdx; vowWord = word; vowSync = syncIdx; vowRate = rate; return *this; }
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
        const std::vector<std::string> vowelNames{"OO", "OH", "UH", "AH", "AE", "EH", "EE"};
        addParam(mpc::boolParam  ("vowon",    "Vowel On",  true));
        addParam(mpc::choiceParam("vowpos",   "Vow Pos",   {"Off", "Pre", "Post"}, 0));
        addParam(mpc::choiceParam("vowa",     "Vowel A",   vowelNames, 0));
        addParam(mpc::choiceParam("vowb",     "Vowel B",   vowelNames, 3));
        addParam(mpc::floatParam ("vowmorph", "Morph",     0.0f, 100.0f, 0.0f,  "%",  0));
        addParam(mpc::choiceParam("vowmode",  "Vow Mode",  {"Manual", "LFO", "Env", "Step"}, 0));
        addParam(mpc::logParam   ("vowrate",  "Vow Rate",  0.1f, 20.0f,  2.0f,  "Hz", 2));
        {
            std::vector<std::string> names{"Free"};
            for (const std::string& n : mpc::dsp::divisionNames()) names.push_back(n);
            addParam(mpc::choiceParam("vowsync", "Vow Sync", names, 0));
        }
        addParam(mpc::floatParam ("vowdepth", "Vow Depth", 0.0f, 100.0f, 70.0f, "%",  0));
        addParam(mpc::logParam   ("vowglide", "Vow Glide", 1.0f, 300.0f, 25.0f, "ms", 0));
        addParam(mpc::choiceParam("vowword",  "Vow Word",
                                  {"WOW", "YAY", "YO", "YEAH", "YOI", "WAH", "OOH-AAH", "EOW", "MAMA"}, 0));
        addParam(mpc::floatParam ("gender",   "Gender",    -12.0f, 12.0f, 0.0f, "st", 1));
        addParam(mpc::floatParam ("vowq",     "Vow Q",     0.0f, 100.0f, 60.0f, "%",  0));
        addParam(mpc::floatParam ("vowmix",   "Vow Mix",   0.0f, 100.0f, 100.0f, "%", 0));
        addParam(mpc::choiceParam("vowchar",  "Vow Char",  {"Normal", "Choir", "Whisper", "Robot"}, 0));

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
        addProgram2("Wow Bass",       PS().Out(-5.5).Drv(30).Mode(kHard).Split(80).Sub(60).Tone(9000).Vow(2, 3, 0, 8));
        addProgram2("Yay Growl",      PS().Out(-11).Drv(34).Mode(kFold).SplitOff().Tone(9000).Vow(2, 3, 1, 8));
        addProgram2("Yo-Yo",          PS().Out(-7).Drv(32).Mode(kHard).Split(80).Sub(0).Tone(9000).Vow(2, 3, 2, 5));
        addProgram2("Yeah Bass",      PS().Out(-3).Drv(28).Split(90).Sub(80).Tone(8000).Vow(2, 3, 3, 11).Voice(0, 70, 100, 0, 60));
        addProgram2("Yoi Yoi",        PS().Out(-7).Drv(40).Mode(kHard).Split(80).Sub(0).Tone(9000).Vow(2, 3, 4, 5).Voice(0, 75, 100, 0, 15));
        addProgram2("Talking LFO",    PS().Out(-4).Drv(30).Mode(kHard).Split(80).Sub(40).Tone(9000).Vow(2, 1, 0, 11).VowAB(0, 3, 50, 100));
        addProgram2("Mouth Follow",   PS().Out(-7).Drv(30).Mode(kHard).Split(90).Sub(30).Tone(9000).Vow(2, 2, 0, 0).VowAB(0, 6, 0, 100).Voice(0, 60, 100, 0, 12));
        addProgram2("Choir Bass",     PS().Out(5).Drv(22).Split(90).Sub(60).Tone(7000).Vow(2, 1, 0, 14).VowAB(1, 5, 50, 70).Voice(0, 55, 80, 1, 60));
        addProgram2("Whisper Wub",    PS().Out(-4).Drv(30).Mode(kHard).Split(80).Sub(0).Tone(9000).Vow(2, 3, 8, 8).Voice(0, 60, 100, 2, 30));
        addProgram2("Chipmunk",       PS().Out(0).Drv(26).Mode(kHard).Split(80).Sub(40).Tone(10000).Vow(2, 3, 1, 5).Voice(9, 60, 100, 0, 15));
        addProgram2("Giant Talk",     PS().Out(-8).Drv(30).Split(80).Sub(60).Tone(7000).Vow(2, 3, 5, 11).Voice(-9, 60, 100, 0, 60));
        addProgram2("Robot Vowel",    PS().Out(-5).Drv(28).Mode(kHard).Split(80).Sub(50).Tone(8000).Vow(2, 3, 6, 8).Voice(0, 60, 100, 3, 5));
        addProgram2("Alien Eow",      PS().Out(-10).Drv(34).Mode(kFold).Split(80).Sub(30).Tone(9000).Vow(2, 3, 7, 8).Voice(5, 80, 100, 0, 40));
        addProgram2("Pre Growl",      PS().Out(-6).Drv(36).Mode(kHard).Split(80).Sub(40).Tone(8000).Vow(1, 1, 0, 11).VowAB(0, 3, 50, 100));
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
        vEnv_ = 0.0f; vCount_ = 0; stepPhase_ = 0.0f; stepIdx_ = 0; vSnap_ = true;
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
        const int   vowSync    = getInt(kVowSync);
        float stepSamples;
        if (vowSync > 0)
        {
            const float beats = mpc::dsp::divisionBeats(vowSync - 1);
            stepSamples = mpc::dsp::beatsToSamples(beats, ctx.transport.bpm, sampleRate_);
            lfoV_.setTempoSync(ctx.transport.bpm, beats);
            if (vowActive && ctx.transport.playing && ctx.transport.ppqValid)
            {
                const double pos = ctx.transport.ppqPos / (double)beats;
                const double whole = std::floor(pos);
                stepIdx_ = ((int)whole) & 3;
                stepPhase_ = (float)(pos - whole);
                lfoV_.syncToPpq(ctx.transport.ppqPos);
            }
        }
        else
        {
            const float rate = get(kVowRate);
            stepSamples = (float)sampleRate_ / rate;
            lfoV_.setFrequency(rate);
        }
        stepSamples = std::max(stepSamples, 16.0f);
        const float stepInc = 1.0f / stepSamples;
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

            // Vowel control (shared by both channels): envelope follower, step clock, and, every
            // kWobUpdate samples, the smoothed formant targets and filter coefficients.
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
                vEnv_ += (a > vEnv_ ? envAtk_ : envRel_) * (a - vEnv_);
                stepPhase_ += stepInc;
                if (stepPhase_ >= 1.0f) { stepPhase_ -= 1.0f; stepIdx_ = (stepIdx_ + 1) & 3; }
                const float lv = vowModeSel == 1 ? lfoV_.process() : 0.0f;
                vNoiseState_ ^= vNoiseState_ << 13; vNoiseState_ ^= vNoiseState_ >> 17; vNoiseState_ ^= vNoiseState_ << 5;
                vowNoise = (float)(vNoiseState_ >> 8) * (2.0f / 16777216.0f) - 1.0f;

                if ((vCount_++ % kWobUpdate) == 0)
                {
                    float t[3];
                    if (vowModeSel == 3)
                    {
                        const Formant& f = kVowels[kWords[vowWord][stepIdx_]];
                        t[0] = f.f1; t[1] = f.f2; t[2] = f.f3;
                    }
                    else
                    {
                        float m = vowMorph;
                        if (vowModeSel == 1)      m += vowDepth * lv;
                        else if (vowModeSel == 2) m += vowDepth * mpc::clamp(vEnv_ * 2.0f, 0.0f, 1.0f);
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
                return x + (voiced - x) * vowMix;
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
        static const float kVModeSober[] = {60, 30, 10, 0};
        static const float kVModeDrunk[] = {20, 30, 20, 30};
        static const float kVModeCrazy[] = {10, 25, 25, 40};
        static const float kVCharSober[] = {85, 10, 5, 0};
        static const float kVCharDrunk[] = {55, 20, 15, 10};
        static const float kVCharCrazy[] = {25, 25, 25, 25};
        set(kVowOn, 1.0f);
        const bool vow = r.chance(sober ? 0.15f : drunk ? 0.4f : 0.75f);
        set(kVowPos,   vow ? (r.chance(0.2f) ? 1.0f : 2.0f) : 0.0f);   // mostly Post: the vowels need harmonics
        set(kVowA,     (float)r.irange(0, kNumVowels - 1));
        set(kVowB,     (float)r.irange(0, kNumVowels - 1));
        set(kVowMorph, r.range(0.0f, 100.0f));
        set(kVowMode,  (float)r.pick(sober ? kVModeSober : drunk ? kVModeDrunk : kVModeCrazy, 4));
        set(kVowDepth, sober ? r.range(30.0f, 70.0f) : drunk ? r.range(40.0f, 90.0f) : r.range(0.0f, 100.0f));
        set(kVowGlide, sober ? r.logRange(15.0f, 120.0f) : drunk ? r.logRange(8.0f, 80.0f) : r.logRange(1.0f, 300.0f));
        set(kVowWord,  (float)r.irange(0, kNumWords - 1));
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
                          p.vowGlide, p.vowWord, p.gender, p.vowQ, p.vowMix, p.vowChar});
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
    float              vEnv_ = 0.0f, envAtk_ = 0.01f, envRel_ = 0.0002f;
    float              stepPhase_ = 0.0f;
    int                stepIdx_ = 0;
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
