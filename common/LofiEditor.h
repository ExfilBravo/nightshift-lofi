#pragma once

#include <JuceHeader.h>
#include "LofiProcessor.h"
#include "LofiTheme.h"

namespace lofi
{

/** The window every plugin shares: a faceplate with the product name and tagline on
    the left of the header, bypass and meters on the right, and a body the subclass
    fills with controls and its hardware graphic. Animations run from the 30 Hz timer.
*/
class LofiEditor : public juce::AudioProcessorEditor,
                   private juce::Timer
{
public:
    /** Subclasses add their controls, then call setSize (820, 460) as the LAST line of
        their constructor: that is what lays everything out, and it must not happen
        before the subclass exists (layoutBody is pure virtual). */
    explicit LofiEditor (LofiProcessor&);
    ~LofiEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int headerHeight = 54, footerHeight = 26;

protected:
    LofiProcessor& processor;

    /** Creates a control bound to a parameter and makes it visible. */
    Knob& addKnob (const juce::String& paramId, const juce::String& label, juce::Colour accent = theme::amber, bool bipolar = false);
    ChoiceBox& addChoice (const juce::String& paramId, const juce::String& label);
    LedButton& addLed (const juce::String& paramId, const juce::String& label, juce::Colour led = theme::ledOn);

    /** The area below the header and above the footer. */
    juce::Rectangle<int> getBodyArea() const;

    /** Lay out your controls here. */
    virtual void layoutBody (juce::Rectangle<int> body) = 0;

    /** Draw the hardware graphic and section titles here, under the controls. */
    virtual void paintBody (juce::Graphics&, juce::Rectangle<int> body) = 0;

    /** Called 30 times a second; advance animations and repaint what moved. */
    virtual void animate (float secondsSinceLastFrame) = 0;

    /** Peaks already pulled from the processor this frame, for your own displays. */
    float inputLevel = 0.0f, outputLevel = 0.0f;

private:
    LofiLookAndFeel lookAndFeel;
    juce::OwnedArray<juce::Component> controls;
    VuMeter inputVu { "IN" }, outputVu { "OUT" };
    std::unique_ptr<LedButton> bypassLed;
    juce::TooltipWindow tooltips { this, 700 };
    juce::uint32 lastFrameMs = 0;

    void timerCallback() override;
};

} // namespace lofi
