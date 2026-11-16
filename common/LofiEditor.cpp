#include "LofiEditor.h"

namespace lofi
{

using namespace theme;

LofiEditor::LofiEditor (LofiProcessor& p)
    : juce::AudioProcessorEditor (p), processor (p)
{
    setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (inputVu);
    addAndMakeVisible (outputVu);

    bypassLed = std::make_unique<LedButton> (processor.getState(), "bypass", "BYPASS", danger);
    addAndMakeVisible (*bypassLed);

    // No setSize here: the subclass does that once its controls exist (see the header)
    lastFrameMs = juce::Time::getMillisecondCounter();
    startTimerHz (30);
}

LofiEditor::~LofiEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

//==============================================================================
Knob& LofiEditor::addKnob (const juce::String& paramId, const juce::String& label, juce::Colour accent, bool bipolar)
{
    auto* k = new Knob (processor.getState(), paramId, label, accent, bipolar);
    controls.add (k);
    addAndMakeVisible (k);
    return *k;
}

ChoiceBox& LofiEditor::addChoice (const juce::String& paramId, const juce::String& label)
{
    auto* c = new ChoiceBox (processor.getState(), paramId, label);
    controls.add (c);
    addAndMakeVisible (c);
    return *c;
}

LedButton& LofiEditor::addLed (const juce::String& paramId, const juce::String& label, juce::Colour led)
{
    auto* l = new LedButton (processor.getState(), paramId, label, led);
    controls.add (l);
    addAndMakeVisible (l);
    return *l;
}

juce::Rectangle<int> LofiEditor::getBodyArea() const
{
    return getLocalBounds().withTrimmedTop (headerHeight).withTrimmedBottom (footerHeight).reduced (14, 6);
}

//==============================================================================
void LofiEditor::resized()
{
    // Right of the header: bypass, then the two VUs, clear of the faceplate's corner screw
    auto header = getLocalBounds().removeFromTop (headerHeight);
    header.removeFromRight (28);
    auto right = header.removeFromRight (380).reduced (0, 7);
    outputVu.setBounds (right.removeFromRight (136));
    right.removeFromRight (8);
    inputVu.setBounds (right.removeFromRight (136));
    right.removeFromRight (12);
    bypassLed->setBounds (right.removeFromRight (84).reduced (0, 6));

    layoutBody (getBodyArea());
}

void LofiEditor::paint (juce::Graphics& g)
{
    g.fillAll (background);

    const auto& info = processor.getInfo();
    auto plate = getLocalBounds().reduced (6);
    drawFaceplate (g, plate, faceplate);

    for (auto corner : { plate.getTopLeft(), plate.getTopRight(), plate.getBottomLeft(), plate.getBottomRight() })
        drawScrew (g, corner.toFloat() + juce::Point<float> (corner.x == plate.getX() ? 11.0f : -11.0f,
                                                             corner.y == plate.getY() ? 11.0f : -11.0f), 4.5f);

    // Header: product badge, clear of the top-left screw
    auto header = getLocalBounds().removeFromTop (headerHeight).withTrimmedLeft (30);
    g.setColour (info.accent);
    g.fillRoundedRectangle ((float) header.getX(), header.getY() + 16.0f, 5.0f, headerHeight - 32.0f, 2.5f);

    g.setColour (text);
    g.setFont (font (20.0f, true).withExtraKerningFactor (0.08f));
    g.drawText ("NIGHTSHIFT", header.withTrimmedLeft (14).withTrimmedTop (8).withHeight (24), juce::Justification::centredLeft, true);

    const int nameWidth = (int) juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), "NIGHTSHIFT");
    g.setColour (info.accent);
    g.drawText (info.name.fromLastOccurrenceOf (" ", false, false).toUpperCase(),
                header.withTrimmedLeft (14 + nameWidth + 10).withTrimmedTop (8).withHeight (24), juce::Justification::centredLeft, true);

    g.setColour (textDim);
    g.setFont (font (11.5f).withExtraKerningFactor (0.08f));
    g.drawText (info.tagline.toUpperCase(), header.withTrimmedLeft (14).withTrimmedTop (31).withHeight (14), juce::Justification::centredLeft, true);

    paintBody (g, getBodyArea());

    // Footer
    auto footer = getLocalBounds().removeFromBottom (footerHeight).reduced (24, 0);
    g.setColour (textDim);
    g.setFont (font (12.0f));
    g.drawText ("Nightshift Lofi  v" JucePlugin_VersionString, footer, juce::Justification::centredLeft, true);
    g.drawText ("double-click a knob to reset", footer, juce::Justification::centredRight, true);
}

void LofiEditor::timerCallback()
{
    inputLevel = std::max (processor.getInputPeak (0), processor.getInputPeak (1));
    outputLevel = std::max (processor.getOutputPeak (0), processor.getOutputPeak (1));
    inputVu.push (inputLevel);
    outputVu.push (outputLevel);

    const auto now = juce::Time::getMillisecondCounter();
    animate (juce::jlimit (0.0f, 0.2f, (now - lastFrameMs) * 0.001f));
    lastFrameMs = now;
}

} // namespace lofi
