#include "LofiTheme.h"

namespace lofi::theme
{

juce::Font font (float height, bool bold)
{
   #if JUCE_MAC
    return juce::Font (juce::FontOptions ("Helvetica Neue", height, bold ? juce::Font::bold : juce::Font::plain));
   #else
    return juce::Font (juce::FontOptions ("Segoe UI", height, bold ? juce::Font::bold : juce::Font::plain));
   #endif
}

juce::Font monoFont (float height, bool bold)
{
    return juce::Font (juce::FontOptions ("Consolas", height, bold ? juce::Font::bold : juce::Font::plain));
}

} // namespace lofi::theme

namespace lofi
{

using namespace theme;

LofiLookAndFeel::LofiLookAndFeel()
{
   #if JUCE_MAC
    setDefaultSansSerifTypefaceName ("Helvetica Neue");
   #else
    setDefaultSansSerifTypefaceName ("Segoe UI");
   #endif
    setColour (juce::ResizableWindow::backgroundColourId, background);
    setColour (juce::Label::textColourId, text);
    setColour (juce::Slider::rotarySliderFillColourId, amber);
    setColour (juce::TextButton::buttonColourId, panelLight);
    setColour (juce::TextButton::buttonOnColourId, amber.withAlpha (0.85f));
    setColour (juce::TextButton::textColourOffId, text);
    setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    setColour (juce::ComboBox::backgroundColourId, panel);
    setColour (juce::ComboBox::textColourId, text);
    setColour (juce::ComboBox::outlineColourId, outline);
    setColour (juce::ComboBox::arrowColourId, textDim);
    setColour (juce::PopupMenu::backgroundColourId, panelLight);
    setColour (juce::PopupMenu::textColourId, text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, violet.withAlpha (0.35f));
    setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
    setColour (juce::TooltipWindow::backgroundColourId, juce::Colour (0xff2d3342));
    setColour (juce::TooltipWindow::textColourId, text);
    setColour (juce::BubbleComponent::backgroundColourId, juce::Colour (0xff2d3342));
    setColour (juce::BubbleComponent::outlineColourId, outline);
}

void LofiLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float pos,
                                        float startAngle, float endAngle, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (2.0f);
    const float radius = std::min (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float arcRadius = radius - 2.0f;
    const float angle = startAngle + pos * (endAngle - startAngle);
    const bool bipolar = slider.getProperties()["bipolar"];
    const auto fill = slider.findColour (juce::Slider::rotarySliderFillColourId);

    juce::Path track;
    track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
    g.setColour (outline);
    g.strokePath (track, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // A bipolar arc grows from the parameter's default (unity, centre), not from the minimum
    const float from = bipolar ? startAngle + (float) slider.valueToProportionOfLength (slider.getDoubleClickReturnValue())
                                                  * (endAngle - startAngle)
                               : startAngle;
    juce::Path value;
    value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, std::min (from, angle), std::max (from, angle), true);
    g.setColour (slider.isEnabled() ? fill : textDim);
    g.strokePath (value, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Knob body with a soft top light, a rim, and a pointer line
    const float bodyRadius = radius - 6.0f;
    const auto body = juce::Rectangle<float> (bodyRadius * 2.0f, bodyRadius * 2.0f).withCentre (centre);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3d4455), centre.x, body.getY(),
                                             juce::Colour (0xff1a1d26), centre.x, body.getBottom(), false));
    g.fillEllipse (body);
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.drawEllipse (body, 1.2f);
    g.setColour (juce::Colours::white.withAlpha (0.08f));
    g.drawEllipse (body.reduced (2.0f), 1.0f);

    juce::Path pointer;
    pointer.addRoundedRectangle (-1.2f, -bodyRadius + 3.0f, 2.4f, bodyRadius * 0.5f, 1.2f);
    g.setColour (text);
    g.fillPath (pointer, juce::AffineTransform::rotation (angle).translated (centre));
}

void LofiLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& colour, bool highlighted, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    auto c = b.getToggleState() ? b.findColour (juce::TextButton::buttonOnColourId) : colour;

    if (down)              c = c.brighter (0.15f);
    else if (highlighted)  c = c.brighter (0.08f);

    g.setColour (c);
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (outline);
    g.drawRoundedRectangle (r, 4.0f, 1.0f);
}

