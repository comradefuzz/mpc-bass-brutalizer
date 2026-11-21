// Feature check for Bass Brutalizer: drives the plugin through its VST2 interface with synthetic signals.
#include "mpc_vst2_abi.h"
#include <dlfcn.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static VstIntPtr host(AEffect*, VstInt32 op, VstInt32, VstIntPtr, void*, float) { return op == audioMasterVersion ? 2400 : 0; }

struct Plug
{
    AEffect* e = nullptr;
    void setp(int idx, const char* text) { e->dispatcher(e, effString2Parameter, idx, 0, (void*)text, 0); }
    void setp(int idx, double v) { char b[64]; std::snprintf(b, sizeof b, "%.6f", v); setp(idx, (const char*)b); }
    void setp(int idx, int v) { setp(idx, (double)v); }
};

static const double SR = 44100.0;

// Renders `secs` of an input and returns the left channel.
template <class Gen>
std::vector<float> render(Plug& p, double secs, Gen gen)
{
    const int block = 128, total = (int)(secs * SR);
    p.e->dispatcher(p.e, effMainsChanged, 0, 1, nullptr, 0);   // resume (resets state)
    std::vector<float> out; out.reserve(total);
    std::vector<float> l(block), r(block);
    float* bufs[2] = {l.data(), r.data()};
    for (int n = 0; n < total; n += block)
    {
        for (int i = 0; i < block; ++i) { l[i] = r[i] = gen(n + i); }
        p.e->processReplacing(p.e, bufs, bufs, block);
        for (int i = 0; i < block; ++i) out.push_back(l[i]);
    }
    return out;
}

static double goertzel(const std::vector<float>& x, size_t from, double hz)
{
    double w = 2.0 * M_PI * hz / SR, c = 2.0 * std::cos(w), s1 = 0, s2 = 0;
    size_t n = x.size() - from;
    for (size_t i = from; i < x.size(); ++i) { double s = x[i] + c * s1 - s2; s2 = s1; s1 = s; }
    double p = s1 * s1 + s2 * s2 - c * s1 * s2;
    return std::sqrt(std::max(0.0, p)) * 2.0 / (double)n;
}

static double rms(const std::vector<float>& x, size_t a, size_t b)
{
    double s = 0; for (size_t i = a; i < b; ++i) s += (double)x[i] * x[i];
    return std::sqrt(s / (double)(b - a));
}

int main(int argc, char** argv)
{
    void* h = dlopen(argv[1], RTLD_NOW);
    if (!h) { std::printf("dlopen failed\n"); return 2; }
    auto entry = (AEffect* (*)(audioMasterCallback))dlsym(h, "VSTPluginMain");
    Plug p; p.e = entry(host);
    p.e->dispatcher(p.e, effOpen, 0, 0, nullptr, 0);
    p.e->dispatcher(p.e, effSetSampleRate, 0, 0, nullptr, (float)SR);
    p.e->dispatcher(p.e, effSetBlockSize, 0, 128, nullptr, 0);
    int fails = 0;
    auto check = [&](bool ok, const char* what) { std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++fails; };

    // ---- Oct Sub: 55 Hz sine in, wet only (mix 100), clean sub off, so the 27.5 Hz line must come from the octave generator.
    auto sine55 = [](int n) { return 0.5f * (float)std::sin(2.0 * M_PI * 55.0 * n / SR); };
    p.setp(7, 100); p.setp(5, 0); p.setp(13, 0);
    auto a = render(p, 2.0, sine55);
    p.setp(13, 100);
    auto b = render(p, 2.0, sine55);
    double a27 = goertzel(a, (size_t)SR, 27.5), b27 = goertzel(b, (size_t)SR, 27.5);
    std::printf("INFO  27.5 Hz level: oct off %.5f, oct on %.5f\n", a27, b27);
    check(b27 > 0.02 && b27 > 10.0 * (a27 + 1e-6), "Oct Sub generates a clear octave-down line at 27.5 Hz");

    // silence in -> octave generator silent (no runaway)
    auto sil = render(p, 1.0, [](int) { return 0.0f; });
    check(rms(sil, (size_t)(0.5 * SR), sil.size()) < 1e-4, "Oct Sub is silent when the input is silent");
    p.setp(13, 0);

    // ---- Wobble: 80 Hz saw, split off, wobble Post, LFO 4 Hz free, depth 100%.
    auto saw80 = [](int n) { double ph = std::fmod(80.0 * n / SR, 1.0); return 0.4f * (float)(2.0 * ph - 1.0); };
    p.setp(8, 0); p.setp(1, 6); p.setp(6, 20000); p.setp(7, 100);
    p.setp(21, 300); p.setp(22, 60); p.setp(20, 100); p.setp(19, 4); p.setp(24, "Free");
    p.setp(18, "Off");
    auto off = render(p, 2.0, saw80);
    p.setp(18, "Post");
    auto on = render(p, 2.0, saw80);
    auto spread = [&](const std::vector<float>& x)
    {
        double lo = 1e9, hi = 0; size_t win = (size_t)(0.02 * SR);
        for (size_t s = (size_t)SR; s + win < x.size(); s += win) { double r = rms(x, s, s + win); lo = std::min(lo, r); hi = std::max(hi, r); }
        return hi / std::max(lo, 1e-9);
    };
    double so = spread(off), sn = spread(on);
    std::printf("INFO  level spread (max/min over 20 ms windows): wobble off %.2f, on %.2f\n", so, sn);
    check(sn > 2.0 && sn > 1.8 * so, "Wobble filter audibly sweeps the level");

    // ---- Wobble synced to tempo (host gives no time info -> defaults to 120 bpm): just must stay finite and moving.
    p.setp(24, "1/4");
    auto sy = render(p, 2.0, saw80);
    bool finite = true; for (float v : sy) if (!std::isfinite(v)) finite = false;
    check(finite && spread(sy) > 1.5, "Wobble works with tempo sync (1/4)");

    // ---- Extreme everything: finite and bounded.
    p.setp(1, 48); p.setp(2, "Fold"); p.setp(9, 3); p.setp(10, 20); p.setp(11, 100); p.setp(15, 1); p.setp(14, 12);
    p.setp(18, "Pre"); p.setp(22, 100); p.setp(13, 100); p.setp(8, 1);
    auto ex = render(p, 3.0, saw80);
    double peak = 0; bool fin = true; for (float v : ex) { if (!std::isfinite(v)) fin = false; peak = std::max(peak, (double)std::fabs(v)); }
    std::printf("INFO  extreme settings peak %.2f\n", peak);
    check(fin && peak <= 4.5, "Extreme settings stay finite and bounded");

    std::printf("%s (%d failure%s)\n", fails ? "RESULT: FAIL" : "RESULT: PASS", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
