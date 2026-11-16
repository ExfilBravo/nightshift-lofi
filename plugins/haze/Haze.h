#pragma once

#include <JuceHeader.h>
#include "common/LofiDsp.h"
#include "common/LofiEditor.h"
#include "common/LofiParams.h"
#include "common/LofiProcessor.h"
#include "HazeDsp.h"

/** Nightshift Haze: a lofi ensemble chorus into a dusty spring reverb. See SPEC.md. */
class HazeProcessor : public lofi::LofiProcessor
{
public:
    HazeProcessor();
    juce::AudioProcessorEditor* createEditor() override;

    /** Current chorus LFO position (-1..1) for the editor's animation. */
    float getLfoPosition() const        { return lfoPosition.load(); }

    /** Each voice's modulation (-1..1) and how present it is (0..1, follows `voices`). */
    float getVoicePosition (int v) const    { return voicePosition[juce::jlimit (0, maxVoices - 1, v)].load(); }
    float getVoiceLevel (int v) const       { return voiceLevel[juce::jlimit (0, maxVoices - 1, v)].load(); }

    /** Peak of the wet spring signal since the last call, as gain, so the tank art can twitch. */
    float getSpringLevel()              { return springLevel.exchange (0.0f); }

    /** Settings the editor's displays follow, read lock-free from the cached parameters. */
    int getVoiceCount() const           { return juce::jlimit (1, maxVoices, juce::roundToInt (voicesParam->load())); }
    float getSize() const               { return sizeParam->load() * 0.01f; }
    float getWidth() const              { return widthParam->load() * 0.01f; }

    /** Where voice v sits in the stereo field, -1 (L) .. 1 (R), for `voices` and `width`. */
    static float voicePanFor (int v, int voices, float width)
    {
        static constexpr float basePan[maxVoices] { -1.0f, 1.0f, 0.0f };
        return voices > 1 ? basePan[juce::jlimit (0, maxVoices - 1, v)] * width : 0.0f;
    }

    static constexpr int maxVoices = haze::ChorusLfo::maxVoices;

protected:
    void prepareDsp (const juce::dsp::ProcessSpec&) override;
    void processDsp (juce::AudioBuffer<float>&) override;
    void resetDsp() override;

private:
    static Layout createLayout();

    /** Reads the parameters into every ramp's target; once per block. */
    void updateTargets();

    /** Lands every ramp on its target, so the next sample is already at the setting. */
    void snapSmoothers();

    static constexpr float voiceBaseMs[maxVoices] { 11.0f, 17.0f, 14.0f };     // each voice sweeps +-5 ms inside 6..22 ms

    /** The parameters, looked up once: reading them by ID on the audio thread would build
        a juce::String (a heap allocation) per read. */
    std::atomic<float>* rateParam = nullptr;
    std::atomic<float>* depthParam = nullptr;
    std::atomic<float>* voicesParam = nullptr;
    std::atomic<float>* chorusMixParam = nullptr;
    std::atomic<float>* sizeParam = nullptr;
    std::atomic<float>* toneParam = nullptr;
    std::atomic<float>* dripParam = nullptr;
    std::atomic<float>* springMixParam = nullptr;
    std::atomic<float>* widthParam = nullptr;

    std::atomic<float> lfoPosition { 0.0f }, springLevel { 0.0f };
    std::atomic<float> voicePosition[maxVoices] {}, voiceLevel[maxVoices] {};

    int numChannels = 2;
    double sampleRate = 48000.0;
    bool snapPending = true;

    haze::ChorusLfo lfo;
    lofi::dsp::ModDelay chorusDelay[2];
    lofi::dsp::OnePole chorusSplit[2];                  // the bass stays dry and solid; the voices get the rest
    haze::Highpass2 voiceFeed[2];                       // and none of it leaks into the voices either
    lofi::dsp::OnePole voiceTone[2];                    // a bucket-brigade's gentle top end on the voices
    haze::Spring springs[2][2];
    lofi::dsp::OnePole springInLow[2], springInHigh[2];

    lofi::dsp::Smoothed depthMs, chorusMix, springMix, springTrim;
    lofi::dsp::Smoothed voiceGain[maxVoices], voicePan[maxVoices];

    /** Fades the signal into the empty delay lines after a reset, so the first sample
        to reach a read head does not arrive as a step. 1 in steady state. */
    lofi::dsp::Smoothed feedIn;
};

//==============================================================================
/** The voice count as a row of three latching keys with LEDs, as on an ensemble chorus,
    bound to the `voices` parameter. Same caption and value rows as a Knob, so it sits in
    the knob grid. Click or drag across the keys. */
class VoiceSwitch : public juce::Component,
                    public juce::SettableTooltipClient
{
public:
    VoiceSwitch (juce::AudioProcessorValueTreeState&, const juce::String& paramId);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent& e) override     { mouseDown (e); }
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> keyBounds (int index) const;
    int keyAt (juce::Point<float>) const;

    juce::RangedAudioParameter& parameter;
    juce::ParameterAttachment attachment;
    int current = 2;
};

//==============================================================================
class HazeEditor : public lofi::LofiEditor
{
public:
    explicit HazeEditor (HazeProcessor&);

protected:
    void layoutBody (juce::Rectangle<int>) override;
    void paintBody (juce::Graphics&, juce::Rectangle<int>) override;
    void animate (float seconds) override;

private:
    void paintTank (juce::Graphics&, juce::Rectangle<int> area);
    void paintRail (juce::Graphics&, juce::Rectangle<float> rail, const float* canX);
    void paintSpring (juce::Graphics&, float centreX, float top, float bottom, int index);
    void paintField (juce::Graphics&);
    void paintLfoLamp (juce::Graphics&);

    HazeProcessor& haze;
    VoiceSwitch voiceSwitch;

    /** Body rows, the tank graphic, the Out row's stereo-field readout, and the rate LED
        (its glow included, so repainting it leaves no stale edge). */
    juce::Rectangle<int> graphic, chorusRow, springRow, outRow, field, lfoLamp;
    juce::Point<float> lampCentre;

    lofi::Knob* rateKnob = nullptr;
    lofi::Knob* spreadKnob = nullptr;
    std::vector<juce::Component*> chorusCells, springCells, outCells;
    float lfoSmooth = 0.0f, tankLevel = 0.0f, wobblePhase = 0.0f;
    float voiceSmooth[3] {}, voiceLit[3] {}, voicePanSmooth[3] {};
    bool spreadActive = true;
};