juce::Font LofiLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return font (juce::jlimit (12.0f, 14.0f, buttonHeight * 0.55f), true);
}

void LofiLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool highlighted, bool down)
{
    drawButtonBackground (g, b, panelLight, highlighted, down);
    g.setColour (b.getToggleState() ? juce::Colours::black : text);
    g.setFont (font (12.0f, true));
    g.drawText (b.getButtonText(), b.getLocalBounds(), juce::Justification::centred, true);
}

void LofiLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox&)
{
    auto r = juce::Rectangle<int> (width, height).toFloat().reduced (0.5f);
    g.setColour (panel);
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (outline);
    g.drawRoundedRectangle (r, 4.0f, 1.0f);

    juce::Path arrow;
    arrow.addTriangle (width - 14.0f, height * 0.42f, width - 6.0f, height * 0.42f, width - 10.0f, height * 0.62f);
    g.setColour (textDim);
    g.fillPath (arrow);
}

juce::Font LofiLookAndFeel::getComboBoxFont (juce::ComboBox&)        { return font (13.0f); }
juce::Font LofiLookAndFeel::getPopupMenuFont()                        { return font (14.0f); }
juce::Font LofiLookAndFeel::getLabelFont (juce::Label& l)             { return font (l.getFont().getHeight()); }

void LofiLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    g.fillAll (panelLight);
    g.setColour (outline);
    g.drawRect (0, 0, width, height);
}

void LofiLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& tip, int width, int height)
{
    g.setColour (juce::Colour (0xff2d3342));
    g.fillRoundedRectangle (0.0f, 0.0f, (float) width, (float) height, 4.0f);
    g.setColour (outline);
    g.drawRoundedRectangle (0.5f, 0.5f, width - 1.0f, height - 1.0f, 4.0f, 1.0f);
    g.setColour (text);
    g.setFont (font (15.0f));
    g.drawFittedText (tip, 8, 4, width - 16, height - 8, juce::Justification::centredLeft, 3);
}

//==============================================================================
Knob::Knob (juce::AudioProcessorValueTreeState& state, const juce::String& paramId, const juce::String& l,
            juce::Colour a, bool bipolar)
    : slider (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox), label (l), accent (a)
{
    slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
    slider.setMouseDragSensitivity (180);
    slider.setColour (juce::Slider::rotarySliderFillColourId, accent);
    slider.getProperties().set ("bipolar", bipolar);
    slider.setWantsKeyboardFocus (false);
    slider.onValueChange = [this] { repaint(); };
    addAndMakeVisible (slider);

    attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (state, paramId, slider);
    slider.setDoubleClickReturnValue (true, slider.getDoubleClickReturnValue());

    if (auto* p = state.getParameter (paramId))
    {
        const double def = p->convertFrom0to1 (p->getDefaultValue());
        slider.setDoubleClickReturnValue (true, def);

        // Bipolar knobs put their default at twelve o'clock even when the range is lopsided
        // (Output's -24..+12 dB): each half of the travel maps linearly to one side of it.
        // Only the knob's feel changes; the parameter and its automation curve do not.
        const auto range = p->getNormalisableRange();

        if (bipolar && def > range.start && def < range.end)
        {
            const double step = range.interval;

            slider.setNormalisableRange ({ range.start, range.end,
                [def] (double lo, double hi, double t) { return t < 0.5 ? juce::jmap (t, 0.0, 0.5, lo, def) : juce::jmap (t, 0.5, 1.0, def, hi); },
                [def] (double lo, double hi, double v) { return v < def ? juce::jmap (v, lo, def, 0.0, 0.5) : juce::jmap (v, def, hi, 0.5, 1.0); },
                [step] (double lo, double hi, double v) { return step > 0.0 ? juce::jlimit (lo, hi, lo + std::round ((v - lo) / step) * step) : v; } });
        }
    }
}

void Knob::resized()
{
    auto r = getLocalBounds();
    r.removeFromTop (16);
    r.removeFromBottom (16);
    const int size = std::min (r.getWidth(), r.getHeight());
    slider.setBounds (juce::Rectangle<int> (size, size).withCentre (r.getCentre()));
}

