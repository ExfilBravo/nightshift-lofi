#pragma once

#include <JuceHeader.h>
#include "common/LofiDsp.h"
#include "common/LofiEditor.h"
#include "common/LofiParams.h"
#include "common/LofiProcessor.h"
#include "CrushDsp.h"

class ModeSwitch;

/** Nightshift Crush: the 12-bit sampler sound. SP-1200 / MPC60 converters, sample-rate
    reduction with real aliasing, input drive into the converter, the SP's output filter.
    See SPEC.md.

    Chain: drive (smoothed) -> converter input clip (2x oversampled) -> sampler at `rate`
    (anti-alias lowpass morphed by `alias`) truncating to `bits` -> band-limited hold,
    crossfaded by `alias` with the samples' band-limited reconstruction -> DC block ->
    makeup -> resonant 2-pole output filter. Everything before the output filter is
    linear phase with a constant delay, which is the latency reported.
*/
class CrushProcessor : public lofi::LofiProcessor,
                       private juce::AudioProcessorValueTreeState::Listener,
                       private juce::AsyncUpdater,
                       private juce::Timer
{
public:
    CrushProcessor();
    ~CrushProcessor() override;
    juce::AudioProcessorEditor* createEditor() override;
    void setStateInformation (const void*, int) override;

    /** The converter presets behind `mode`; index 2 (Custom) has no preset. */
    struct Preset { int bits; float rate, alias, filter; };
    static Preset preset (int mode);
    static constexpr int customMode = 2;

protected:
    void prepareDsp (const juce::dsp::ProcessSpec&) override;
    void processDsp (juce::AudioBuffer<float>&) override;
    void resetDsp() override;
    int getDspLatency() const override  { return latency; }

private:
    static Layout createLayout();
    void updateTargets();
    void processChunk (juce::dsp::AudioBlock<float>);

    // Mode presets. A mode change sets the converter knobs, and a knob change that leaves
    // the preset flips the mode to Custom; both write parameters, so they happen on the
    // message thread (syncMode, from an async update when the change came from there,
    // else from a 30 Hz timer). The listener itself only touches atomics. Until the
    // message thread has written a new mode's knobs, the audio thread already runs on
    // that preset's values, so a mode change takes effect on the sample it arrives at.
    void parameterChanged (const juce::String& id, float value) override;
    void handleAsyncUpdate() override   { syncMode(); }
    void timerCallback() override       { syncMode(); }
    void syncMode();
    void setParameter (const juce::String& id, float value);
    bool knobsMatch (int mode) const;

    std::atomic<float>* pBits = nullptr;
    std::atomic<float>* pRate = nullptr;
    std::atomic<float>* pDrive = nullptr;
    std::atomic<float>* pAlias = nullptr;
    std::atomic<float>* pFilter = nullptr;
    std::atomic<float>* pResonance = nullptr;
    std::atomic<float>* pMode = nullptr;

    std::atomic<int> modeChanges { 0 }, modeChangesApplied { 0 };
    std::atomic<bool> knobsTouched { false }, restoringState { false };

    // DSP state
    double sampleRate = 48000.0;
    int maxBlock = 512, numChannels = 2, latency = 0;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampling;
    crush::Sampler sampler[2];
    crush::Quantiser quantiser;
    crush::MatchedLowpassCoefficients outCoefficients;
    crush::MatchedLowpass outFilter[2];
    lofi::dsp::DcBlocker dcBlocker[2];
    lofi::dsp::Smoothed driveGain, makeupGain, aliasAmount, rateHz, bitsValue, filterLog2, filterQ;
    float lastRate = -1.0f, lastBits = -1.0f, lastFc = -1.0f, lastQ = -1.0f;
};

//==============================================================================
/** Sampler faceplate on the left: seven-segment display with bits and rate, LED input
    meter, a scope of the converter's staircase, and eight pads that light with the
    signal's hits. Sections "Converter", "Filter", "Out" on the right, on one 4-column
    grid, with the mode as three latching LED buttons in the Converter title band. */
class CrushEditor : public lofi::LofiEditor
{
public:
    explicit CrushEditor (CrushProcessor&);
    ~CrushEditor() override;

protected:
    void layoutBody (juce::Rectangle<int>) override;
    void paintBody (juce::Graphics&, juce::Rectangle<int>) override;
    void animate (float seconds) override;

private:
    void paintFaceplate (juce::Graphics&, juce::Rectangle<float>);
    void paintDisplay (juce::Graphics&, juce::Rectangle<float>);
    void paintMeter (juce::Graphics&, juce::Rectangle<float>);
    void paintScope (juce::Graphics&, juce::Rectangle<float>);
    void paintPads (juce::Graphics&, juce::Rectangle<float>);
    static void drawSevenSegment (juce::Graphics&, juce::Rectangle<float> digitBox, int digit, bool dot,
                                  juce::Colour on, juce::Colour off);

    CrushProcessor& crush;
    juce::Rectangle<int> graphic, converterTitle, filterTitle, outTitle;
    std::vector<lofi::Knob*> converterKnobs, filterKnobs, outKnobs;
    std::unique_ptr<ModeSwitch> modeSwitch;

    // Animation state, advanced from animate()
    float heldInput = 0.0f;                         // the last non-empty meter read
    int emptyFrames = 0;
    float meterLevel = 0.0f, meterHold = 0.0f;
    int holdFrames = 0, clipFrames = 0;
    float fastEnv = 0.0f, slowEnv = 0.0f, sinceOnset = 1.0f, lastLevel = 0.0f;
    float padGlow[8] {};
    int activePad = -1;
    float scopePhase = 0.0f, signalLed = 0.0f;
};
