#include "Crush.h"
#include "ModeSwitch.h"

using namespace lofi;

namespace
{
    const juce::Colour displayGlass  { 0xff07090d };
    const juce::Colour padFaceTop    { 0xff2a2e38 };
    const juce::Colour padFaceBottom { 0xff1c1f27 };
    const juce::Colour ledGreen      { 0xff3ecf8e };    // the meter ladder, as common's LedMeter

    const char* const modeNames[] = { "SP-1200", "MPC60", "CUSTOM" };

    constexpr int columns = 4;
    constexpr int titleHeight = 18, titleToKnobs = 14;
}

//==============================================================================
CrushEditor::CrushEditor (CrushProcessor& p)
    : LofiEditor (p), crush (p)
{
    // Signal order: the drive into the converter, then what the converter does
    converterKnobs = { &addKnob ("drive", "Drive", theme::violet), &addKnob ("bits", "Bits", theme::violet),
                       &addKnob ("rate", "Rate", theme::violet),   &addKnob ("alias", "Alias", theme::violet) };
    filterKnobs    = { &addKnob ("filter", "Cutoff", theme::teal), &addKnob ("resonance", "Resonance", theme::teal) };
    outKnobs       = { &addKnob ("mix", "Mix", theme::amber), &addKnob ("output", "Output", theme::amber, true) };

    modeSwitch = std::make_unique<ModeSwitch> (p.getState(), "mode");
    addAndMakeVisible (*modeSwitch);

    setSize (820, 460);
}

CrushEditor::~CrushEditor() = default;

void CrushEditor::layoutBody (juce::Rectangle<int> body)
{
    graphic = body.removeFromLeft (300);
    body.removeFromLeft (16);

    // One grid for both rows: four columns across the right side
    const int col = body.getWidth() / columns;
    body = body.withWidth (col * columns);

    // Knobs hang from the top of their area, a fixed distance under their title
    auto row = [] (std::vector<Knob*>& knobs, juce::Rectangle<int> area, int columnWidth)
    {
        for (auto* k : knobs)
            k->setBounds (area.removeFromLeft (columnWidth).removeFromTop (Knob::defaultSize + 32)
                              .withSizeKeepingCentre (Knob::defaultSize, Knob::defaultSize + 32));
    };

    body.removeFromTop (6);
    auto top = body.removeFromTop (titleHeight + titleToKnobs + Knob::defaultSize + 32);
    converterTitle = top.removeFromTop (titleHeight).withWidth (2 * col - 12);
    modeSwitch->setBounds (body.getX() + 2 * col, converterTitle.getY() - 3, 2 * col, 24);
    row (converterKnobs, top.withTrimmedTop (titleToKnobs), col);

    body.removeFromTop (42);
    auto filterArea = body.removeFromLeft (2 * col);
    auto outArea = body;
    filterTitle = filterArea.removeFromTop (titleHeight).withTrimmedRight (12);
    outTitle = outArea.removeFromTop (titleHeight);
    row (filterKnobs, filterArea.withTrimmedTop (titleToKnobs), col);
    row (outKnobs, outArea.withTrimmedTop (titleToKnobs), col);
}

//==============================================================================
void CrushEditor::paintBody (juce::Graphics& g, juce::Rectangle<int>)
{
    drawSectionTitle (g, converterTitle, "Converter", theme::violet);
    drawSectionTitle (g, filterTitle, "Filter", theme::teal);
    drawSectionTitle (g, outTitle, "Out", theme::amber);

    const auto plate = graphic.toFloat();
    paintFaceplate (g, plate);

    const float x = plate.getX() + 16.0f, w = plate.getWidth() - 32.0f;
    paintDisplay (g, { x, plate.getY() + 16.0f, w, 100.0f });
    paintMeter   (g, { x, plate.getY() + 128.0f, w, 18.0f });
    paintScope   (g, { x, plate.getY() + 156.0f, w, 108.0f });
    paintPads    (g, { x, plate.getY() + 276.0f, w, 62.0f });
}

