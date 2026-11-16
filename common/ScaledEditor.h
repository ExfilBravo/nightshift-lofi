#pragma once

#include <JuceHeader.h>

namespace lofi
{

/** The plugin's window: the editor as it's designed (820 x 460), shown a quarter bigger as a whole, so its
    words come out at 15 px and up (the user, 2026-11-15: "make all of the text more readable") while its
    knobs, art and spacing keep their balance. createEditor returns one of these around the designed editor;
    it's an ordinary editor to the host, just bigger. */
class ScaledEditor : public juce::AudioProcessorEditor
{
public:
    static constexpr float scale = 1.25f;

    ScaledEditor (juce::AudioProcessor& p, juce::AudioProcessorEditor* designed)
        : juce::AudioProcessorEditor (p), inner (designed)
    {
        addAndMakeVisible (*inner);
        inner->setTransform (juce::AffineTransform::scale (scale));
        setSize (juce::roundToInt ((float) inner->getWidth() * scale), juce::roundToInt ((float) inner->getHeight() * scale));
    }

    void resized() override     { inner->setTopLeftPosition (0, 0); }

private:
    std::unique_ptr<juce::AudioProcessorEditor> inner;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScaledEditor)
};

} // namespace lofi
