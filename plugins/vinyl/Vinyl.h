#pragma once

#include <JuceHeader.h>
#include "common/LofiDsp.h"
#include "common/LofiEditor.h"
#include "common/LofiParams.h"
#include "common/LofiProcessor.h"
#include "VinylDsp.h"

/** Nightshift Vinyl: surface noise and crackle at the user's faint levels, worn-record
    EQ and grit, 33/45 rpm warp, stereo narrowing. See SPEC.md.

    Chain: rumble cut + presence lift -> grit (2x oversampled) -> HF loss -> warp
    (platter-locked speed error integrated into a shared fractional delay) -> width with
    HF crosstalk -> + static (mid/side, following width) + crackle.
*/
class VinylProcessor : public lofi::LofiProcessor
{
public:
    VinylProcessor();
    juce::AudioProcessorEditor* createEditor() override;

    /** Platter speed modulation for the editor's animation, around 1.0. */
    float getPlatterWobble() const      { return platterWobble.load(); }

    /** Where the platter is in its revolution, 0..1, for the editor. */
    float getPlatterPhase() const       { return platterPhase.load(); }

protected:
    void prepareDsp (const juce::dsp::ProcessSpec&) override;
    void processDsp (juce::AudioBuffer<float>&) override;
    void resetDsp() override;
    int getDspLatency() const override  { return latency; }

private:
    static Layout createLayout();

    struct Channel
    {
        lofi::dsp::OnePole hp1;
        vinyl::Svf hp2, lift, lp;
        lofi::dsp::ModDelay delay;
    };

    void updateTargets();
    void setWear (float wear);
    void snapSmoothers();

    static constexpr int chunkSize = 64;

    // Cached once in the constructor: the audio thread reads them without building a
    // juce::String or searching the parameter tree
    std::atomic<float>* speedParam = nullptr;
    std::atomic<float>* noiseParam = nullptr;
    std::atomic<float>* crackleParam = nullptr;
    std::atomic<float>* wearParam = nullptr;
    std::atomic<float>* warpParam = nullptr;
    std::atomic<float>* widthParam = nullptr;

    int latency = 0, numChannels = 2;
    int fadePos = 0, fadeLength = 1;   // input fade-in after resetDsp(), so the cleared filters see no step
    double sampleRate = 48000.0, baseDelay = 0.0, maxDelay = 0.0, delayPos = 0.0, leak = 0.0;
    float lastWear = -1.0f, gritDrive = 1.0f, gritBlend = 0.0f;
    bool primed = false;

    std::atomic<float> platterWobble { 1.0f }, platterPhase { 0.0f };

    Channel ch[2];
    lofi::dsp::OnePole sideHp;
    vinyl::Static staticMid, staticSide;
    vinyl::Crackle crackle;
    vinyl::Warp warp;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampling;

    lofi::dsp::Smoothed wearS, warpCentsS, widthS, rateS, noiseGainS, popPeakS, lidS, tickPeakS;
};

//==============================================================================
/** The deck's 33 / 45 push buttons, bound to the speed parameter. The selected one
    latches down with its lamp lit; the other stands proud and lights up on hover. */
class SpeedSelector : public juce::Component,
                      public juce::SettableTooltipClient
{
public:
    SpeedSelector (juce::AudioProcessorValueTreeState&, const juce::String& paramId);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> buttonArea (int index) const;
    int buttonAt (juce::Point<float>) const;
    void setHover (int index);

    juce::RangedAudioParameter& parameter;
    juce::ParameterAttachment attachment;
    int current = 0, hover = -1;
};

//==============================================================================
/** A direct-drive deck on the left: a 12-inch LP at 33 or a 7-inch single on a 45
    adapter at 45, spinning at the platter's real speed error (magnified), a tonearm on
    a fixed-length arc that swings with the warp, a strobe ring that stands still at the
    nominal speed and drifts with the error, and a spin-down when bypassed. Controls in
    three panels on a four-column grid on the right: Noise, Out, Record. */
class VinylEditor : public lofi::LofiEditor
{
public:
    explicit VinylEditor (VinylProcessor&);

protected:
    void layoutBody (juce::Rectangle<int>) override;
    void paintBody (juce::Graphics&, juce::Rectangle<int>) override;
    void animate (float seconds) override;

private:
    bool is45() const                   { return speedParam->load() >= 0.5f; }

    void renderDeck (float scale, bool fast);
    void renderLabel (float scale, bool fast);
    void drawRecord (juce::Graphics&);
    void drawTonearm (juce::Graphics&);
    void drawLamp (juce::Graphics&);
    void drawSectionPanel (juce::Graphics&, juce::Rectangle<int>);

    juce::Point<float> stylusAt (float armAngle) const;
    float armAngleFor (float grooveRadius) const;
    juce::Rectangle<int> movingArea() const;
    juce::Rectangle<int> lampArea() const;

    VinylProcessor& vinyl;
    std::atomic<float>* speedParam = nullptr;
    std::atomic<float>* warpParam = nullptr;
    std::atomic<float>* bypassParam = nullptr;

    juce::Rectangle<int> graphic, noiseArea, recordArea, outArea;
    std::vector<lofi::Knob*> noiseKnobs, recordKnobs, outKnobs;
    std::unique_ptr<SpeedSelector> speed;

    // Cached layers, rendered at the display's physical pixel scale (the label at twice
    // that, as it is drawn rotated every frame) and re-rendered when scale or speed change
    juce::Image deckImage, labelImage;
    juce::Rectangle<int> deckBounds;
    float deckScale = 0.0f, labelScale = 0.0f;
    int deckSpeed = -1, labelSpeed = -1;

    juce::Point<float> platterCentre, armPivot;
    float angle = 0.0f, strobe = 0.0f, wobbleSmoothed = 1.0f;
    float spin = 1.0f, power = 1.0f, grooveRadius = 76.0f, shownPower = -1.0f;
};