void CrushEditor::paintFaceplate (juce::Graphics& g, juce::Rectangle<float> plate)
{
    // The sampler's casing: its own lighter, cooler panel with a bevelled top edge
    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.fillRoundedRectangle (plate.translated (0.0f, 2.0f), 10.0f);
    g.setGradientFill (juce::ColourGradient (theme::panelLight.brighter (0.05f), plate.getX(), plate.getY(),
                                             theme::panel, plate.getX(), plate.getBottom(), false));
    g.fillRoundedRectangle (plate, 10.0f);
    g.setColour (juce::Colours::white.withAlpha (0.09f));
    g.drawHorizontalLine ((int) plate.getY() + 1, plate.getX() + 8.0f, plate.getRight() - 8.0f);
    g.setColour (juce::Colours::white.withAlpha (0.035f));
    g.drawRoundedRectangle (plate.reduced (1.0f), 9.0f, 1.0f);
    g.setColour (theme::outline);
    g.drawRoundedRectangle (plate.reduced (0.5f), 10.0f, 1.0f);

    for (auto corner : { plate.getTopLeft(), plate.getTopRight(), plate.getBottomLeft(), plate.getBottomRight() })
        drawScrew (g, corner + juce::Point<float> (corner.x == plate.getX() ? 9.0f : -9.0f,
                                                   corner.y == plate.getY() ? 9.0f : -9.0f), 3.5f);

    // Model badge: an accent bar and the name, as on the header
    const auto badge = juce::Rectangle<float> (plate.getX(), plate.getBottom() - 26.0f, plate.getWidth(), 18.0f);
    g.setFont (theme::font (11.5f, true).withExtraKerningFactor (0.16f));
    const juce::String maker ("NIGHTSHIFT"), model ("NS-1200");
    const float makerW = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), maker);
    const float modelW = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), model);
    const float total = 3.0f + 8.0f + makerW + 10.0f + modelW;
    float bx = badge.getCentreX() - total * 0.5f;

    g.setColour (theme::violet);
    g.fillRoundedRectangle (bx, badge.getCentreY() - 6.0f, 3.0f, 12.0f, 1.5f);
    bx += 11.0f;
    g.setColour (theme::textDim);
    g.drawText (maker, juce::Rectangle<float> (bx, badge.getY(), makerW + 4.0f, badge.getHeight()), juce::Justification::centredLeft, true);
    bx += makerW + 10.0f;
    g.setColour (theme::violet.withAlpha (0.9f));
    g.drawText (model, juce::Rectangle<float> (bx, badge.getY(), modelW + 4.0f, badge.getHeight()), juce::Justification::centredLeft, true);
}

