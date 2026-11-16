# Nightshift Lofi — design spec

Four effect plugins (VST3, Windows) that give a track the sound of worn media, built on
the shared framework in `common/`. The owner is a producer who wants these to sound
premium, not gimmicky: every noise source faint and tasteful by default, every
saturation stage oversampled, nothing clicks when a knob moves, nothing aliases unless
aliasing is the point. Levels below are measured, not guessed — use `lofitool`.

## Shared rules

- Subclass `lofi::LofiProcessor`; implement `prepareDsp`, `processDsp`, `getDspLatency`.
  The base handles mix (with latency compensation), output trim, bypass, meters, state.
- Parameters are declared in the plugin's `createLayout()`; the IDs there are the
  contract with the editor and with `lofitool --set`. Do not rename them.
- Deterministic: seed every noise source in `prepareDsp` so two instances render the
  same output (`lofitool --validate` checks this). No `rand()`, no `juce::Random` on
  the audio thread; use `lofi::dsp::Noise`.
- Sound a plugin makes up itself (Cassette's hiss, Vinyl's static and crackle) follows the
  host's transport: multiply it by `transportGateAt (sample)`, which glides to 0 over
  0.1 s when the host stops and back when it plays, so a stopped song is silent. The
  processing of the input carries on (monitoring through Vinyl still sounds like vinyl).
  Hosts that report no transport get the noise throughout; `lofitool` reports one that
  is always playing, as a DAW's is while it renders.
