# Nightshift Bass — design spec

Two bass distortion plugins (VST3, Windows) in the Nightshift family, built on the shared
framework in `common/`. The owner plays bass and guitar through a MiniFuse 2 at a 32-sample
ASIO buffer, so these have to feel immediate: low latency, low CPU, no clicks. The designs
follow the two classic modern bass-drive layouts (a clean-blend overdrive, and a multiband
drive with a clean low band); the circuits are modelled from first principles, and the names,
look and voicings are our own. Levels below are measured, not guessed — use `basstool`.

## Shared rules

Everything in the Nightshift Lofi spec's shared rules applies (subclass `BassProcessor`,
parameter IDs are a contract, deterministic, no allocation on the audio thread, smooth every
gain/cutoff, `resetDsp()` clears state, block-size invariant, setSize last, the window a quarter bigger
through `bass::ScaledEditor` with every word readable at that size: design sizes 11.5 for the small capitals and 12
for the rest, full strength, `--snapshot gui.png --textaudit` listing nothing). On top:

- **Oversampling**: clipping runs at 4x through JUCE's FIR equiripple halfbands with integer
  latency (`getDspLatency()` returns it). Every path that is summed with the clipped one
  (Drive's clean path; all three of Split's bands) runs at the same latency, so blends and
  band sums never comb.
- **Clippers** (`dsp::Clipper`): three curves with closed-form antiderivatives, run with
  first-order ADAA on top of the 4x, crossfading over 20 ms when the character changes.
  Every curve has unity small-signal gain (drive 0 is nearly transparent; switching
  character never jumps in level) and saturates around an average of 1.
  - *Smooth*: `tanh(x/s + 0.25) - tanh(0.25)`, s = its slope at 0 — soft, asymmetric
    (+0.76 / −1.24), even harmonics.
  - *Tight*: cubic soft clip of x/1.5, flat at ±1 from |x| ≥ 1.5 — firm knee.
  - *Fuzz*: hard ceiling at +0.75, soft (1.25·tanh(x/1.25)) at −1.25 — bright, torn,
    strong even harmonics.
- **Makeup**: after a clipper, `sqrt(1 + (G r)^2) / G` for drive gain G and a reference level
  r (the level the band typically sits at), so turning drive up changes grit more than level.
- **Tone stage** (`common/BassTone`, both plugins, end of chain): six-band graphic EQ
  (60, 160, 400, 1k, 2.5k, 6k; peaking Q 1.1; +-12 dB) then a cabinet: Off, four built-in
  voicings (4x10, 8x10, 1x15, 2x12 — highpass box resonance, three peaks, 24 dB/oct top; zero
  latency; trims level-match each to Off on bassdi) or IR (a file the player loads,
  level-matched to 0 dB average over 40–250 Hz, where a bass line's energy is; plays as Off
  until one is loaded; its path is saved in the state). Cab changes crossfade over 50 ms.