//==============================================================================
void CrushEditor::drawSevenSegment (juce::Graphics& g, juce::Rectangle<float> box, int digit, bool dot,
                                    juce::Colour on, juce::Colour off)
{
    static const juce::uint8 masks[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    const float w = box.getWidth(), h = box.getHeight(), t = w * 0.17f, gap = 1.2f;
    const float hx = t * 0.5f + gap, hw = w - t - 2.0f * gap;
    const float vy1 = t * 0.5f + gap, vh = h * 0.5f - t - 2.0f * gap, vy2 = h * 0.5f + t * 0.5f + gap;

    const juce::Rectangle<float> segments[7] =
    {
        { hx, 0.0f, hw, t },                 // a
        { w - t, vy1, t, vh },               // b
        { w - t, vy2, t, vh },               // c
        { hx, h - t, hw, t },                // d
        { 0.0f, vy2, t, vh },                // e
        { 0.0f, vy1, t, vh },                // f
        { hx, h * 0.5f - t * 0.5f, hw, t }   // g
    };

    const auto transform = juce::AffineTransform::shear (-0.12f, 0.0f).translated (box.getX() + 0.06f * h, box.getY());
    const int mask = digit >= 0 && digit <= 9 ? masks[digit] : 0;

    juce::Path lit, dark;

    for (int s = 0; s < 7; ++s)
        ((mask >> s) & 1 ? lit : dark).addRoundedRectangle (segments[s], t * 0.3f);

    dark.applyTransform (transform);
    lit.applyTransform (transform);
    g.setColour (off);
    g.fillPath (dark);

    if (! lit.isEmpty())
    {
        g.setColour (on.withAlpha (0.28f));
        g.strokePath (lit, juce::PathStrokeType (3.0f));
        g.setColour (on);
        g.fillPath (lit);
    }

    if (dot)
    {
        auto d = juce::Rectangle<float> (t, t).withPosition (box.getRight() + 2.0f, box.getBottom() - t);
        g.setColour (on.withAlpha (0.28f));
        g.fillRoundedRectangle (d.expanded (1.5f), t * 0.4f);
        g.setColour (on);
        g.fillRoundedRectangle (d, t * 0.3f);
    }
}

void CrushEditor::paintDisplay (juce::Graphics& g, juce::Rectangle<float> r)
{
    g.setColour (juce::Colours::black.withAlpha (0.55f));
    g.fillRoundedRectangle (r.expanded (1.5f), 7.0f);
    g.setGradientFill (juce::ColourGradient (displayGlass.brighter (0.12f), r.getX(), r.getY(),
                                             displayGlass, r.getX(), r.getBottom(), false));
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (juce::Colours::white.withAlpha (0.04f));
    g.drawRoundedRectangle (r.reduced (1.0f), 5.0f, 1.0f);

    const auto on = theme::amber;
    const auto off = theme::amber.withAlpha (0.07f);
    const float digitW = 24.0f, digitH = 42.0f, digitGap = 6.0f, margin = 18.0f;
    const float top = r.getY() + 14.0f;
    const auto captionFont = theme::monoFont (11.5f).withExtraKerningFactor (0.06f);

    // Bits: two digits, blank-padded, from the left margin
    const int bits = juce::roundToInt (crush.param ("bits"));
    const float bitsX = r.getX() + margin;
    drawSevenSegment (g, { bitsX, top, digitW, digitH }, bits >= 10 ? bits / 10 : -1, false, on, off);
    drawSevenSegment (g, { bitsX + digitW + digitGap, top, digitW, digitH }, bits % 10, false, on, off);
    const float bitsRight = bitsX + 2.0f * digitW + digitGap;

    g.setColour (theme::textDim);
    g.setFont (captionFont);
    g.drawText ("BITS", juce::Rectangle<float> (bitsX, top + digitH + 8.0f, bitsRight - bitsX, 12.0f), juce::Justification::centred, true);

    // Rate in kHz: four digits with the point after the second, to the right margin
    const float rateW = 4.0f * digitW + 3.0f * digitGap + 8.0f;
    const float rateX = r.getRight() - margin - rateW;
    const int hundredths = juce::roundToInt (crush.param ("rate") / 10.0f);
    const int digits[4] = { hundredths >= 1000 ? (hundredths / 1000) % 10 : -1, (hundredths / 100) % 10,
                            (hundredths / 10) % 10, hundredths % 10 };
    float x = rateX;

    for (int i = 0; i < 4; ++i)
    {
        drawSevenSegment (g, { x, top, digitW, digitH }, digits[i], i == 1, on, off);
        x += digitW + digitGap + (i == 1 ? 8.0f : 0.0f);
    }

    g.setColour (theme::textDim);
    g.setFont (captionFont);
    g.drawText ("RATE  kHz", juce::Rectangle<float> (rateX, top + digitH + 8.0f, rateW, 12.0f), juce::Justification::centred, true);

    // Divider, halfway between the two groups
    g.setColour (juce::Colours::white.withAlpha (0.07f));
    g.fillRect (std::round ((bitsRight + rateX) * 0.5f), r.getY() + 14.0f, 1.0f, 56.0f);

    // Status line: converter mode, and a lamp that follows the signal
    const int modeIndex = juce::jlimit (0, 2, juce::roundToInt (crush.param ("mode")));
    auto status = r.withTrimmedTop (r.getHeight() - 24.0f).reduced (margin, 0.0f).withTrimmedBottom (8.0f);
    g.setColour (on.withAlpha (0.85f));
    g.setFont (theme::monoFont (11.5f, true).withExtraKerningFactor (0.06f));
    g.drawText (modeNames[modeIndex], status, juce::Justification::centredLeft, true);

    g.setColour (theme::textDim);
    g.setFont (captionFont);
    g.drawText ("SMPL", status.withTrimmedRight (14.0f), juce::Justification::centredRight, true);
    auto lamp = juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ status.getRight() - 4.0f, status.getCentreY() });
    g.setColour (theme::ledOn.withAlpha (0.15f + 0.85f * signalLed));
    g.fillEllipse (lamp);

    if (signalLed > 0.1f)
    {
        g.setColour (theme::ledOn.withAlpha (0.3f * signalLed));
        g.fillEllipse (lamp.expanded (2.5f));
    }

    // Glass glint
    g.setColour (juce::Colours::white.withAlpha (0.025f));
    g.fillRoundedRectangle (r.withHeight (r.getHeight() * 0.4f).reduced (1.0f), 5.0f);
}

