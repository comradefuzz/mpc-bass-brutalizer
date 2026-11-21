// Vowel stage checks: word contours, one-shot Hit words, Env on a decaying hit, LFO rate, Gender, Mix 0, silence, loudness.
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
       VOWRATE = 36, VOWSYNC = 37, VOWDEPTH = 38, VOWGLIDE = 39, VOWWORD = 40, GENDER = 41, VOWQ = 42, VOWMIX = 43, VOWCHAR = 44,
       VOWTRIG = 45, S1 = 46, S2 = 47, S3 = 48, S4 = 49, VOWATK = 50, VOWDEC = 51, VOWSUS = 52, VOWREL = 53, VOWSENS = 54, VOWHOLD = 55, VOWENV = 56, VOWGAP = 57 };

template <class Gen>
std::vector<float> render(double secs, Gen gen)
{
    e->dispatcher(e, effMainsChanged, 0, 1, nullptr, 0);   // resume: resets state and the word clock
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

static double band(const std::vector<float>& x, double t0, double t1, double lo, double hi)
{
    size_t a = (size_t)(t0 * SR), b = std::min(x.size(), (size_t)(t1 * SR));
    double e2 = 0;   // energy at the harmonics of 80 Hz inside [lo, hi]
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
// how dark a window is: energy around OO's F2 (640-1100 Hz) over the energy around EE's F2 (1900-2600 Hz)
static double darkness(const std::vector<float>& x, double t0, double t1)
{ return band(x, t0, t1, 640, 1100) / std::max(1e-12, band(x, t0, t1, 1900, 2600)); }
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

    // an 80 Hz saw through a soft clipper: rich in harmonics, like a distorted bass
    auto saw = [](int n) { double ph = std::fmod(80.0 * n / SR, 1.0); return 0.4f * (float)(2.0 * ph - 1.0); };
    setn(DRIVE, 12); setp(MODE, "Soft"); setp(SPLITON, "0"); setn(OUT, 0);
    setp(VOWON, "1"); setp(VOWPOS, "Post"); setp(VOWMIX, "100"); setn(VOWQ, 70);

    // ---- level reference per character
    setp(VOWPOS, "Off");
    auto base = render(1.5, saw);
    double rBase = rms(base, (size_t)(0.3 * SR), base.size());
    setp(VOWPOS, "Post"); setp(VOWMODE, "Manual"); setp(VOWA, "AH"); setp(VOWB, "AH");
    const char* chars[] = {"Normal", "Choir", "Whisper", "Robot"};
    for (const char* c : chars)
    {
        setp(VOWCHAR, c);
        auto v = render(1.5, saw);
        std::printf("INFO  level vs vowel off, char %-8s: %+.1f dB\n", c, 20 * std::log10(rms(v, (size_t)(0.3 * SR), v.size()) / rBase + 1e-9));
    }
    setp(VOWCHAR, "Normal");

    // ---- all 15 vowels can be chosen, differ from each other and keep the level sane
    {
        const char* names[] = {"OO", "OH", "UH", "AH", "AE", "EH", "EE", "IH", "AW", "ER", "UE", "OE", "YI", "W", "L"};
        std::vector<std::vector<double>> sig;
        bool levelOk = true, finite = true;
        setp(VOWMODE, "Manual"); setn(MORPH, 0);
        for (const char* nm : names)
        {
            setp(VOWA, nm); setp(VOWB, nm);
            auto v = render(1.2, saw);
            std::vector<double> b = {band(v, 0.4, 1.2, 200, 450), band(v, 0.4, 1.2, 450, 900), band(v, 0.4, 1.2, 900, 1700), band(v, 0.4, 1.2, 1700, 2800)};
            double tot = 0; for (double q : b) tot += q;
            for (double& q : b) q /= std::max(tot, 1e-12);
            sig.push_back(b);
            double r = rms(v, (size_t)(0.4 * SR), v.size());
            if (!std::isfinite(r) || r < 0.5 * rBase || r > 2.0 * rBase) levelOk = false;
            for (float s : v) if (!std::isfinite(s)) finite = false;
        }
        int identical = 0;
        for (size_t i = 0; i < sig.size(); ++i) for (size_t j = i + 1; j < sig.size(); ++j)
        { double d = 0; for (int k = 0; k < 4; ++k) d += std::fabs(sig[i][k] - sig[j][k]); if (d < 0.05) { ++identical; std::printf("INFO  vowels %s and %s are close (%.3f)\n", names[i], names[j], d); } }
        check(finite && levelOk, "All 15 vowels are selectable, finite and within +-6 dB of the unprocessed level");
        check(identical <= 3, "The vowels sound different from each other (at most three close pairs)");
        setp(VOWA, "OO"); setp(VOWB, "AH");
    }

    // ---- Word mode, looping, 1 Hz (Vow Sync = Hz): WOW = OO(0) AH(1/3) OH(2/3) OO(1); look at the second cycle
    setp(VOWMODE, "Word"); setp(VOWTRIG, "Loop"); setp(VOWSYNC, "Hz"); setn(VOWRATE, 1.0); setn(VOWGLIDE, 5);
    setp(VOWWORD, "WOW");
    auto w = render(2.6, saw);
    double dOO = darkness(w, 1.00, 1.05), dAH = darkness(w, 1.31, 1.36);
    std::printf("INFO  WOW darkness (mid/high): key OO %.2f, key AH %.2f\n", dOO, dAH);
    check(std::fabs(10 * std::log10(dOO / dAH)) > 3.0, "A word glides: the first key vowel (OO) and the second (AH) sound different");
    setp(VOWWORD, "YAY");
    auto y = render(2.6, saw);
    double dEE = darkness(y, 1.00, 1.05);
    std::printf("INFO  darkness at key 0: WOW (OO) %.2f, YAY (EE) %.4f\n", dOO, dEE);
    check(dOO > 4.0 * dEE, "EE is bright, OO is dark");

    // ---- Custom word from Step 1..4, and a gradual-glide check on its first segment (EE -> OO)
    setp(VOWWORD, "CUSTOM"); setp(S1, "EE"); setp(S2, "OO"); setp(S3, "EE"); setp(S4, "OO");
    auto cw = render(2.6, saw);
    double cEE = darkness(cw, 1.00, 1.05), cOO = darkness(cw, 1.31, 1.36);
    double c_a = darkness(cw, 1.10, 1.14), c_b = darkness(cw, 1.20, 1.24);
    std::printf("INFO  CUSTOM EE-OO-EE-OO: key 0 %.3f, key 1 %.3f; along the glide %.3f -> %.3f -> %.3f -> %.3f\n", cEE, cOO, cEE, c_a, c_b, cOO);
    check(cOO > 4.0 * cEE, "Custom word: Step 1 (EE) and Step 2 (OO) are used as written");
    check(cEE < c_a && c_a < c_b && c_b < cOO, "Between the keys the vowel moves gradually (a contour, not steps)");

    // ---- Hit mode: a one-shot YOY (EE OH OH EE) of 0.5 s per hit (Vow Sync 1/4 at 120 bpm)
    setp(VOWWORD, "YOY"); setp(VOWTRIG, "Hit"); setp(VOWSYNC, "1/4"); setn(VOWGLIDE, 5);
    auto burst = [&](int n)
    {
        double t = n / SR;
        bool on = (t >= 0.3 && t < 1.5) || (t >= 2.3 && t < 3.5);
        return on ? saw(n) : 0.0f;
    };
    auto hit = render(3.6, burst);
    double h0 = darkness(hit, 0.31, 0.35), h1 = darkness(hit, 0.50, 0.60), h2 = darkness(hit, 0.95, 1.30);
    double g0 = darkness(hit, 2.31, 2.35), g1 = darkness(hit, 2.50, 2.60);
    std::printf("INFO  YOY hit darkness: start %.3f, middle %.2f, held end %.3f | second hit: start %.3f, middle %.2f\n", h0, h1, h2, g0, g1);
    check(h1 > 2.5 * h0 && h1 > 2.5 * h2, "Hit word: bright EE, then dark OH in the middle, then EE again and held");
    check(g1 > 2.5 * g0, "Every hit restarts the word");

    // ---- ADSR: each hit opens the vowel from A (OO, dark) to B (EE, bright), holds at Sustain while the note lasts
    //      and closes again when its level has fallen about 18 dB. Timing must not depend on how the source decays.
    setp(VOWMODE, "ADSR"); setp(VOWA, "OO"); setp(VOWB, "EE"); setn(MORPH, 0); setn(VOWDEPTH, 100); setn(VOWGLIDE, 3);
    setn(VOWATK, 20); setn(VOWDEC, 150); setn(VOWSUS, 40); setn(VOWREL, 150);
    auto tail = [&](double rate) { return [=](int n) { double t = n / SR - 0.3; return t < 0 ? 0.0f : (float)(std::exp(-t * rate) * saw(n) * 2.0); }; };
    auto fast = render(2.2, tail(3.0));      // -18 dB after about 0.7 s
    auto slow = render(3.2, tail(0.8));      // -18 dB after about 2.6 s
    double peakF = darkness(fast, 0.34, 0.37), susF = darkness(fast, 0.55, 0.70), endF = darkness(fast, 1.60, 1.90);
    double peakS = darkness(slow, 0.34, 0.37), susS = darkness(slow, 0.80, 1.20), endS = darkness(slow, 2.90, 3.15);
    std::printf("INFO  ADSR darkness, fast decay: peak %.3f, sustain %.3f, after release %.2f | slow decay: peak %.3f, sustain %.3f, after release %.2f\n",
                peakF, susF, endF, peakS, susS, endS);
    check(peakF < susF && susF < endF && endF > 4.0 * peakF, "ADSR: opens to B on the hit, settles at Sustain, closes to A after the note");
    check(peakS < 0.5 && peakF < 0.5, "ADSR: the attack happens on the hit whatever the source does afterwards");
    check(endS > 4.0 * peakS, "ADSR: a slowly decaying source closes later, still back at A at the end");

    // ---- ADSR Hold: with Hold the note lasts a fixed time even if the source keeps playing; without it a steady tone stays open
    setn(VOWATK, 5); setn(VOWDEC, 20); setn(VOWSUS, 100); setn(VOWREL, 50);
    auto steady = [&](int n) { return n / SR < 0.3 ? 0.0f : saw(n); };
    setn(VOWHOLD, 0);
    auto noHold = render(1.6, steady);
    setn(VOWHOLD, 200);
    auto withHold = render(1.6, steady);
    double openNo = darkness(noHold, 1.0, 1.4), openHold = darkness(withHold, 0.35, 0.45), closedHold = darkness(withHold, 1.0, 1.4);
    std::printf("INFO  ADSR Hold: steady tone without Hold %.3f (stays open), with Hold 200 ms: during %.3f, after %.2f\n", openNo, openHold, closedHold);
    check(openNo < 0.5 && openHold < 0.5 && closedHold > 4.0 * openHold, "ADSR Hold: the note ends after Hold even while the source keeps playing");
    setn(VOWHOLD, 0);

    // ---- Sens: a gentle rise is missed at low sensitivity and caught at high sensitivity
    auto gentle = [&](int n) { double t = n / SR; double g = t < 0.5 ? 0.5 : 1.0; return (float)(g * saw(n)); };   // a +6 dB step on top of a running tone
    setn(VOWHOLD, 150); setn(VOWATK, 5);
    setn(VOWSENS, 5);
    auto lowS = render(1.2, gentle);
    setn(VOWSENS, 95);
    auto highS = render(1.2, gentle);
    double lowDark = darkness(lowS, 0.51, 0.60), highDark = darkness(highS, 0.51, 0.60);
    std::printf("INFO  Sens on a gentle +6 dB step: Sens 5%% -> %.2f (closed), Sens 95%% -> %.2f (opened)\n", lowDark, highDark);
    check(lowDark > 3.0 * highDark, "Sens: low sensitivity ignores a gentle rise, high sensitivity catches it");
    setn(VOWSENS, 50); setn(VOWHOLD, 0);

    // ---- Word Env: on a one-shot word the ADSR sets the amount of the vowel effect (attack fades it in)
    setp(VOWMODE, "Word"); setp(VOWWORD, "YOY"); setp(VOWTRIG, "Hit"); setp(VOWSYNC, "1/4"); setn(VOWGLIDE, 5);
    setn(VOWATK, 300); setn(VOWDEC, 50); setn(VOWSUS, 100); setn(VOWREL, 100); setn(VOWHOLD, 800); setn(VOWSENS, 50);
    auto steady2 = [&](int n) { return n / SR < 0.3 ? 0.0f : saw(n); };
    setp(VOWENV, "0");
    auto noEnv = render(1.6, steady2);
    setp(VOWENV, "1");
    auto envOn = render(1.6, steady2);
    setp(VOWPOS, "Off");
    auto dryRef = render(1.6, steady2);
    setp(VOWPOS, "Post");
    auto diff = [&](const std::vector<float>& a, const std::vector<float>& b, double t0, double t1)
    { double s = 0; size_t i0 = (size_t)(t0 * SR), i1 = (size_t)(t1 * SR); for (size_t i = i0; i < i1; ++i) s += std::pow((double)a[i] - b[i], 2); return std::sqrt(s / (double)(i1 - i0)); };
    double early = diff(envOn, dryRef, 0.31, 0.34), late = diff(envOn, dryRef, 0.65, 0.75);
    double earlyNo = diff(noEnv, dryRef, 0.31, 0.34);
    std::printf("INFO  Word Env: effect amount early (attack) %.3f, later %.3f; without Word Env early %.3f\n", early, late, earlyNo);
    check(early < 0.4 * late && earlyNo > 2.0 * early, "Word Env: the hit word fades in with the ADSR attack, and without Word Env it does not");
    setp(VOWENV, "0"); setn(VOWHOLD, 0);

    // ---- no double trigger: a bump of +4 dB inside a decaying hit must not restart the ADSR, a real second hit must
    setp(VOWMODE, "ADSR"); setp(VOWA, "OO"); setp(VOWB, "EE"); setn(MORPH, 0); setn(VOWDEPTH, 100); setn(VOWGLIDE, 3);
    setn(VOWATK, 5); setn(VOWDEC, 100); setn(VOWSUS, 0); setn(VOWREL, 80); setn(VOWHOLD, 0);
    auto bumpy = [&](int n)
    {
        double t = n / SR;
        double env = 0.0;
        if (t >= 0.3 && t < 1.0) env = std::exp(-(t - 0.3) * 4.0) * (1.0 + 0.6 * std::exp(-std::pow((t - 0.45) / 0.03, 2.0)));
        else if (t >= 1.0) env = std::exp(-(t - 1.0) * 4.0);
        return (float)(env * saw(n) * 2.0);
    };
    setn(VOWGAP, 200);   // a bump 150 ms after the hit is inside the gap
    for (double sens : {50.0, 70.0, 90.0})
    {
        setn(VOWSENS, sens);
        auto bm = render(1.4, bumpy);
        double afterBump = darkness(bm, 0.50, 0.56), secondHit = darkness(bm, 1.01, 1.05), firstHit = darkness(bm, 0.31, 0.34);
        std::printf("INFO  Sens %.0f%%: first hit %.3f, after the bump %.2f (should stay closed), second hit %.3f\n", sens, firstHit, afterBump, secondHit);
        char what[96]; std::snprintf(what, sizeof what, "Sens %.0f%%: a bump inside a decaying hit does not retrigger, a real second hit does", sens);
        check(afterBump > 10.0 * firstHit && secondHit < 0.2 * afterBump, what);
    }
    setn(VOWSENS, 50); setn(VOWGAP, 150);

    // ---- LFO mode: Vow Rate sets the speed when Vow Sync = Hz
    setp(VOWMODE, "LFO"); setp(VOWSYNC, "Hz"); setn(VOWDEPTH, 100); setn(MORPH, 50); setn(VOWGLIDE, 5);
    auto crossings = [&](double hz)
    {
        setn(VOWRATE, hz);
        auto v = render(3.0, saw);
        std::vector<double> d; for (double t = 0.5; t < 2.9; t += 0.03) d.push_back(std::log(darkness(v, t, t + 0.03) + 1e-9));
        double mean = 0; for (double x : d) mean += x; mean /= (double)d.size();
        int c = 0; for (size_t i = 1; i < d.size(); ++i) if ((d[i - 1] - mean) * (d[i] - mean) < 0) ++c;
        return c;
    };
    int c1 = crossings(1.0), c4 = crossings(4.0);
    std::printf("INFO  LFO mean crossings in 2.4 s: Vow Rate 1 Hz %d, 4 Hz %d\n", c1, c4);
    check(c4 >= 2 * c1 && c4 >= 8, "Vow Rate changes the LFO speed (with Vow Sync = Hz)");

    // ---- Gender: +12 st doubles the formants
    setp(VOWMODE, "Manual"); setp(VOWA, "OO"); setp(VOWB, "OO"); setn(GENDER, 0);
    auto g0w = render(1.2, saw);
    setn(GENDER, 12);
    auto g1w = render(1.2, saw);
    double a0 = band(g0w, 0.4, 1.2, 200, 400), a1 = band(g1w, 0.4, 1.2, 200, 400);
    std::printf("INFO  200-400 Hz energy: gender 0 %.2e, gender +12 %.2e\n", a0, a1);
    check(a0 > 3.0 * a1, "Gender +12 moves the formants up");
    setn(GENDER, 0);

    // ---- Mix 0 must be transparent
    setp(VOWMODE, "Word"); setp(VOWTRIG, "Loop"); setp(VOWMIX, "0");
    auto m0 = render(0.6, saw);
    setp(VOWPOS, "Off");
    auto off = render(0.6, saw);
    double maxd = 0; for (size_t i = 0; i < m0.size(); ++i) maxd = std::max(maxd, (double)std::fabs(m0[i] - off[i]));
    check(maxd < 1e-4, "Vow Mix 0 equals vowel off");
    setp(VOWPOS, "Post"); setp(VOWMIX, "100");

    // ---- silence stays silent in every mode, character and trigger
    double worst = 0;
    const char* modes[] = {"Manual", "Env", "LFO", "Word"};
    for (const char* mo : modes) for (const char* c : chars) for (const char* tr : {"Loop", "Hit"})
    {
        setp(VOWMODE, mo); setp(VOWCHAR, c); setp(VOWTRIG, tr);
        auto s = render(0.5, [](int) { return 0.0f; });
        for (size_t i = (size_t)(0.1 * SR); i < s.size(); ++i) worst = std::max(worst, (double)std::fabs(s[i]));
    }
    std::printf("INFO  worst output on silence: %.2e\n", worst);
    check(worst < 1e-4, "Silence stays silent in all modes, characters and triggers");

    // ---- extremes: finite and bounded
    setp(VOWMODE, "LFO"); setp(VOWCHAR, "Choir"); setn(GENDER, -12); setn(VOWQ, 100); setn(VOWGLIDE, 1);
    auto x = render(2.0, saw);
    bool fin = true; double pk = 0; for (float v : x) { if (!std::isfinite(v)) fin = false; pk = std::max(pk, (double)std::fabs(v)); }
    std::printf("INFO  extreme vowel settings peak %.2f\n", pk);
    check(fin && pk <= 4.01, "Extreme vowel settings stay finite and bounded");

    std::printf("%s (%d failure%s)\n", fails ? "RESULT: FAIL" : "RESULT: PASS", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
