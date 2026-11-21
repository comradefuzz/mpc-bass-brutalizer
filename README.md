# Bass Brutalizer

by comrade fuzz. An insert effect for Akai MPC standalone (MPC OS 3.10.1+, External Plugin Support enabled).

Bass destruction. The signal is split at **Split**: the low band can stay clean (scaled by **Sub**) while the rest is driven into a waveshaper, ring-modulated, bitcrushed, swept by a wobble filter and tone-filtered. An octave-down generator can thicken the sub.

## Signal flow

```
dry --+--> lowpass @ Split --> low (clean sub) --- * Sub ----------------------+
      |     (Split On off or Split 0 -> low = 0)                               |
      +--> body = dry - low                                                    |
             -> Pre Tilt -> [Wobble Pre] -> Drive + Bias -> shaper (Mode)      |
             -> DC blocker -> Ring -> Bits / Decimate -> [Wobble Post]         |
             -> Tone -> level comp ----------------------------------------> (+)
dry --> octave-down generator --> * Oct Sub --------------------------------> (+)
out = dry + (wet * Output - dry) * Mix
```

## Pages and parameters

**MAIN**

| Name     | Range              | What it does |
|----------|--------------------|--------------|
| Drive    | 0..48 dB           | Gain into the waveshaper |
| Tone     | 500..20000 Hz      | 12 dB/oct lowpass after the distortion |
| Mix      | 0..100 %           | Dry/wet |
| Output   | -24..+12 dB        | Level of the processed signal (parameter 0, harmless on load) |
| Mode     | Soft/Hard/Fold/Rect | Soft = tanh, Hard = clipper, Fold = wavefolder, Rect = full-wave rectifier (octave-up fuzz) |
| Bias     | -50..+50 %         | Asymmetry before shaping: even harmonics |
| Pre Tilt | -12..+12 dB        | Before the shaper: + brightens the signal going in, - boosts its lows (changes what gets distorted most) |
| Split On | On/Off             | Enables the split chain. Off = the whole signal is distorted |
| Split    | 0..1000 Hz         | Crossover between clean sub and distorted body. 0 = Off |
| Sub      | 0..100 %           | Level of the clean low band |
| Oct Sub  | 0..100 %           | Octave-down generator: a square flip-flop on the zero crossings, rounded and following the input level. Works best on a single clean bass note; independent of Split |

**BAND**: Multiband mode (shapes mids and highs separately).

| Name      | Range          | What it does |
|-----------|----------------|--------------|
| Hi Split  | 1000..8000 Hz  | Mid/high crossover |
| Hi Drive  | -24..+24 dB    | Extra drive on the highs relative to Drive |
| Multiband | On/Off         | Off = one shaper for the whole body |

**CRUSH**

| Name     | Range        | What it does |
|----------|--------------|--------------|
| Bits     | 1..16        | Bit depth (16 = Off) |
| Decimate | 1..40x       | Sample-rate divider, aliasing on purpose (1 = Off) |
| Ring     | 0..100 %     | Ring modulation amount |
| Ring Hz  | 20..2000 Hz  | Ring modulator frequency |

**WOBBLE**: a resonant lowpass swept by an LFO.

| Name      | Range              | What it does |
|-----------|--------------------|--------------|
| Wob Rate  | 0.1..20 Hz         | LFO speed (used when Wob Sync = Free) |
| Wob Depth | 0..100 %           | Sweep depth, 100% = +-4 octaves |
| Wob Freq  | 40..8000 Hz        | Centre frequency of the filter |
| Wob Reso  | 0..100 %           | Resonance |
| Wob Pos   | Off/Pre/Post       | Pre = before the shaper (talking, formant-like), Post = after the crusher (classic dubstep wobble) |
| Wob Shape | Sine/Tri/Saw/Square | LFO shape |
| Wob Sync  | Free, 1/64 ... 2/1 | Free = Wob Rate in Hz, otherwise locked to the MPC tempo. Needs the MPC transport to report tempo (defaults to 120 bpm) |

## Programs (25)

808 and sub: Init, 808 Warm, 808 Crunch, 808 Octave, 808 Destroyed, 808 Dirty Crush, Sub Saturator.
Mid growl: Growl Mid, Reese Rot, Octave Fizz, Acid Bite, Talking Bass.
Dubstep wobble (tempo synced): Wobble 1/4, Wobble 1/8 Tri, Wobble Square, Triplet Wub, Dub Digital.
Metallic: Robot Ring, Metal Bass.
Extreme: Parallel Smash, Broken Speaker, Bright Fold, Chainsaw, Lo-Fi Grit, Wall Of Noise.

Tip: with Mix below 100% the dry signal still contains the sub, so "no clean sub" means Sub = 0 and Mix = 100%.

## Notes

- No oversampling: heavy drive, crushing and ring mod on bright material alias; lower Tone to tame it (or enjoy it).
- Split is a simple 12 dB/oct filter, not a phase-perfect crossover.
- High Wob Reso with Pre can get very loud; the wet signal is limited to +12 dBFS (a safety clamp, not a musical limiter). Lower Output if the next effect distorts.
- Tested in the SDK host simulator (20 checks, ~0.15% of a desktop core per block) and with a feature test: Oct Sub makes a clear octave-down tone, the wobble filter sweeps the level, tempo sync and extreme settings stay stable. Not yet tested on hardware by ear.
- Third-party plugins run unprotected inside the MPC application: save your project often.

---

## More sections added since the first version

- **Crush On, Ring On, Wobble On** switch their sections (CRUSH and WOBBLE pages).
- **RED BTN page**: a big red button replaces the sound settings with a random patch; **Sanity** (Sober, Drunkish, Bat Shit Crazy) limits how wild it may be and sets a safe Output.
- **Vowel stage** (parameters 30-44, `Vow ...`, Vowel A/B, Morph, Gender): formant filters that make vowels (OO OH UH AH AE EH EE), morphed by hand, by an LFO, by the input level (mouth follow) or by a four-step word (WOW, YAY, YO, YEAH, YOI, WAH, OOH-AAH, EOW, MAMA) in the MPC tempo; Choir, Whisper and Robot characters. The DSP is in; its UI pages and presets are being added.

## Building

This repository is a plugin project for the [MPC Plugin SDK](https://github.com/inmusic-software/mpc-plugin-builder-sdk) (MIT).

1. Clone the SDK and put this folder at `<SDK>/plugins/Bass Brutalizer`.
2. Apply the small framework patch (preset changes refresh the MPC screen; `NaN` in a preset means "leave this parameter"):
   `git -C <SDK> apply "plugins/Bass Brutalizer/sdk-patch/framework-mpc_plugin.patch"`
3. `python3 tools/build.py "plugins/Bass Brutalizer"` (needs Python 3.9+; the Zig cross compiler is fetched automatically).
4. The touch skin in `skins/` is generated: after changing `ui.json` or the parameters run
   `python3 "plugins/Bass Brutalizer/skin-src/make_retro_skin.py"` (add `--preview` for page mock-ups in `build/`) and build again.
5. Install: copy `build/comrade fuzz - VST - Bass Brutalizer/` into a `VST2 Plugins` folder on the MPC's drive. Needs MPC OS 3.10.1+ with Enhancements → External Plugin Support enabled.

Third-party plugins run unprotected inside the MPC application: save your project often.