- No allocation on the audio thread after `prepareDsp`. Size delay lines for 96 kHz.
- Smooth every parameter that touches gain, cutoff or delay time
  (`lofi::dsp::Smoothed`, 20–50 ms). `--validate` sweeps each parameter on a −12 dBFS
  sine and compares every eighth of the sweep with the plugin held still at both ends of
  it: moving may step at most 1.5× the at-rest step + 0.02 ("no parameter clicks while
  swept"). The raw largest step is reported too, but it includes the waveform's own slope
  (Output at +12 dB, a 4 kHz staircase, hiss at full), which smoothing cannot change.
- Override `resetDsp()`: clear delay lines, oversamplers and filter states and snap
  smoothers, without allocating. The base calls it on the audio thread when the plugin
  leaves bypass, then fades the wet path in; without it, audio from before the bypass is
  replayed. Bypass itself fades out over 50 ms and passes the input through delayed by the
  reported latency (`--validate` checks both, plus that toggling does not click).
- `lofi::dsp::ModDelay::read (d)` delays by exactly `d` samples (`read (1)` is the sample
  before the one just pushed). The latency a plugin reports must equal what an impulse
  measures: render `impulse.wav` and check where the wet peak lands.
- Mono and stereo both work; use the spec's channel count.
- Oversample saturation (`juce::dsp::Oversampling`, 2× or 4×) and report its latency.
- Block-size invariant: processing 64-sample blocks must equal 512-sample blocks.
- The editor is 820×460, set by `setSize (820, 460)` as the LAST line of the editor's
  constructor (never earlier: layout calls the pure virtual `layoutBody`). Keep the shared
  header and footer; the body is yours: section titles via `drawSectionTitle`, knobs via
  `addKnob`, and the hardware graphic on the left animated from `animate()`. Hybrid style:
  Nightshift palette with real-hardware touches (reels, platter, display). Look at a
  snapshot before calling it done.
- The window shows that design a quarter bigger: `createEditor` returns
  `new lofi::ScaledEditor (*this, new XEditor (*this))` (`common/ScaledEditor.h`, 1025×575). So that
  every word reads (the user, 2026-11-15: "make all of the text more readable"), in design sizes:
  the spaced-out small capitals at least 11.5 (14.4 px shown), every other word at least 12 (15 px),
  full strength (4.5 to 1 against what's behind it; a greyed-out control's words 3 to 1). Check with
  `"Nightshift X.exe" --snapshot gui.png --textaudit`: it lists any word that's too small, squeezed,
  cut short, clipped or faint, and should list none. (The vinyl's printed record label is art, drawn
  as paths, and the check doesn't see it.)
- Defaults are "instant good": loading the plugin on a beat with every knob at default
  must already sound like the record, subtle and musical.

## Cassette — tape machine

Chain: input → tape-type pre-emphasis EQ → saturation (4× oversampled, tape curve with
a little even-harmonic bias) → de-emphasis + head bump (low shelf) + age (HF roll-off)
→ wow & flutter (modulated fractional delay, L/R decorrelated by `width`) → dropouts
(brief random level dips with extra HF loss) → + hiss → out.

| Param | Range | Default | Meaning |
|---|---|---|---|
| type | I / II / IV | II | I: softer, duller, more hiss; II: balanced; IV: brighter, harder, less hiss |
| drive | 0–100 % | 35 | input gain into the saturator, 0 → −6 dB, 100 → +18 dB; compensate output so loudness stays roughly level |
| age | 0–100 % | 30 | HF loss: lowpass from 20 kHz (0) down to 5 kHz (100), gentle slope; plus slight dulling of transients |
| bump | 0–100 % | 40 | head bump: low shelf 0 → +5 dB around 70–100 Hz |
| wow | 0–100 % | 25 | slow pitch wander, 0.4–1.2 Hz random-ish, up to ±25 cents at 100; default about ±5 cents |
| flutter | 0–100 % | 20 | fast wander, 6–12 Hz, up to ±8 cents at 100; default about ±1.5 cents |
| width | 0–100 % | 50 | how different L and R wobble are (0 = identical, 100 = independent) |
| hiss | −90…−30 dB | −58 | tape hiss RMS, pink-ish, rising 2–8 kHz then rolling off, type-dependent tilt |
| dropouts | 0–100 % | 10 | rate/depth of brief (30–200 ms) dips, −3 to −18 dB with HF loss; 0 = none |

Must measure (48 kHz, `lofitool`):
- Drive 0, age 0, bump 0, wow/flutter/dropouts 0, hiss −90: 1 kHz −12 dBFS sine, THD < 0.5 %, level within ±0.5 dB of input.
- Drive 35 (default): THD 1–6 % on a 1 kHz −12 dBFS sine, even harmonics present (even ≥ 0.2 %).
- Drive 100: THD 8–30 %, no hard clipping artefacts (no sample-to-sample jump > 0.3).
- Default wow+flutter on sine1k: `--pitch` deviation peak 3–10 cents, modulation rate 0.4–1.5 Hz; with wow 0, flutter 100: rate 5–14 Hz.
- Hiss at default on silence: output RMS −58 ± 2 dBFS; spectrum centroid 2–7 kHz.
- Age 100 on sweep: level at 10 kHz at least 10 dB below 1 kHz.
- Bump 100 on sweep: level at 80 Hz 4–6 dB above 1 kHz.
- `--validate` all PASS.

GUI: cassette shell with two spinning reels whose speed follows the transport wobble
(`getTransportWobble`), tape visible between them, a label strip showing the tape type,
VU meters in the header, sections "Tape", "Motion", "Noise", "Out".

## Vinyl — turntable

Chain: input → wear EQ (low cut 30–80 Hz, HF loss 20 kHz → 6 kHz, gentle 2–4 kHz lift,
light odd-harmonic grit, all scaled by `wear`) → warp (pitch wobble at the platter rate:
0.555 Hz at 33, 0.75 Hz at 45, plus a weaker 2× harmonic, L and R identical) → width
(stereo narrowing toward mono, slight L/R crosstalk) → + surface noise (`noise`) +
crackle (`crackle`) → out. Noise and crackle are added after the wet path so `mix` fades
them too, and they continue over silence (a record keeps turning).

| Param | Range | Default | Meaning |
|---|---|---|---|
| speed | 33 / 45 | 33 | platter rate for warp and the editor's animation |
| noise | −90…−30 dB | −60 | surface static RMS, band-limited 500 Hz – 8 kHz, pink-ish; −60 is the owner's settled "barely there" level |
| crackle | 0–100 % | 30 | pop density, ~0.5 (10 %) to ~25 pops/s (100 %); pop peaks no more than 12 dB above the noise RMS, each a 1–4 ms filtered click, random polarity |
| wear | 0–100 % | 35 | the worn-record EQ and grit described above |
| warp | 0–100 % | 15 | ±0 to ±30 cents; default about ±4 cents |
| width | 0–100 % | 70 | 100 = untouched stereo, 0 = mono |

Must measure:
- Default on silence: output RMS −60 ± 2 dBFS; peaks ≤ −48 dBFS; between 2 and 10 pops/s (count onsets).
- Noise −90, crackle 0, wear 0, warp 0, width 100: sine1k THD < 0.3 %, level ±0.5 dB.
- Default warp on sine1k at 33: `--pitch` modulation rate 0.45–0.65 Hz, peak deviation 2–8 cents; at 45: 0.65–0.85 Hz.
- Wear 100 on sweep: 10 kHz at least 8 dB below 1 kHz; 50 Hz at least 6 dB below 1 kHz.
- Width 0 on a stereo file with different L/R: output L == R.
- `--validate` all PASS.

GUI: platter with a record spinning at the chosen rpm (`getPlatterWobble` scales it),
tonearm over the groove, a subtle dust speck or two, sections "Noise", "Record", "Out".

## Crush — 12-bit sampler

Chain: input → drive (0 → 0 dB, 100 → +18 dB) → converter: sample-rate reduction to
`rate` (`alias` 100 = plain sample-and-hold with full aliasing; 0 = anti-aliased
decimation and reconstruction) → bit reduction to `bits` with truncation (no dither; the
grit comes from the error) → output filter (resonant 2-pole lowpass at `filter`,
`resonance` 0–100 → Q 0.7–6) → out. `mode` presets: SP-1200 = 26.04 kHz, 12-bit, alias
100, filter 12 kHz; MPC60 = 40 kHz, 12-bit, alias 60, filter 16 kHz; Custom = knobs as
set. Changing mode sets the knobs (via the parameters, so the editor follows).

| Param | Range | Default |
|---|---|---|
| mode | SP-1200 / MPC60 / Custom | SP-1200 |
| bits | 4–16 | 12 |
| rate | 4–48 kHz | 26.04 kHz |
| drive | 0–100 % | 20 |
| alias | 0–100 % | 100 |
| filter | 200 Hz – 20 kHz | 12 kHz |
| resonance | 0–100 % | 20 |

Must measure:
- bits 16, rate 48000, alias 0, drive 0, filter 20 kHz, resonance 0: sine1k THD+N < 0.05 %.
- bits 12, rate 48000: sine1k THD+N between 0.01 % and 0.1 % (12-bit error, about −74 dB).
- bits 8, rate 48000: THD+N 0.2–2 %.
- rate 26040, alias 100, filter 20 kHz, on sine5k: spectrum shows an alias component near 21.04 kHz within 20 dB of the fundamental (use `--bands` / `--response`); with alias 0 that component is at least 40 dB down. (The SP-1200 preset's own 12 kHz filter takes the image another ~8 dB down, so this line opens the filter to test the converter alone.)
- resonance 100, filter 1 kHz on noise: `--response` shows a peak at 1 kHz at least 8 dB above 4 kHz.
- `--validate` all PASS.

GUI: sampler faceplate: a dark display with bits and rate in a seven-segment style,
an LED input meter driven by `inputLevel`, pads along the bottom that light with the
signal, sections "Converter", "Filter", "Out".

## Haze — chorus + spring reverb

Chain: input → ensemble chorus (`voices` modulated delays 6–22 ms, LFO `rate` with
per-voice phase offsets and a touch of random wander, `depth` 0 → 0, 100 → ±5 ms; dry +
voices mixed by `chorusMix`; `width` spreads voices across L/R) → spring reverb: a
feedback loop with a chain of dispersive allpass sections (the "boing"), `size` sets the
loop length and feedback for T60 0.6 s (0) to 4 s (100), `tone` sets loop damping
(dark to bright), `drip` sets allpass dispersion depth; mixed by `springMix`) → out.

| Param | Range | Default |
|---|---|---|
| rate | 0.1–5 Hz | 0.6 |
| depth | 0–100 % | 40 |
| voices | 1–3 | 2 |
| chorusMix | 0–100 % | 50 |
| size | 0–100 % | 50 |
| tone | 0–100 % | 50 |
| drip | 0–100 % | 40 |
| springMix | 0–100 % | 35 |
| width | 0–100 % | 100 |

Must measure:
- chorusMix 100, springMix 0, depth 100, rate 1 Hz on sine1k: `--pitch` modulation rate 0.8–1.2 Hz, peak deviation ≥ 10 cents.
- springMix 100, chorusMix 0 on impulse.wav: `--decay` T60 within 30 % of 0.6 s (size 0), 2 s (size 50), 4 s (size 100); late centroid lower than early centroid (it darkens).
- drip 100 vs 0 on the impulse: drip 100 adds a falling chirp (the spring "pew": dispersion sends the highs first and smears the lows later), so the centroid of the first 5 ms after arrival is well above that of 15–50 ms; drip 0 shows much less of a drop.
- chorusMix 0, springMix 0: sine1k passes within ±0.5 dB, THD < 0.1 %.
- `--validate` all PASS; tail reported ≥ 4 s.

GUI: a spring tank window with two or three springs that shimmer with the LFO
(`getLfoPosition`), sections "Chorus", "Spring", "Out".

## Verification routine (each plugin)

```
build.cmd Release "NightshiftX_VST3 NightshiftX_Standalone" <sub>   # your own build folder, see README
lofitool --validate <X.vst3>
lofitool --render <X.vst3> signals\sine1k.wav out.wav --set Drive=35 ; lofitool --measure out.wav --thd 1000
lofitool --render <X.vst3> signals\noise.wav out.wav --set Age=100 ; lofitool --measure out.wav --response --ref signals\noise.wav
...each "must measure" line above...
"Nightshift X.exe" --snapshot gui.png          # the Standalone build; then look at the PNG
```

Frequency-response lines ("10 kHz at least 10 dB below 1 kHz") are read from `--response --ref`
with the dry file as the reference: process `noise.wav` (flat) or `sweep.wav`, and compare the
processed file to the dry one. Without `--ref` the numbers carry the source's own spectrum.

Report the numbers. If a number misses its window, change the DSP, not the window.
