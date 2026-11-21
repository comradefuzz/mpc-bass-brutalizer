# Feature tests

Small stand-alone harnesses that load the built native plugin (`build/native/bass-brutalizer.dylib`)
through its VST2 interface and check behaviour the SDK's `host_sim` does not cover.

```
c++ -std=c++17 -O1 -I <SDK>/framework tests/<name>.cpp -o /tmp/<name>
/tmp/<name> "build/native/bass-brutalizer.dylib"
```

| File | Checks |
|---|---|
| `feat_test.cpp` | Oct Sub makes an octave-down tone, wobble sweeps the level, tempo sync and extreme settings stay stable |
| `mode_switch.cpp` | silent input stays silent while Mode / Bias / Multiband change |
| `red_test.cpp` | Red Button: 300 presses per Sanity level give distinct patches, finite bounded output, button resets itself |
| `preset_levels.cpp` | RMS / peak of every built-in program on a 55 Hz tone (used to balance preset Output levels) |
| `vowel_test.cpp` | vowel stage: steps move the formants (OO vs AH, EE vs OO), Gender, mouth follow, Mix 0 transparent, silence stays silent, level per character, extremes stay bounded |
