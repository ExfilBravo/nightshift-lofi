#pragma once

#include <JuceHeader.h>

namespace lofi::theme
{
    // The Nightshift palette, so the plugins feel like part of the studio
    inline const juce::Colour background   { 0xff0f1116 };
    inline const juce::Colour panel        { 0xff1a1e27 };
    inline const juce::Colour panelLight   { 0xff222733 };
    inline const juce::Colour faceplate    { 0xff161a22 };
    inline const juce::Colour outline      { 0xff07080b };
    inline const juce::Colour text         { 0xffe1e5ee };
    inline const juce::Colour textDim      { 0xff8a91a4 };
    inline const juce::Colour amber        { 0xffffa94d };
    inline const juce::Colour violet       { 0xff8794ff };
    inline const juce::Colour teal         { 0xff5fd0c5 };
    inline const juce::Colour rose         { 0xffe27fd0 };
    inline const juce::Colour ledOn        { 0xff7ee081 };
    inline const juce::Colour danger       { 0xffff5d6c };
    inline const juce::Colour vuCream      { 0xffe9dcc0 };   // classic meter face

    juce::Font font (float height, bool bold = false);
    juce::Font monoFont (float height, bool bold = false);
}

namespace lofi
{

class LofiLookAndFeel : public juce::LookAndFeel_V4
{
public:
    LofiLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos,
                           float startAngle, float endAngle, juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted, bool down) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown, int buttonX, int buttonY,
                       int buttonW, int buttonH, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;
    juce::Font getPopupMenuFont() override;
    void drawTooltip (juce::Graphics&, const juce::String& text, int width, int height) override;
    juce::Font getLabelFont (juce::Label&) override;
};

//==============================================================================
/** A rotary control with its name above and its value below, bound to a parameter. */
class Knob : public juce::Component
{
public:
    Knob (juce::AudioProcessorValueTreeState&, const juce::String& paramId, const juce::String& label,
          juce::Colour accent = theme::amber, bool bipolar = false);

    void resized() override;
    void paint (juce::Graphics&) override;

    juce::Slider& getSlider()       { return slider; }
    static constexpr int defaultSize = 84;

private:
    juce::Slider slider;
    juce::String label;
    juce::Colour accent;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

/** A choice parameter as a drop-down with a caption. */
class ChoiceBox : public juce::Component
{
public:
    ChoiceBox (juce::AudioProcessorValueTreeState&, const juce::String& paramId, const juce::String& label);
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    juce::ComboBox box;
    juce::String label;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> attachment;
};

/** A bool parameter as an LED push button. */
class LedButton : public juce::Component
{
public:
    LedButton (juce::AudioProcessorValueTreeState&, const juce::String& paramId, const juce::String& label,
               juce::Colour ledColour = theme::ledOn);
    void resized() override;

private:
    struct Button : public juce::ToggleButton
    {
        Button (const juce::String& n, juce::Colour c) : juce::ToggleButton (n), led (c) {}
        void paintButton (juce::Graphics&, bool highlighted, bool down) override;
        juce::Colour led;
    } button;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> attachment;
};

//==============================================================================
/** An analogue-style VU meter: cream face, arc scale, a needle with ballistics. */
class VuMeter : public juce::Component
{
public:
    explicit VuMeter (const juce::String& caption = "VU");

    /** Feed the latest peak (as gain, 1 = 0 dBFS); call from a timer at 30-60 Hz. */
    void push (float peakGain);
    void paint (juce::Graphics&) override;

private:
    juce::String caption;
    float needle = 0.0f, target = 0.0f;
    bool clipped = false;
    int clipFrames = 0;
};

/** A thin LED bar meter, vertical, with a falling peak. */
class LedMeter : public juce::Component
{
public:
    void push (float peakGain);
    void paint (juce::Graphics&) override;

private:
    float level = 0.0f, hold = 0.0f;
    int holdFrames = 0;
};

/** Draws a section title with a faint rule, as on a hardware faceplate. */
void drawSectionTitle (juce::Graphics&, juce::Rectangle<int> area, const juce::String& title, juce::Colour accent);

/** The brushed faceplate texture plugins share; subtle, no tiling artefacts. */
void drawFaceplate (juce::Graphics&, juce::Rectangle<int> area, juce::Colour base);

/** A screw head, for the corners of faceplates. */
void drawScrew (juce::Graphics&, juce::Point<float> centre, float radius);

} // namespace lofi