void Knob::paint (juce::Graphics& g)
{
    g.setColour (textDim.brighter (0.15f));
    g.setFont (font (11.5f, true).withExtraKerningFactor (0.06f));
    g.drawText (label.toUpperCase(), getLocalBounds().removeFromTop (16), juce::Justification::centred, true);

    g.setColour (text);
    g.setFont (monoFont (12.0f));
    g.drawText (slider.getTextFromValue (slider.getValue()), getLocalBounds().removeFromBottom (16), juce::Justification::centred, true);
}

//==============================================================================
ChoiceBox::ChoiceBox (juce::AudioProcessorValueTreeState& state, const juce::String& paramId, const juce::String& l)
    : label (l)
{
    if (auto* p = dynamic_cast<juce::AudioParameterChoice*> (state.getParameter (paramId)))
        box.addItemList (p->choices, 1);

    box.setWantsKeyboardFocus (false);
    addAndMakeVisible (box);
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, paramId, box);
}

void ChoiceBox::resized()
{
    box.setBounds (getLocalBounds().withTrimmedTop (16).withHeight (24));
}

void ChoiceBox::paint (juce::Graphics& g)
{
    g.setColour (textDim.brighter (0.15f));
    g.setFont (font (11.5f, true).withExtraKerningFactor (0.06f));
    g.drawText (label.toUpperCase(), getLocalBounds().removeFromTop (16), juce::Justification::centred, true);
}

//==============================================================================
LedButton::LedButton (juce::AudioProcessorValueTreeState& state, const juce::String& paramId, const juce::String& l, juce::Colour c)
    : button (l, c)
{
    button.setWantsKeyboardFocus (false);
    addAndMakeVisible (button);
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (state, paramId, button);
}

void LedButton::resized()
{
    button.setBounds (getLocalBounds());
}

void LedButton::Button::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    auto c = panelLight;

    if (down)              c = c.brighter (0.15f);
    else if (highlighted)  c = c.brighter (0.08f);

    g.setColour (c);
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (outline);
    g.drawRoundedRectangle (r, 4.0f, 1.0f);

    auto ledArea = juce::Rectangle<float> (8.0f, 8.0f).withCentre ({ r.getX() + 12.0f, r.getCentreY() });
    g.setColour (getToggleState() ? led : led.withAlpha (0.15f));
    g.fillEllipse (ledArea);

    if (getToggleState())
    {
        g.setColour (led.withAlpha (0.3f));
        g.fillEllipse (ledArea.expanded (2.5f));
    }

    g.setColour (theme::text);
    g.setFont (font (12.0f, true));
    g.drawText (getButtonText(), getLocalBounds().withTrimmedLeft (22), juce::Justification::centredLeft, true);
}

//==============================================================================
VuMeter::VuMeter (const juce::String& c) : caption (c) {}

namespace
{
    // 0 VU sits at -10 dBFS peak, where a healthy track lives, so the needle moves
    constexpr float vuReferenceDbfs = -10.0f;

    // The classic VU scale is not linear in dB: -20 at the far left, 0 at 80 %, +3 at the end
    const std::pair<float, float> vuScale[] = { { -20.0f, 0.00f }, { -10.0f, 0.25f }, { -7.0f, 0.38f }, { -5.0f, 0.48f },
                                                { -3.0f, 0.60f }, { -2.0f, 0.67f }, { -1.0f, 0.74f }, { 0.0f, 0.80f },
                                                { 1.0f, 0.86f }, { 2.0f, 0.93f }, { 3.0f, 1.00f } };

    float vuPosition (float vu)
    {
        if (vu <= vuScale[0].first)
            return 0.0f;

        for (size_t i = 1; i < std::size (vuScale); ++i)
            if (vu <= vuScale[i].first)
                return juce::jmap (vu, vuScale[i - 1].first, vuScale[i].first, vuScale[i - 1].second, vuScale[i].second);

        return 1.0f;
    }
}

