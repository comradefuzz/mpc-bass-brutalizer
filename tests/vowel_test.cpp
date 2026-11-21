// Vowel stage checks: steps change the formants, Mix 0 is transparent, silence stays silent, loudness is sane.
#include "mpc_vst2_abi.h"
#include <dlfcn.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>

static VstIntPtr host(AEffect*, VstInt32 op, VstInt32, VstIntPtr, void*, float) { return op == audioMasterVersion ? 2400 : 0; }
static const double SR = 44100.0;
static AEffect* e;
static void setp(int idx, const char* t) { e->dispatcher(e, effString2Parameter, idx, 0, (void*)t, 0); }
static void setn(int idx, double v) { char b[64]; std::snprintf(b, sizeof b, "%.5f", v); setp(idx, b); }

enum { OUT = 0, DRIVE = 1, MODE = 2, SPLITON = 8, VOWON = 30, VOWPOS = 31, VOWA = 32, VOWB = 33, MORPH = 34, VOWMODE = 35,
       VOWRATE = 36, VOWSYNC = 37, VOWDEPTH = 38, VOWGLIDE = 39, VOWWORD = 40, GENDER = 41, VOWQ = 42, VOWMIX = 43, VOWCHAR = 44 };

template <class Gen>
std::vector<float> render(double secs, Gen gen)
{
    e->dispatcher(e, effMainsChanged, 0, 1, nullptr, 0);   // resume: resets state and the step clock
    const int block = 128, total = (int)(secs * SR);
    std::vector<float> out; out.reserve(total);
    std::vector<float> l(block), r(block);
    float* bufs[2] = {l.data(), r.data()};
    for (int n = 0; n < total; n += block)
    {
        for (int i = 0; i < block; ++i) l[i] = r[i] = gen(n + i);
        e->processReplacing(e, bufs, bufs, block);
        for (int i = 0; i < block; ++i) out.push_back(l[i]);
    }
    return out;
}

static double band(const std::vector<float>& x, size_t a, size_t b, double lo, double hi)
{
    // energy at the harmonics of 80 Hz inside [lo, hi]
    double e2 = 0;
    for (int k = 1; k <= 60; ++k)
    {
        double f = 80.0 * k; if (f < lo || f > hi) continue;
        double w = 2 * M_PI * f / SR, c = 2 * std::cos(w), s1 = 0, s2 = 0;
        for (size_t i = a; i < b; ++i) { double v = x[i] + c * s1 - s2; s2 = s1; s1 = v; }
        double pw = s1 * s1 + s2 * s2 - c * s1 * s2;
        double amp = std::sqrt(std::max(0.0, pw)) * 2.0 / (double)(b - a);
        e2 += amp * amp;
    }
    return e2;
}
static double rms(const std::vector<float>& x, size_t a, size_t b)
{ double s = 0; for (size_t i = a; i < b; ++i) s += (double)x[i] * x[i]; return std::sqrt(s / (double)(b - a)); }