//==============================================================================
void CrushEditor::paintMeter (juce::Graphics& g, juce::Rectangle<float> r)
{
    g.setColour (theme::textDim);
    g.setFont (theme::font (11.5f, true).withExtraKerningFactor (0.08f));
    g.drawText ("INPUT", r.withWidth (40.0f), juce::Justification::centredLeft, true);
    g.drawText ("CLIP", r.withTrimmedLeft (r.getWidth() - 40.0f).withTrimmedRight (12.0f), juce::Justification::centredRight, true);

    auto ladder = r.withTrimmedLeft (44.0f).withTrimmedRight (46.0f);
    g.setColour (juce::Colour (0xff0b0d11));
    g.fillRoundedRectangle (ladder, 3.0f);

    constexpr int segments = 22;
    constexpr float floorDb = -48.0f;
    const float segW = (ladder.getWidth() - 4.0f) / (float) segments;
    const float dbLevel = juce::Decibels::gainToDecibels (meterLevel, -100.0f);
    const float dbHold = juce::Decibels::gainToDecibels (meterHold, -100.0f);
    const int holdSegment = dbHold > floorDb ? juce::roundToInt (juce::jmap (dbHold, floorDb, 0.0f, 0.0f, (float) segments - 1)) : -1;

    for (int i = 0; i < segments; ++i)
    {
        const float dbSeg = juce::jmap ((float) i, 0.0f, (float) segments - 1, floorDb, 0.0f);
        const auto c = dbSeg > -3.0f ? theme::danger : (dbSeg > -12.0f ? theme::amber : ledGreen);
        const bool on = dbLevel >= dbSeg || i == holdSegment;
        auto seg = juce::Rectangle<float> (ladder.getX() + 2.0f + i * segW, ladder.getY() + 3.0f, segW - 1.5f, ladder.getHeight() - 6.0f);
        g.setColour (on ? c : c.withAlpha (0.12f));
        g.fillRect (seg);
    }

    auto lamp = juce::Rectangle<float> (8.0f, 8.0f).withCentre ({ r.getRight() - 5.0f, r.getCentreY() });
    g.setColour (clipFrames > 0 ? theme::danger : theme::danger.withAlpha (0.18f));
    g.fillEllipse (lamp);

    if (clipFrames > 0)
    {
        g.setColour (theme::danger.withAlpha (0.3f));
        g.fillEllipse (lamp.expanded (2.5f));
    }
}

