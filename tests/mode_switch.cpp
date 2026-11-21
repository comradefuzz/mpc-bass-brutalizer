// Silence in, switch Mode (and other params) while running: output must stay silent.
#include "mpc_vst2_abi.h"
#include <dlfcn.h>
#include <cmath>
#include <cstdio>
#include <vector>

static VstIntPtr host(AEffect*, VstInt32 op, VstInt32, VstIntPtr, void*, float) { return op == audioMasterVersion ? 2400 : 0; }
static const double SR = 44100.0;
static AEffect* e;

static void setp(int idx, const char* t) { e->dispatcher(e, effString2Parameter, idx, 0, (void*)t, 0); }

static double run(double secs)   // returns peak of the output for silent input
{
    const int block = 128, total = (int)(secs * SR);
    std::vector<float> l(block, 0.0f), r(block, 0.0f);
    float* bufs[2] = {l.data(), r.data()};
    double peak = 0;
    for (int s = 0; s < total; s += block)
    {
        std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
        e->processReplacing(e, bufs, bufs, block);
        for (int i = 0; i < block; ++i) peak = std::max(peak, (double)std::fabs(l[i]));
    }
    return peak;
}

int main(int argc, char** argv)
{
    void* h = dlopen(argv[1], RTLD_NOW);
    auto entry = (AEffect* (*)(audioMasterCallback))dlsym(h, "VSTPluginMain");
    e = entry(host);
    e->dispatcher(e, effOpen, 0, 0, nullptr, 0);
    e->dispatcher(e, effSetSampleRate, 0, 0, nullptr, (float)SR);
    e->dispatcher(e, effSetBlockSize, 0, 128, nullptr, 0);
    e->dispatcher(e, effMainsChanged, 0, 1, nullptr, 0);
    const char* modes[] = {"Soft", "Hard", "Fold", "Rect"};
    const char* biases[] = {"0", "15", "-30"};
    const char* multis[] = {"0", "1"};
    double worst = 0;
    for (const char* mb : multis)
    for (const char* bias : biases)
    {
        setp(15, mb); setp(3, bias); setp(1, "30"); setp(2, "Soft");
        run(0.6);
        for (const char* m : modes)
        {
            setp(2, m);
            double pk = run(0.4);
            worst = std::max(worst, pk);
            std::printf("multiband %s bias %-4s -> %-4s peak %.5f (%.1f dBFS)\n", mb, bias, m, pk, 20 * std::log10(pk + 1e-9));
        }
    }
    std::printf("WORST %.5f dBFS %.1f  %s\n", worst, 20 * std::log10(worst + 1e-9), worst < 1e-4 ? "PASS" : "FAIL");
    return worst < 1e-4 ? 0 : 1;
}