void VuMeter::push (float peak)
{
    // Needle ballistics: roughly 300 ms to settle, like the real thing
    const float vu = juce::Decibels::gainToDecibels (peak, -60.0f) - vuReferenceDbfs;
    target = vuPosition (vu);
    needle += (target - needle) * (target > needle ? 0.35f : 0.12f);

    if (peak >= 0.999f)
    {
        clipped = true;
        clipFrames = 45;
    }
    else if (--clipFrames < 0)
    {
        clipped = false;
    }

    repaint();
}

void VuMeter::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (juce::Colour (0xff0b0d11));
    g.fillRoundedRectangle (r, 6.0f);

    auto face = r.reduced (3.0f);
    g.setGradientFill (juce::ColourGradient (vuCream, face.getX(), face.getY(), vuCream.darker (0.25f), face.getX(), face.getBottom(), false));
    g.fillRoundedRectangle (face, 3.0f);

    // A wide, shallow arc whose pivot sits far below the face, as on a real meter: the
    // scale spans most of the width and leaves the top corners free for caption and lamp
    const float radius = face.getWidth() * 0.72f;
    const auto pivot = juce::Point<float> (face.getCentreX(), face.getY() + 6.0f + radius);
    constexpr float halfAngle = 0.42f;
    const auto ink = juce::Colour (0xff3a3328);
    const auto red = danger.darker (0.25f);

    auto angleAt = [&] (float position)          { return juce::jmap (position, -halfAngle, halfAngle); };
    auto pointAt = [&] (float angle, float rad)  { return pivot + juce::Point<float> (std::sin (angle), -std::cos (angle)) * rad; };

    g.saveState();
    g.reduceClipRegion (face.toNearestInt());

    const float zero = angleAt (vuPosition (0.0f));
    juce::Path blackArc, redArc;
    blackArc.addCentredArc (pivot.x, pivot.y, radius, radius, 0.0f, angleAt (0.0f), zero, true);
    redArc.addCentredArc (pivot.x, pivot.y, radius, radius, 0.0f, zero, angleAt (1.0f), true);
    g.setColour (ink);
    g.strokePath (blackArc, juce::PathStrokeType (1.2f));
    g.setColour (red);
    g.strokePath (redArc, juce::PathStrokeType (2.6f));

    for (auto& [vu, position] : vuScale)
    {
        const float a = angleAt (position);
        const bool major = vu == -20.0f || vu == -10.0f || vu == -5.0f || vu == 0.0f || vu == 3.0f;
        g.setColour (vu > 0.0f ? red : ink);
        g.drawLine ({ pointAt (a, radius), pointAt (a, radius + (major ? 4.0f : 2.5f)) }, major ? 1.3f : 1.0f);
    }

    // Labels inside the arc: the minus side unsigned, the plus side in red, as printed on VUs
    // (the plus side's figures in a deep red: the arc's red is under 3 to 1 on the cream; 0.75 darker,
    // not 0.6, as a Mac's cream measured 4.4 to 1 against it)
    g.setFont (font (12.0f, true));

    for (auto vu : { -20.0f, -10.0f, 0.0f, 3.0f })   // (5 left out: at a readable size it runs into 10)
    {
        const auto at = pointAt (angleAt (vuPosition (vu)), radius - 11.5f);
        g.setColour (vu > 0.0f ? red.darker (0.75f) : ink);
        g.drawText (juce::String (std::abs ((int) vu)), juce::Rectangle<float> (26.0f, 15.0f).withCentre (at), juce::Justification::centred, true);
    }

    // Needle, from the hidden pivot below the face to just past the scale
    const float a = angleAt (needle);
    g.setColour (juce::Colour (0xff1b1712));
    g.drawLine ({ pivot, pointAt (a, radius + 3.0f) }, 1.5f);

    g.restoreState();

    g.setColour (ink);
    g.setFont (font (11.5f, true).withExtraKerningFactor (0.08f));
    g.drawText (caption, juce::Rectangle<float> (face.getX() + 5.0f, face.getY() + 2.0f, 40.0f, 14.0f), juce::Justification::centredLeft, true);

    // Clip lamp, top right
    auto lamp = juce::Rectangle<float> (5.0f, 5.0f).withCentre ({ face.getRight() - 6.0f, face.getY() + 6.0f });
    g.setColour (clipped ? danger : danger.withAlpha (0.2f));
    g.fillEllipse (lamp);

    // Glass glint
    g.setColour (juce::Colours::white.withAlpha (0.06f));
    g.fillRoundedRectangle (face.withHeight (face.getHeight() * 0.35f), 4.0f);
    g.setColour (outline);
    g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
}