- **Compressor**: peak detector with a 30 ms hold (longer than a low B's cycle, so held notes
  don't ripple) and 100 ms fall; 2 ms / 150 ms gain smoothing; auto makeup level-matched at a
  −12 dBFS peak.
- **Drive makeup** (Drive plugin): on top of the formula, up to +4 dB while the clipper is
  nearly linear (the grunt filter has removed the fundamentals and nothing has replaced them),
  fading out as it saturates.
- Editor sizes: Drive 960×540, Split 1000×620. Hybrid style as in the lofi suite.
- Defaults are "instant good": load it on a DI'd bass, every knob at default, and it already
  sounds like a finished bass tone.

## Drive — clean-blend bass overdrive

Chain: gate → compressor → split into **clean** (delayed by the oversampler latency) and
**drive** (drive gain → grunt highpass + shelf → attack shelf → 4x clipper → DC block → tone
lowpass → makeup → level) → blend → graphic EQ → cabinet.

| Param | Range | Default | Meaning |
|---|---|---|---|
| gate | Off (−90) … −30 dB | Off | threshold; closes 6 dB under, 40 ms hold, 1 ms open, ~60 ms close |
| comp | 0–100 % | 25 | one knob: threshold 0 → −36 dB, ratio 1:1 → 8:1, soft knee, auto makeup |
| drive | 0–100 % | 45 | +6 → +42 dB into the clipper |
| grunt | Thin / Raw / Fat | Raw | low end into the clipper, first-order: HP 320 Hz / HP 120 Hz / HP 30 Hz + 6 dB shelf at 110 Hz |
| attack | Cut / Flat / Boost | Flat | high shelf at 1.8 kHz into the clipper: −6 / 0 / +6 dB |
| character | Smooth / Tight / Fuzz | Tight | clipper curve |
| tone | 0–100 % | 55 | lowpass after the clipper, 1.2 → 14 kHz (log) |
| blend | 0–100 % | 60 | clean → drive, equal-power (cos / sin) |
| level | −12…+12 dB | 0 | drive path level |
| eq1..eq6, cab | | 0 dB, 4x10 | tone stage |

Must measure (48 kHz, `basstool`):
- Clean only (blend 0, comp 0, cab Off): sine1k THD < 0.01 %, level within ±0.1 dB; `--response`
  on noise30 vs dry flat within ±0.3 dB from 30 Hz to 16 kHz.
- Blend 100, cab Off, tone 100, drive 0 → 50 → 100 on sine82: THD rises monotonically; drive 100
  THD ≥ 20 %.
- Aliasing, blend 100, drive 100, tone 100, cab Off, character each of the three, on sine1490:
  worst non-harmonic spur up to 20 kHz ≤ −60 dB relative to the fundamental. (`--alias` looks
  at the audible band: the downsampler's 20–24 kHz transition band folds a trace of the
  harmonics just above Nyquist to just below it, where no one hears it.)
- Loudness, blend 100, cab Off, on bassdi: RMS at drive 0, 50, 100 within 4 dB of each other.
- Blend 60 (comp 0, cab Off) on bassdi: RMS no more than 2 dB under the louder of blend 0 and
  blend 100 — the two paths must not cancel. (A second-order grunt highpass turned the drive
  path's fundamentals ~120° and blend 60 came out quieter than either path; the grunt filters
  are first-order for that reason.)
- Defaults on bassdi: RMS within ±2 dB of the dry input, sub-60 Hz share of energy ≥ 20 %.
- Grunt, blend 100, drive 60, cab Off, on noise30 (`--response --ref`): Fat at 60 Hz at least
  10 dB above Thin at 60 Hz.
- Attack, same settings: Boost at 4 kHz at least 6 dB above Cut at 4 kHz.
- Comp 100 vs 0 (blend 0, cab Off) on bassdi: `--dynamics` range (95th minus 10th percentile of
  50 ms RMS windows) at least 3 dB narrower, overall RMS within ±3 dB. (Sample-peak crest factor
  is the wrong yardstick: like any compressor with a few ms of attack, it lets the first
  milliseconds of a pluck through, by design.)
- Gate −50 dB on noise at −70 dBFS: output below −100 dBFS after the close; gate Off passes it.
- Latency: blend 0, comp 0, cab Off on impulse.wav: the peak lands exactly `latency` samples late.
- Each EQ band at +12 (everything else flat, blend 0, cab Off): response at the band's centre
  +11…+13 dB.
- Each cab voicing on noise30: 8 kHz at least 20 dB under 1 kHz; on bassdi, RMS within ±2 dB of
  cab Off.
- CPU at 32-sample blocks: under 5 % of one core; latency under 1 ms at 48 kHz.
- `--validate` all PASS.

GUI: a bass preamp pedal, our own design: a matte midnight-graphite enclosure with a lit
die-cast rim, warm cream silkscreen, amber for what's lit, and our own name and lettering (a
crescent and NIGHTSHIFT over BASS DRIVE in heavy capitals between amber rules). Two rows of
hardware knobs (knurled black caps over printed scales), Level and Blend over Drive and Tone;
Grunt, Attack and Character as three-way bat toggles stacked between the two columns, levers in
one line, the lever up being the last choice (Fat, Boost, Fuzz) and the chosen one lit; an
amber LED that brightens with the drive, over a stomp footswitch (bypass); IN and OUT printed
at the sides. No other maker's names, marks, artwork, lettering or colour scheme. Right: the
back panel, in the same metal and with the same knobs: Dynamics (gate, comp, GR meter, gate
lamp) and Out (mix, output); the graphic EQ with its live response; the cabinet keys, LOAD IR,
and the cab's curve. Shared widgets: `Knob::setPedalStyle`, `MiniToggle`, `Footswitch`.

