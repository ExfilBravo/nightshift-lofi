# Nightshift Lofi

**Download:** these plugins come with NightShift's free installers for Windows and Mac, on the
[NightShift releases page](https://github.com/ExfilBravo/nightshift/releases/latest).
Free software under the GNU GPL v3 or later ([LICENSE](LICENSE)).

Four lofi effect plugins (VST3, Windows) in the Nightshift family, built on JUCE under the GPLv3:

| Plugin | What it does |
| --- | --- |
| **Nightshift Cassette** | tape saturation, head bump, age, wow & flutter, hiss, dropouts |
| **Nightshift Vinyl** | surface noise and crackle, worn-record EQ, 33/45 warp, stereo narrowing |
| **Nightshift Crush** | 12-bit sampler converters (SP-1200 / MPC60), aliasing, the SP's output filter |
| **Nightshift Haze** | ensemble chorus into a spring reverb |

The design, parameters and the measurements each plugin must meet are in [SPEC.md](SPEC.md).

## Building

Needs Visual Studio 2022 Build Tools (C++ workload with CMake) and the Nightshift checkout next door,
whose JUCE this project uses (`-DJUCE_DIR=...` to point elsewhere).

```bash
build.cmd Release
```

Builds every plugin and `lofitool`, and copies each `.vst3` to `%LOCALAPPDATA%\Programs\Common\VST3`
(no admin rights needed; add that folder in FL Studio's plugin manager, or in Nightshift's scanner).

To build one plugin in its own build tree, so two builds can run side by side:

```bash
build.cmd Release NightshiftCassette_VST3 cassette
```

Outputs land in `build\<tree>\<Target>_artefacts\Release\VST3\<Name>.vst3` and
`...\Standalone\<Name>.exe`.

## Checking a plugin without ears

Cassette's hiss and Vinyl's surface noise and crackle play only while the host's transport does:
they fade out over a tenth of a second when it stops and back in when it plays.

`lofitool` loads the built VST3 like a DAW would, with its transport playing:

```bash
lofitool --signals testdata\signals
lofitool --params "build\Release\NightshiftCassette_artefacts\Release\VST3\Nightshift Cassette.vst3"
lofitool --render <plugin.vst3> testdata\signals\sine1k.wav out.wav --set Drive=60 --set Hiss=-90
lofitool --measure out.wav --thd 1000
lofitool --validate <plugin.vst3>
```

`--validate` checks loading at 44.1/48/96 kHz, determinism, block-size invariance, bypass
(latency-aligned, click-free toggling), state round-trip, parameter sweeps (each eighth of a sweep
against the plugin held still there, so only real clicks fail) and the noise floor. `--measure` reports level, band balance, pitch
modulation (for wow/flutter/chorus), harmonic distortion and decay time.

Each plugin's standalone exe takes `--snapshot gui.png [--set Name=value]` to capture its window
offscreen; without arguments it opens as a normal app with audio.

Like Nightshift.exe, these are GUI-subsystem programs: from PowerShell use `Start-Process -Wait` or pipe
to `Out-String` so the shell waits for them.

## Layout

- `common/` – look-and-feel, meters and faceplate graphics; DSP building blocks; the processor and
  editor base classes every plugin shares (mix, output, bypass, meters, state).
- `plugins/<name>/` – one folder per plugin: processor and editor.
- `tools/lofitool/` – the host harness and measurements.
- `tools/standalone/` – the custom standalone app with the snapshot mode.
- `testdata/` – test signals (generated) and dry musical material rendered from Nightshift.