//==============================================================================
void LedMeter::push (float peak)
{
    level = peak > level ? peak : level * 0.85f;

    if (peak >= hold)
    {
        hold = peak;
        holdFrames = 40;
    }
    else if (--holdFrames < 0)
    {
        hold *= 0.9f;
    }

    repaint();
}

void LedMeter::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (juce::Colour (0xff0b0d11));
    g.fillRoundedRectangle (r, 3.0f);

    const int segments = std::max (6, getHeight() / 7);
    const float segH = (r.getHeight() - 4.0f) / (float) segments;
    const float dbLevel = juce::Decibels::gainToDecibels (level, -60.0f);
    const float dbHold = juce::Decibels::gainToDecibels (hold, -60.0f);

    for (int i = 0; i < segments; ++i)
    {
        const float dbSeg = juce::jmap ((float) i, 0.0f, (float) segments - 1, -60.0f, 0.0f);
        const auto c = dbSeg > -3.0f ? danger : (dbSeg > -12.0f ? amber : juce::Colour (0xff3ecf8e));
        const bool on = dbLevel >= dbSeg || std::abs (dbHold - dbSeg) < 60.0f / segments * 0.5f;
        auto seg = juce::Rectangle<float> (r.getX() + 2.0f, r.getBottom() - 2.0f - (i + 1) * segH, r.getWidth() - 4.0f, segH - 1.0f);
        g.setColour (on ? c : c.withAlpha (0.12f));
        g.fillRect (seg);
    }
}

//==============================================================================
void drawSectionTitle (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title, juce::Colour accent)
{
    g.setColour (accent);
    g.setFont (font (11.5f, true).withExtraKerningFactor (0.14f));
    const int w = (int) juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), title.toUpperCase()) + 10;
    g.drawText (title.toUpperCase(), area.withWidth (w), juce::Justification::centredLeft, true);
    g.setColour (juce::Colours::white.withAlpha (0.08f));
    g.fillRect (area.getX() + w, area.getCentreY(), area.getWidth() - w, 1);
}

void drawFaceplate (juce::Graphics& g, juce::Rectangle<int> area, juce::Colour base)
{
    g.setGradientFill (juce::ColourGradient (base.brighter (0.08f), (float) area.getX(), (float) area.getY(),
                                             base.darker (0.12f), (float) area.getX(), (float) area.getBottom(), false));
    g.fillRoundedRectangle (area.toFloat(), 8.0f);

    // Faint brushed lines, deterministic so repaints don't shimmer
    juce::Random rng (1234);
    g.setColour (juce::Colours::white.withAlpha (0.018f));

    for (int y = area.getY() + 2; y < area.getBottom() - 2; y += 3)
    {
        const int x0 = area.getX() + rng.nextInt (24);
        const int x1 = area.getRight() - rng.nextInt (24);
        g.drawHorizontalLine (y, (float) x0, (float) x1);
    }

    g.setColour (juce::Colours::white.withAlpha (0.05f));
    g.drawRoundedRectangle (area.toFloat().reduced (1.0f), 7.0f, 1.0f);
    g.setColour (outline);
    g.drawRoundedRectangle (area.toFloat().reduced (0.5f), 8.0f, 1.0f);
}

void drawScrew (juce::Graphics& g, juce::Point<float> centre, float radius)
{
    auto r = juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff5a6172), centre.x, r.getY(), juce::Colour (0xff22262f), centre.x, r.getBottom(), false));
    g.fillEllipse (r);
    g.setColour (juce::Colours::black.withAlpha (0.7f));
    g.drawEllipse (r, 1.0f);
    g.setColour (juce::Colours::black.withAlpha (0.55f));
    g.drawLine (centre.x - radius * 0.6f, centre.y - radius * 0.4f, centre.x + radius * 0.6f, centre.y + radius * 0.4f, 1.4f);
}

} // namespace lofi