## Split — multiband bass drive

Chain: gate → 4x → LR4 three-way split (low = LP(f1)·AP(f2), mid = HP(f1)·LP(f2),
high = HP(f1)·HP(f2); sums to an allpass) → low: compressor, level; mid: drive → clipper →
DC block → makeup → level; high: drive → clipper → DC block → makeup → level → sum → 1x →
graphic EQ → cabinet.

| Param | Range | Default | Meaning |
|---|---|---|---|
| gate | Off (−90) … −30 dB | Off | as Drive |
| lowCross | 60–400 Hz | 160 | low/mid crossover |
| highCross | 600–5000 Hz | 1600 | mid/high crossover |
| lowComp | 0–100 % | 40 | compressor on the clean low band |
| lowLevel | Off (−40) … +12 dB | 0 | |
| midDrive | 0–100 % | 50 | 0 → +36 dB into the mid clipper (reference 0.15) |
| midLevel | Off … +12 dB | 0 | |
| highDrive | 0–100 % | 40 | 0 → +40 dB into the high clipper (reference 0.06) |
| highLevel | Off … +12 dB | −3 | |
| character | Smooth / Tight / Fuzz | Tight | both clippers |
| eq1..eq6, cab | | 0 dB, 4x10 | tone stage |

Must measure:
- Bands sum flat: lowComp 0, midDrive 0, highDrive 0, all levels 0, cab Off, on noise30 vs dry:
  flat within ±0.3 dB from 30 Hz to 16 kHz (the crossovers are allpass in sum).
- Low band clean: mid and high Off, lowComp 0, cab Off, on sine41: THD < 0.05 %.
- Low band alone on noise30: 60 Hz within ±1 dB; at 4× lowCross (640 Hz) at least 24 dB down.
- High band alone on noise30 (low, mid Off, highDrive 0): 120 Hz at least 30 dB under 4 kHz.
- Mid drive 100 (low, high Off, cab Off) on sine440: THD ≥ 15 %.
- Aliasing: low Off, mid and high drive 100, cab Off, each character, on sine1490: worst spur
  ≤ −60 dB.
- Loudness on bassdi: midDrive 0 / 50 / 100 (others default, cab Off): RMS within 4 dB.
- Defaults on bassdi: RMS within ±2 dB of the dry input.
- Latency: drives 0, comp 0, cab Off on impulse.wav: peak lands at `latency` ± 1 samples.
- CPU at 32-sample blocks: under 8 % of one core; latency under 1 ms.
- `--validate` all PASS.

GUI: a band display across the top (the three LR4 bands on a log axis, scaled by their level,
lit by their signal, two crossover handles to drag) beside Input (gate, character); Low / Mid
/ High panels; then the graphic EQ, cabinet, and Out.

## Verification routine

```
build.cmd Release
basstool --signals testdata\signals
basstool --validate <X.vst3>
basstool --render <X.vst3> testdata\signals\sine82.wav out.wav --set Blend=100 --set Cab=Off ; basstool --measure out.wav --thd 82.4
basstool --render <X.vst3> testdata\signals\noise30.wav out.wav ... ; basstool --measure out.wav --response --ref testdata\signals\noise30.wav
basstool --measure out.wav --alias 1490
basstool --cpu <X.vst3> --block 32
"Nightshift Bass X.exe" --snapshot gui.png            # then look at it
"Nightshift Bass X.exe" --render in.wav out.wav --ir cab.wav
```

Report the numbers. If a number misses its window, change the DSP, not the window.
