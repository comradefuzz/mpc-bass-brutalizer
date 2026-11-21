#include "mpc_vst2_abi.h"
#include <dlfcn.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
static int updates = 0;
static VstIntPtr host(AEffect*, VstInt32 op, VstInt32, VstIntPtr, void*, float) { if (op == audioMasterUpdateDisplay) ++updates; return op == audioMasterVersion ? 2400 : 0; }
static const double SR = 44100.0;
static AEffect* e;
static void setp(int idx, const char* t) { e->dispatcher(e, effString2Parameter, idx, 0, (void*)t, 0); }
static double disp(int idx) { char b[64] = {0}; e->dispatcher(e, effGetParamDisplay, idx, 0, b, 0); return std::atof(b); }
static std::string dispS(int idx) { char b[64] = {0}; e->dispatcher(e, effGetParamDisplay, idx, 0, b, 0); return b; }
int main(int argc, char** argv)
{
    void* h = dlopen(argv[1], RTLD_NOW);
    auto entry = (AEffect* (*)(audioMasterCallback))dlsym(h, "VSTPluginMain");
    e = entry(host);
    e->dispatcher(e, effOpen, 0, 0, nullptr, 0);
    e->dispatcher(e, effSetSampleRate, 0, 0, nullptr, (float)SR);
    e->dispatcher(e, effSetBlockSize, 0, 128, nullptr, 0);
    e->dispatcher(e, effMainsChanged, 0, 1, nullptr, 0);
    std::printf("params: %d\n", e->numParams);
    const int BTN = 28, SAN = 29;
    const char* names[] = {"Sober", "Drunkish", "Bat Shit Crazy"};
    int fails = 0;
    for (int lvl = 0; lvl < 3; ++lvl)
    {
        setp(SAN, names[lvl]);
        std::printf("== %s (display '%s')\n", names[lvl], dispS(SAN).c_str());
        double dMin = 1e9, dMax = -1e9, oMin = 1e9, oMax = -1e9, peakAll = 0, rmsMin = 1e9, rmsMax = -1e9;
        std::vector<std::string> seen; int distinct = 0; int btnStuck = 0; int bitsLow = 0, wobOnN = 0, splitOffN = 0;
        const int N = 300;
        for (int k = 0; k < N; ++k)
        {
            setp(BTN, "1");                         // press
            if (e->getParameter(e, BTN) < 0.5f) ++btnStuck;      // still shows On right after the press
            double drv = disp(1), out = disp(0);
            dMin = std::min(dMin, drv); dMax = std::max(dMax, drv); oMin = std::min(oMin, out); oMax = std::max(oMax, out);
            if (disp(9) < 16) ++bitsLow;
            if (dispS(18) != "Off") ++wobOnN;
            if (dispS(4) == "Off") ++splitOffN;
            std::string sig = dispS(1) + dispS(2) + dispS(3) + dispS(4) + dispS(6) + dispS(9) + dispS(11) + dispS(21);
            if (std::find(seen.begin(), seen.end(), sig) == seen.end()) { seen.push_back(sig); ++distinct; }
            // audio check
            e->dispatcher(e, effMainsChanged, 0, 1, nullptr, 0);
            const int block = 128, total = (int)(0.4 * SR);
            std::vector<float> l(block), r(block);
            float* bufs[2] = {l.data(), r.data()};
            double sum = 0, pk = 0; long cnt = 0; bool finite = true;
            for (int s = 0; s < total; s += block)
            {
                for (int i = 0; i < block; ++i) { double t = (s + i) / SR; l[i] = r[i] = (float)(0.5 * std::sin(2 * M_PI * 55.0 * t)); }
                e->processReplacing(e, bufs, bufs, block);
                for (int i = 0; i < block; ++i) { if (!std::isfinite(l[i])) finite = false; pk = std::max(pk, (double)std::fabs(l[i])); sum += (double)l[i] * l[i]; ++cnt; }
            }
            double rms = 20 * std::log10(std::sqrt(sum / cnt) + 1e-9);
            if (!finite) { ++fails; std::printf("NON-FINITE output at press %d\n", k); }
            peakAll = std::max(peakAll, pk); rmsMin = std::min(rmsMin, rms); rmsMax = std::max(rmsMax, rms);
            setp(BTN, "0");                          // release (it was already reset by process())
            if (e->getParameter(e, BTN) > 0.001f) ++btnStuck;
        }
        std::printf("   %d presses: %d distinct patches, button stuck %d times, UpdateDisplay calls so far %d\n", N, distinct, btnStuck, updates);
        std::printf("   drive %.1f..%.1f dB, output %.1f..%.1f dB | bits<16 %d%%, wobble on %d%%, split off %d%%\n", dMin, dMax, oMin, oMax, bitsLow * 100 / N, wobOnN * 100 / N, splitOffN * 100 / N);
        std::printf("   level for a 55 Hz tone: rms %.1f..%.1f dB, worst peak %.2f (%.1f dBFS)\n", rmsMin, rmsMax, peakAll, 20 * std::log10(peakAll + 1e-9));
        if (btnStuck) ++fails;
        if (distinct < N * 9 / 10) ++fails;
        if (peakAll > 4.01) ++fails;
    }
    std::printf("%s\n", fails ? "RESULT: FAIL" : "RESULT: PASS");
    return fails ? 1 : 0;
}