//==============================================================================
void CrushEditor::paintScope (juce::Graphics& g, juce::Rectangle<float> r)
{
    g.setColour (juce::Colours::black.withAlpha (0.55f));
    g.fillRoundedRectangle (r.expanded (1.5f), 6.0f);
    g.setColour (juce::Colour (0xff090b10));
    g.fillRoundedRectangle (r, 5.0f);

    // The plot keeps clear of the label strips at the top and bottom
    const auto inner = r.reduced (8.0f).withTrimmedTop (10.0f).withTrimmedBottom (10.0f);
    const float midY = inner.getCentreY();
    const float halfH = inner.getHeight() * 0.5f;
    const float bits = juce::jlimit (4.0f, 16.0f, crush.param ("bits"));
    const float rate = juce::jlimit (4000.0f, 48000.0f, crush.param ("rate"));
    const int levels = (int) std::ldexp (1.0f, (int) bits - 1);
    const float amplitude = 0.85f;

    // Grid: the quantisation levels, on the same scale as the trace. When they are closer
    // than 4 px, every Nth level (N a power of two, so the lines stay on levels)
    {
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        const float spacing = halfH / (float) levels;
        const int every = spacing >= 4.0f ? 1 : juce::nextPowerOfTwo ((int) std::ceil (4.0f / spacing));
        const int reach = (int) std::ceil (amplitude * (float) levels);

        for (int l = -(reach / every) * every; l <= reach; l += every)
            g.drawHorizontalLine (juce::roundToInt (midY - (float) l * spacing), inner.getX(), inner.getRight());

        for (int v = 1; v < 8; ++v)
            g.drawVerticalLine (juce::roundToInt (inner.getX() + v * inner.getWidth() / 8.0f), inner.getY(), inner.getBottom());
    }

    // A 1 kHz tone as the converter sees it: holds of 1000/rate periods, each centred on
    // the instant it represents, truncated to `bits`. With `alias` down, the converter
    // rebuilds a smooth waveform through the same truncated values
    const float periods = 1.5f;
    const float holdLen = 1000.0f / rate;                       // in periods of the tone
    const float pxPerPeriod = inner.getWidth() / periods;
    const float stepWidth = holdLen * pxPerPeriod;
    const float aliasAmount = juce::jlimit (0.0f, 1.0f, crush.param ("alias") * 0.01f);
    const float steps = juce::jlimit (0.0f, 1.0f, juce::jmap (aliasAmount, 0.3f, 0.7f, 0.0f, 1.0f));
    auto yFor = [&] (float v) { return midY - v * halfH; };
    auto held = [&] (float t) { return std::floor (amplitude * std::sin (juce::MathConstants<float>::twoPi * (t + scopePhase)) * (float) levels) / (float) levels; };

    juce::Path stairs, smooth;
    std::vector<juce::Point<float>> centres;
    const int first = (int) std::floor (-2.0f / holdLen) - 1;
    const int last = (int) std::ceil ((periods + 2.0f * holdLen) / holdLen) + 1;

    for (int k = first; k <= last; ++k)
    {
        const float t0 = (float) k * holdLen;
        const float y = std::round (yFor (held (t0 + 0.5f * holdLen))) + 0.5f;
        const float x0 = std::round (inner.getX() + t0 * pxPerPeriod) + 0.5f;
        const float x1 = std::round (inner.getX() + (t0 + holdLen) * pxPerPeriod) + 0.5f;

        if (k == first)
            stairs.startNewSubPath (x0, y);
        else
            stairs.lineTo (x0, y);

        stairs.lineTo (x1, y);
        centres.push_back ({ inner.getX() + (t0 + 0.5f * holdLen) * pxPerPeriod, yFor (held (t0 + 0.5f * holdLen)) });
    }

    // Catmull-Rom through the hold centres: the rebuilt waveform keeps the truncation
    if (centres.size() >= 4)
    {
        smooth.startNewSubPath (centres[1]);

        for (size_t i = 1; i + 2 < centres.size(); ++i)
        {
            const auto p0 = centres[i - 1], p1 = centres[i], p2 = centres[i + 1], p3 = centres[i + 2];
            smooth.cubicTo (p1 + (p2 - p0) / 6.0f, p2 - (p3 - p1) / 6.0f, p2);
        }
    }

    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (inner.toNearestInt());

        if (steps < 0.99f)
        {
            g.setColour (theme::teal.withAlpha (0.2f * (1.0f - steps)));
            g.strokePath (smooth, juce::PathStrokeType (3.5f));
            g.setColour (theme::teal.withAlpha (0.9f * (1.0f - steps)));
            g.strokePath (smooth, juce::PathStrokeType (1.4f));
        }

        if (steps > 0.01f)
        {
            // The glow only where steps are wide enough to stay crisp under it
            if (stepWidth >= 6.0f)
            {
                g.setColour (theme::violet.withAlpha (0.22f * steps));
                g.strokePath (stairs, juce::PathStrokeType (3.5f, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
            }

            g.setColour (theme::violet.withAlpha (steps));
            g.strokePath (stairs, juce::PathStrokeType (1.3f, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
        }
    }

    g.setColour (theme::textDim);
    g.setFont (theme::monoFont (11.5f).withExtraKerningFactor (0.04f));
    const auto labels = r.reduced (8.0f, 3.0f);
    g.drawText ("SCOPE", labels, juce::Justification::topLeft, true);
    g.drawText ("1 kHz", labels, juce::Justification::topRight, true);
    g.drawText (juce::String (juce::roundToInt (bits)) + " BIT", labels, juce::Justification::bottomLeft, true);
    g.drawText ("ALIAS " + juce::String (juce::roundToInt (aliasAmount * 100.0f)) + "%", labels, juce::Justification::bottomRight, true);

    g.setColour (juce::Colours::white.withAlpha (0.04f));
    g.drawRoundedRectangle (r.reduced (1.0f), 4.0f, 1.0f);
}

//==============================================================================
void CrushEditor::paintPads (juce::Graphics& g, juce::Rectangle<float> r)
{
    // A recessed well holding eight square rubber pads, lit violet from underneath
    constexpr int count = 8;
    const float gap = 6.0f, labelH = 13.0f;
    const float padW = (r.getWidth() - gap * (count - 1)) / (float) count;
    const float padH = padW + 2.0f;
    const auto well = juce::Rectangle<float> (r.getX(), r.getCentreY() - (padH + labelH) * 0.5f, r.getWidth(), padH);

    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.fillRoundedRectangle (well.expanded (4.0f, 4.0f), 7.0f);
    g.setColour (juce::Colours::white.withAlpha (0.04f));
    g.drawRoundedRectangle (well.expanded (4.0f, 4.0f).translated (0.0f, 0.5f), 7.0f, 1.0f);

    for (int i = 0; i < count; ++i)
    {
        const auto pad = juce::Rectangle<float> (well.getX() + i * (padW + gap), well.getY(), padW, padH);
        const float glow = juce::jlimit (0.0f, 1.0f, padGlow[i]);

        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.fillRoundedRectangle (pad.translated (0.0f, 1.5f), 5.0f);

        // Bloom: light escaping round the pad's edge
        if (glow > 0.02f)
        {
            g.setGradientFill (juce::ColourGradient (theme::violet.withAlpha (0.45f * glow), pad.getCentreX(), pad.getCentreY(),
                                                     theme::violet.withAlpha (0.0f), pad.getCentreX(), pad.getY() - 7.0f, true));
            g.fillRoundedRectangle (pad.expanded (7.0f), 10.0f);
        }

        // Rubber: slightly domed, lighter at the top edge
        g.setGradientFill (juce::ColourGradient (padFaceTop, pad.getX(), pad.getY(), padFaceBottom, pad.getX(), pad.getBottom(), false));
        g.fillRoundedRectangle (pad, 5.0f);

        // Backlight through the rubber: a saturated violet field with a hot core, opaque
        // enough even when held that it reads as lit rather than greyed
        if (glow > 0.02f)
        {
            const auto deep = theme::violet.withMultipliedSaturation (1.6f);
            g.setGradientFill (juce::ColourGradient (deep.brighter (0.55f).withAlpha (juce::jmin (1.0f, 0.3f + 0.9f * glow)), pad.getCentreX(), pad.getCentreY(),
                                                     deep.withAlpha (0.2f + 0.6f * glow), pad.getX(), pad.getY(), true));
            g.fillRoundedRectangle (pad, 5.0f);
        }

        g.setColour (juce::Colours::white.withAlpha (0.09f));
        g.drawHorizontalLine ((int) pad.getY() + 1, pad.getX() + 4.0f, pad.getRight() - 4.0f);
        g.setColour (juce::Colours::white.withAlpha (0.03f));
        g.fillRoundedRectangle (pad.reduced (3.0f).withHeight (pad.getHeight() * 0.35f), 3.0f);
        g.setColour (theme::outline);
        g.drawRoundedRectangle (pad.reduced (0.5f), 5.0f, 1.0f);

        g.setColour (glow > 0.3f ? theme::text : theme::textDim);
        g.setFont (theme::monoFont (12.0f));
        g.drawText (juce::String (i + 1), pad.withY (pad.getBottom() + 2.0f).withHeight (15.0f), juce::Justification::centred, true);
    }
}

//==============================================================================
void CrushEditor::animate (float seconds)
{
    // A zero read means no audio block arrived since the last frame (a large host
    // buffer, or a frame that came early), not silence: hold the last level briefly
    if (inputLevel > 0.0f)
    {
        emptyFrames = 0;
        heldInput = inputLevel;
    }
    else if (++emptyFrames > 3)
    {
        heldInput = 0.0f;
    }

    // The meter shows what the converter gets: the input after Drive, and CLIP where the
    // converter's input stage starts to fold (inputClip's knee)
    const float level = heldInput * juce::Decibels::decibelsToGain (crush.param ("drive") * 0.18f);
    meterLevel = level > meterLevel ? level : meterLevel * std::pow (10.0f, -1.0f * seconds);    // 20 dB/s

    if (level >= meterHold)
    {
        meterHold = level;
        holdFrames = 30;
    }
    else if (--holdFrames < 0)
    {
        meterHold *= std::pow (10.0f, -1.0f * seconds);
    }

    clipFrames = level >= 0.75f ? 40 : clipFrames - 1;
    signalLed = heldInput > 0.003f ? 1.0f : signalLed * 0.8f;

    // Pads: each hit (a fast envelope jumping over a slow one) lights the next pad; while
    // the signal lasts, the pad that took the last hit keeps a soft glow with its level
    // A hit is a rising edge as well, so a steady tone lights one pad, not a run of them
    // while the slow envelope catches up
    const bool rising = heldInput > lastLevel * 1.25f + 0.005f;
    lastLevel = heldInput;
    fastEnv = std::max (heldInput, fastEnv * 0.6f);
    slowEnv += (fastEnv - slowEnv) * 0.08f;
    sinceOnset += seconds;

    if (rising && fastEnv > slowEnv * 1.5f + 0.01f && sinceOnset > 0.09f)
    {
        activePad = (activePad + 1) % 8;
        padGlow[activePad] = 1.0f;
        sinceOnset = 0.0f;
    }

    const float decay = std::exp (-seconds / 0.18f);

    for (auto& glow : padGlow)
        glow *= decay;

    if (activePad >= 0 && heldInput > 0.003f)
    {
        const float sustain = 0.4f * juce::jlimit (0.0f, 1.0f, (juce::Decibels::gainToDecibels (heldInput) + 48.0f) / 48.0f);
        padGlow[activePad] = std::max (padGlow[activePad], sustain);
    }

    scopePhase = std::fmod (scopePhase + seconds * 0.12f, 1.0f);
    repaint (graphic);
}