int main(int argc, char** argv)
{
    void* h = dlopen(argv[1], RTLD_NOW);
    auto entry = (AEffect* (*)(audioMasterCallback))dlsym(h, "VSTPluginMain");
    e = entry(host);
    e->dispatcher(e, effOpen, 0, 0, nullptr, 0);
    e->dispatcher(e, effSetSampleRate, 0, 0, nullptr, (float)SR);
    e->dispatcher(e, effSetBlockSize, 0, 128, nullptr, 0);
    int fails = 0;
    auto check = [&](bool ok, const char* what) { std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++fails; };

    // a distorted saw as the bass source: rich in harmonics, 80 Hz
    auto saw = [](int n) { double ph = std::fmod(80.0 * n / SR, 1.0); return 0.4f * (float)(2.0 * ph - 1.0); };
    setn(DRIVE, 12); setp(MODE, "Soft"); setp(SPLITON, "0"); setn(OUT, 0);
    setp(VOWON, "1"); setp(VOWPOS, "Post"); setp(VOWMIX, "100"); setn(VOWQ, 60);

    // ---- level reference: vowel off vs Normal at Mix 100
    setp(VOWPOS, "Off");
    auto base = render(1.5, saw);
    double rBase = rms(base, (size_t)(0.3 * SR), base.size());
    setp(VOWPOS, "Post"); setp(VOWMODE, "Manual"); setp(VOWA, "AH"); setp(VOWB, "AH");
    const char* chars[] = {"Normal", "Choir", "Whisper", "Robot"};
    for (const char* c : chars)
    {
        setp(VOWCHAR, c);
        auto v = render(1.5, saw);
        double r = rms(v, (size_t)(0.3 * SR), v.size());
        std::printf("INFO  level vs vowel off, char %-8s: %+.1f dB\n", c, 20 * std::log10(r / rBase + 1e-9));
    }
    setp(VOWCHAR, "Normal");

    // ---- Step mode, word WOW at 1/8 (0.25 s per step at the default 120 bpm): step 0 = OO, step 1 = AH
    setp(VOWMODE, "Step"); setp(VOWWORD, "WOW"); setp(VOWSYNC, "1/8"); setn(VOWGLIDE, 5);
    auto w = render(1.2, saw);
    size_t s0a = (size_t)(0.10 * SR), s0b = (size_t)(0.24 * SR), s1a = (size_t)(0.35 * SR), s1b = (size_t)(0.49 * SR);
    double lo0 = band(w, s0a, s0b, 200, 450), mid0 = band(w, s0a, s0b, 640, 900);
    double lo1 = band(w, s1a, s1b, 200, 450), mid1 = band(w, s1a, s1b, 640, 900);
    double r0 = 10 * std::log10((lo0 + 1e-12) / (mid0 + 1e-12)), r1 = 10 * std::log10((lo1 + 1e-12) / (mid1 + 1e-12));
    std::printf("INFO  WOW low/mid energy ratio: step0 (OO) %.1f dB, step1 (AH) %.1f dB\n", r0, r1);
    check(r0 - r1 > 6.0, "Step mode moves the formants: OO is low-heavy, AH is mid-heavy");

    // ---- OO has its energy low (F2 870), EE high (F2 2290): compare the mid/high band ratio of the two
    setp(VOWWORD, "YAY");
    auto y = render(1.2, saw);
    double ratioEE = band(y, s0a, s0b, 640, 1100) / std::max(1e-12, band(y, s0a, s0b, 1900, 2600));
    double ratioOO = band(w, s0a, s0b, 640, 1100) / std::max(1e-12, band(w, s0a, s0b, 1900, 2600));
    std::printf("INFO  mid/high energy ratio: YAY step0 (EE) %.2f, WOW step0 (OO) %.2f\n", ratioEE, ratioOO);
    check(ratioOO > 4.0 * ratioEE, "EE is bright, OO is dark");

    // ---- Gender: +12 st doubles formants, so OO F1 energy moves up
    setp(VOWMODE, "Manual"); setp(VOWA, "OO"); setp(VOWB, "OO"); setn(GENDER, 0);
    auto g0 = render(1.2, saw);
    setn(GENDER, 12);
    auto g1 = render(1.2, saw);
    double a0 = band(g0, (size_t)(0.4 * SR), g0.size(), 200, 400), a1 = band(g1, (size_t)(0.4 * SR), g1.size(), 200, 400);
    std::printf("INFO  200-400 Hz energy: gender 0 %.2e, gender +12 %.2e\n", a0, a1);
    check(a0 > 3.0 * a1, "Gender +12 moves the formants up (less energy where OO F1 used to be)");
    setn(GENDER, 0);

    // ---- Env mode on a decaying hit (808 style): with depth 100 the vowel opens toward B on the loud
    //      start and closes toward A as the note dies; with depth 0 it does not move.
    setp(VOWMODE, "Env"); setp(VOWA, "OO"); setp(VOWB, "EE"); setn(MORPH, 0); setn(VOWGLIDE, 5);
    auto hit = [&](int n) { return (float)(std::exp(-n / SR * 1.6) * saw(n)); };
    auto brightness = [&](const std::vector<float>& v, double t0, double t1)
    { return band(v, (size_t)(t0 * SR), (size_t)(t1 * SR), 1900, 2600) / std::max(1e-12, band(v, (size_t)(t0 * SR), (size_t)(t1 * SR), 200, 450)); };
    setn(VOWDEPTH, 0);
    auto d0 = render(2.0, hit);
    setn(VOWDEPTH, 100);
    auto d1 = render(2.0, hit);
    double drift0 = brightness(d0, 0.1, 0.3) / std::max(1e-12, brightness(d0, 1.0, 1.4));
    double drift1 = brightness(d1, 0.1, 0.3) / std::max(1e-12, brightness(d1, 1.0, 1.4));
    std::printf("INFO  brightness early/late on a decaying hit: depth 0 %.2f, depth 100 %.2f\n", drift0, drift1);
    check(drift1 > 2.0 * drift0, "Env mode: the vowel follows the decay (opens on the loud start, closes as it dies)");

    // ---- Mix 0 must be transparent
    setp(VOWMODE, "Step"); setp(VOWMIX, "0");
    auto m0 = render(0.6, saw);
    setp(VOWPOS, "Off");
    auto off = render(0.6, saw);
    double maxd = 0; for (size_t i = 0; i < m0.size(); ++i) maxd = std::max(maxd, (double)std::fabs(m0[i] - off[i]));
    check(maxd < 1e-4, "Vow Mix 0 equals vowel off");
    setp(VOWPOS, "Post"); setp(VOWMIX, "100");

    // ---- silence stays silent in every mode and character, also when switching
    double worst = 0;
    const char* modes[] = {"Manual", "LFO", "Env", "Step"};
    for (const char* mo : modes) for (const char* c : chars)
    {
        setp(VOWMODE, mo); setp(VOWCHAR, c);
        auto s = render(0.5, [](int) { return 0.0f; });
        for (size_t i = (size_t)(0.1 * SR); i < s.size(); ++i) worst = std::max(worst, (double)std::fabs(s[i]));
    }
    std::printf("INFO  worst output on silence: %.2e\n", worst);
    check(worst < 1e-4, "Silence stays silent in all modes and characters");

    // ---- extremes: finite and bounded
    setp(VOWMODE, "LFO"); setp(VOWCHAR, "Choir"); setn(GENDER, -12); setn(VOWQ, 100); setn(VOWGLIDE, 1);
    auto x = render(2.0, saw);
    bool fin = true; double pk = 0; for (float v : x) { if (!std::isfinite(v)) fin = false; pk = std::max(pk, (double)std::fabs(v)); }
    std::printf("INFO  extreme vowel settings peak %.2f\n", pk);
    check(fin && pk <= 4.01, "Extreme vowel settings stay finite and bounded");

    std::printf("%s (%d failure%s)\n", fails ? "RESULT: FAIL" : "RESULT: PASS", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
