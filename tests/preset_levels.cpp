// Measures the output level of every built-in program for a 55 Hz bass tone (decaying like an 808).
#include "mpc_vst2_abi.h"
#include <dlfcn.h>
#include <cmath>
#include <cstdio>
#include <vector>
#include <string>

static VstIntPtr host(AEffect*, VstInt32 op, VstInt32, VstIntPtr, void*, float) { return op == audioMasterVersion ? 2400 : 0; }
static const double SR = 44100.0;

int main(int argc, char** argv)
{
    void* h = dlopen(argv[1], RTLD_NOW);
    auto entry = (AEffect* (*)(audioMasterCallback))dlsym(h, "VSTPluginMain");
    AEffect* e = entry(host);
    e->dispatcher(e, effOpen, 0, 0, nullptr, 0);
    e->dispatcher(e, effSetSampleRate, 0, 0, nullptr, (float)SR);
    e->dispatcher(e, effSetBlockSize, 0, 128, nullptr, 0);
    int n = e->numPrograms;
    std::printf("%-3s %-18s %8s %8s %12s\n", "#", "program", "rms dB", "peak dB", "55Hz share");
    for (int p = 0; p < n; ++p)
    {
        e->dispatcher(e, effSetProgram, 0, p, nullptr, 0);
        char name[64] = {0};
        e->dispatcher(e, effGetProgramName, 0, 0, name, 0);
        e->dispatcher(e, effMainsChanged, 0, 1, nullptr, 0);
        const int block = 128, total = (int)(1.5 * SR);
        std::vector<float> l(block), r(block), out;
        float* bufs[2] = {l.data(), r.data()};
        for (int s = 0; s < total; s += block)
        {
            for (int i = 0; i < block; ++i)
            {
                double t = (s + i) / SR;
                float x = (float)(0.5 * std::exp(-t * 1.5) * std::sin(2 * M_PI * 55.0 * t));
                l[i] = r[i] = x;
            }
            e->processReplacing(e, bufs, bufs, block);
            for (int i = 0; i < block; ++i) out.push_back(l[i]);
        }
        size_t a = (size_t)(0.1 * SR), b = out.size();
        double sum = 0, peak = 0;
        for (size_t i = a; i < b; ++i) { sum += (double)out[i] * out[i]; peak = std::max(peak, (double)std::fabs(out[i])); }
        double rms = std::sqrt(sum / (double)(b - a));
        // energy at 55 Hz relative to total
        double w = 2 * M_PI * 55.0 / SR, c = 2 * std::cos(w), s1 = 0, s2 = 0;
        for (size_t i = a; i < b; ++i) { double v = out[i] + c * s1 - s2; s2 = s1; s1 = v; }
        double pw = s1 * s1 + s2 * s2 - c * s1 * s2;
        double amp = std::sqrt(std::max(0.0, pw)) * 2.0 / (double)(b - a);
        double share = (amp * amp / 2.0) / std::max(sum / (double)(b - a), 1e-12);
        std::printf("%-3d %-18s %8.1f %8.1f %11.0f%%\n", p, name, 20 * std::log10(rms + 1e-9), 20 * std::log10(peak + 1e-9), share * 100);
    }
    return 0;
}
