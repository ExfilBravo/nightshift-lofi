#include "Vinyl.h"
#include "common/ScaledEditor.h"

using namespace lofi;
using namespace lofi::params;

//==============================================================================
VinylProcessor::VinylProcessor()
    : LofiProcessor ({ "Nightshift Vinyl", "turntable", theme::rose }, createLayout())
{
    speedParam   = apvts.getRawParameterValue ("speed");
    noiseParam   = apvts.getRawParameterValue ("noise");
    crackleParam = apvts.getRawParameterValue ("crackle");
    wearParam    = apvts.getRawParameterValue ("wear");
    warpParam    = apvts.getRawParameterValue ("warp");
    widthParam   = apvts.getRawParameterValue ("width");
    jassert (speedParam != nullptr && noiseParam != nullptr && crackleParam != nullptr
             && wearParam != nullptr && warpParam != nullptr && widthParam != nullptr);

    // The surface noise doesn't stop with the input (only with the host's transport), so
    // hosts that suspend plug-ins on silent input must keep calling this one: kInfiniteTail
    setTailSeconds (std::numeric_limits<double>::infinity());
}

LofiProcessor::Layout VinylProcessor::createLayout()
{
    Layout layout;
    layout.add (choice   ("speed",   "Speed",   { "33 rpm", "45 rpm" }, 0));
    layout.add (decibels ("noise",   "Noise",   -90.0f, -30.0f, -60.0f));
    layout.add (percent  ("crackle", "Crackle", 30.0f));
    layout.add (percent  ("wear",    "Wear",    35.0f));
    layout.add (percent  ("warp",    "Warp",    15.0f));
    layout.add (percent  ("width",   "Width",   70.0f));
    addCommonParameters (layout);
    return layout;
}

juce::AudioProcessorEditor* VinylProcessor::createEditor()
{
    return new lofi::ScaledEditor (*this, new VinylEditor (*this));
}

//==============================================================================
void VinylProcessor::prepareDsp (const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate;
    numChannels = (int) juce::jlimit (1u, 2u, spec.numChannels);

    // Grit runs 2x oversampled; integer latency so the dry path lines up exactly
    oversampling = std::make_unique<juce::dsp::Oversampling<float>> ((size_t) numChannels, 1,
                                                                      juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, false, true);
    oversampling->initProcessing ((size_t) std::max (chunkSize, (int) spec.maximumBlockSize));
    oversampling->reset();

    // The warp swings the read head about +-5.5 ms at 33 rpm and 30 cents, so the
    // head sits 6.5 ms behind the write head; that offset is reported as latency
    baseDelay = std::round (0.0065 * sampleRate);
    maxDelay = baseDelay * 2.0 - 4.0;
    delayPos = baseDelay;
    leak = 1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * 0.05 / sampleRate);
    latency = (int) std::round (oversampling->getLatencyInSamples()) + (int) baseDelay;

    for (auto& k : ch)
    {
        k.hp1.prepare (sampleRate);
        k.hp2.prepare (sampleRate);
        k.lift.prepare (sampleRate);
        k.lp.prepare (sampleRate);
        k.delay.prepare (sampleRate, 14.0f);
    }

    sideHp.prepare (sampleRate);
    sideHp.setCutoff (2000.0f);

    const auto norms = vinyl::Static::calibrate (sampleRate);
    staticMid.prepare (sampleRate, 0x5eedb0b1u);
    staticMid.setNorms (norms);
    staticSide.prepare (sampleRate, 0x7a1fd00du);
    staticSide.setNorms (norms);
    crackle.prepare (sampleRate, 0xc4ac71e5u);
    warp.prepare (sampleRate, 0x3a9b4c2du);

    for (auto* s : { &wearS, &warpCentsS, &widthS, &noiseGainS, &popPeakS, &lidS, &tickPeakS })
        lofi::dsp::prepareSmoothed (*s, sampleRate, 0.0f, 0.03);

    lofi::dsp::prepareSmoothed (rateS, sampleRate, 0.5556f, 0.4);   // the platter takes a moment to change speed

    lastWear = -1.0f;
    primed = false;
    fadeLength = std::max (1, (int) std::round (0.002 * sampleRate));
    fadePos = fadeLength;   // no fade on a fresh start: renders begin settled
    snapSmoothers();
    platterWobble.store (1.0f);
    platterPhase.store (0.0f);
}

void VinylProcessor::snapSmoothers()
{
    updateTargets();

    for (auto* s : { &wearS, &warpCentsS, &widthS, &rateS, &noiseGainS, &popPeakS, &lidS, &tickPeakS })
        s->setCurrentAndTargetValue (s->getTargetValue());

    setWear (wearS.getCurrentValue());
    lastWear = wearS.getCurrentValue();
}

void VinylProcessor::resetDsp()
{
    // Audio thread, leaving bypass: return the wet path to silence without allocating
    for (auto& k : ch)
    {
        k.hp1.reset();
        k.hp2.reset();
        k.lift.reset();
        k.lp.reset();
        k.delay.reset();
    }

    sideHp.reset();

    if (oversampling != nullptr)
        oversampling->reset();

    delayPos = baseDelay;
    crackle.silence();
    snapSmoothers();   // knobs moved while bypassed start where they are, not on a ramp
    primed = true;

    // The first samples into the cleared chain ramp in over 2 ms (raised cosine), so the
    // highpasses and the oversampler never see a step from silence to mid-waveform
    fadePos = 0;
}

void VinylProcessor::updateTargets()
{
    wearS.setTargetValue (wearParam->load() * 0.01f);
    warpCentsS.setTargetValue (warpParam->load() * 0.30f);
    widthS.setTargetValue (widthParam->load() * 0.01f);
    rateS.setTargetValue (speedParam->load() >= 0.5f ? 0.75f : 0.5556f);

    const float noiseDb = noiseParam->load();
    noiseGainS.setTargetValue (lofi::dsp::dbToGain (noiseDb));
    // Pops are launched 14 dB over the static and meet a lid at +11.7 dB, so every pop
    // reads about 12 dB over the bed and none, even one on a static peak, gets past it.
    // All three follow the noise knob over its whole range: at -90 the pops are at -78
    popPeakS.setTargetValue (lofi::dsp::dbToGain (noiseDb + 14.0f));
    lidS.setTargetValue (lofi::dsp::dbToGain (noiseDb + 11.7f));
    tickPeakS.setTargetValue (lofi::dsp::dbToGain (noiseDb - 1.0f));

    // Pop density 25 * 1.8 c^2 / (c + 0.8): 0.5/s at 10 %, 3.7/s at the default 30 %,
    // 8.7/s at 50 %, 25/s at 100 % (steep at the bottom, so low settings stay sparse)
    const float c = crackleParam->load() * 0.01f;
    const bool on = c > 0.0005f;
    const float pops = 25.0f * 1.8f * c * c / (c + 0.8f);
    crackle.setRates (on ? pops : 0.0f, on ? 25.0f + 175.0f * c : 0.0f);
}

void VinylProcessor::setWear (float wear)
{
    const float hpHz = 30.0f + 50.0f * std::pow (wear, 1.3f);   // 30 Hz .. 80 Hz, 43 Hz at the default so a kick keeps its weight
    const float lpHz = 20000.0f * std::pow (0.3f, wear);          // 20 kHz .. 6 kHz

    for (auto& k : ch)
    {
        k.hp1.setCutoff (hpHz);
        k.hp2.set (vinyl::Svf::Mode::highpass, hpHz, 1.0f);   // with hp1: a 3rd-order rumble cut
        k.lift.set (vinyl::Svf::Mode::bell, 3000.0f, 0.9f, 2.5f * wear);
        k.lp.set (vinyl::Svf::Mode::lowpass, lpHz, 0.65f);
    }

    gritDrive = 1.0f + 1.5f * wear;
    gritBlend = 0.6f * wear;
}

void VinylProcessor::processDsp (juce::AudioBuffer<float>& buffer)
{
    const int numCh = std::min (numChannels, buffer.getNumChannels());
    const int n = buffer.getNumSamples();
    updateTargets();

    if (! primed)
    {
        // First block after prepare: nothing to ramp from, so land on the settings at once
        for (auto* s : { &wearS, &warpCentsS, &widthS, &rateS, &noiseGainS, &popPeakS, &lidS, &tickPeakS })
            s->setCurrentAndTargetValue (s->getTargetValue());

        primed = true;
    }

    float lastError = 0.0f;

    for (int start = 0; start < n; start += chunkSize)
    {
        const int len = std::min (chunkSize, n - start);
        const float wear = wearS.skip (len);
        const float cents = warpCentsS.skip (len);
        const float width = widthS.skip (len);
        const float rate = rateS.skip (len);
        const float noiseGain = noiseGainS.skip (len);
        const float popPeak = popPeakS.skip (len);
        const float lid = lidS.skip (len);
        const float tickPeak = tickPeakS.skip (len);

        if (wear != lastWear)
        {
            setWear (wear);
            lastWear = wear;
        }

        // 0. Just out of bypass: ease the input into the cleared chain
        if (fadePos < fadeLength)
        {
            const int count = std::min (len, fadeLength - fadePos);

            for (int i = 0; i < count; ++i)
            {
                const float g = 0.5f - 0.5f * std::cos (vinyl::kPi * (float) (fadePos + i + 1) / (float) fadeLength);

                for (int c = 0; c < numCh; ++c)
                    buffer.getWritePointer (c, start)[i] *= g;
            }

            fadePos += count;
        }

        // 1. Worn groove: rumble cut and a touch of presence
        for (int c = 0; c < numCh; ++c)
        {
            auto& k = ch[c];
            auto* d = buffer.getWritePointer (c, start);

            for (int i = 0; i < len; ++i)
                d[i] = k.lift.process (k.hp2.process (k.hp1.highpass (d[i])));
        }

        // 2. Grit: light odd-harmonic tracing distortion, 2x oversampled (identity at wear 0,
        //    but always in the path so the latency never moves)
        {
            juce::dsp::AudioBlock<float> block (buffer.getArrayOfWritePointers(), (size_t) numCh, (size_t) start, (size_t) len);
            auto up = oversampling->processSamplesUp (block);
            const float drive = gritDrive, blend = gritBlend, inv = 1.0f / gritDrive;

            for (size_t c = 0; c < up.getNumChannels(); ++c)
            {
                auto* d = up.getChannelPointer (c);

                for (size_t i = 0; i < up.getNumSamples(); ++i)
                    d[i] += blend * (lofi::dsp::shape::tanhFast (d[i] * drive) * inv - d[i]);
            }

            oversampling->processSamplesDown (block);
        }

        // 3. HF loss, then the warp: one speed error for both channels
        for (int i = 0; i < len; ++i)
        {
            const float e = warp.next (rate, cents);
            delayPos += (baseDelay - delayPos) * leak - (double) e;
            delayPos = juce::jlimit (2.0, maxDelay, delayPos);
            const float pos = (float) delayPos;
            lastError = e;

            for (int c = 0; c < numCh; ++c)
            {
                auto& k = ch[c];
                auto* d = buffer.getWritePointer (c, start);
                k.delay.push (k.lp.process (d[i]));
                d[i] = k.delay.read (pos);
            }
        }

        // 4. Width: narrow the side signal, the highs a little more (cartridge crosstalk)
        if (numCh == 2)
        {
            auto* l = buffer.getWritePointer (0, start);
            auto* r = buffer.getWritePointer (1, start);
            const float cross = 0.5f * (1.0f - width);

            for (int i = 0; i < len; ++i)
            {
                const float m = 0.5f * (l[i] + r[i]), s = 0.5f * (l[i] - r[i]);
                const float side = width * (s - cross * sideHp.highpass (s));
                l[i] = m + side;
                r[i] = m - side;
            }
        }

        // 5. Static and crackle, after the wet path so the mix fades them too. The static
        //    is mostly vertical (anti-phase) like a real groove's, so it sits wide and
        //    leaves the centre clean; it folds to mono with width. A bounded ceiling on the
        //    whole surface bus keeps every pop, even one riding a static peak, within
        //    12 dB of the static's RMS. The surface goes quiet while the host is stopped
        {
            const float spread = juce::jmin (1.0f, width * (1.0f / 0.7f));
            const float b = numCh == 2 ? 0.866f * spread : 0.0f;
            const float a = std::sqrt (std::max (0.0f, 1.0f - b * b));
            auto* l = buffer.getWritePointer (0, start);
            auto* r = buffer.getWritePointer (numCh - 1, start);

            for (int i = 0; i < len; ++i)
            {
                const float nm = staticMid.next(), ns = staticSide.next();
                float cl, cr;
                crackle.next (cl, cr, popPeak, tickPeak, numCh == 2 ? spread : 0.0f);
                const float gate = transportGateAt (start + i);

                if (numCh == 2)
                {
                    l[i] += gate * vinyl::ceiling (noiseGain * (a * nm + b * ns) + cl, lid);
                    r[i] += gate * vinyl::ceiling (noiseGain * (a * nm - b * ns) + cr, lid);
                }
                else
                {
                    l[i] += gate * vinyl::ceiling (noiseGain * nm + cl, lid);
                }
            }
        }
    }

    platterWobble.store (1.0f + lastError);
    platterPhase.store (warp.getPhase());
}


//==============================================================================
SpeedSelector::SpeedSelector (juce::AudioProcessorValueTreeState& state, const juce::String& paramId)
    : parameter (*state.getParameter (paramId)),
      attachment (parameter, [this] (float v) { current = juce::roundToInt (v); repaint(); })
{
    setTooltip ("Platter speed: a 33 1/3 rpm LP or a 45 rpm single");
    attachment.sendInitialUpdate();
}

juce::Rectangle<float> SpeedSelector::buttonArea (int index) const
{
    auto r = getLocalBounds().toFloat().withTrimmedTop (16.0f).withTrimmedBottom (16.0f);
    const float w = 42.0f, h = 46.0f, gap = 6.0f;
    const float x0 = r.getCentreX() - w - gap * 0.5f;
    return juce::Rectangle<float> (x0 + index * (w + gap), r.getCentreY() - h * 0.5f, w, h);
}

int SpeedSelector::buttonAt (juce::Point<float> p) const
{
    for (int i = 0; i < 2; ++i)
        if (buttonArea (i).expanded (3.0f).contains (p))
            return i;

    return -1;
}

void SpeedSelector::setHover (int index)
{
    if (index == hover)
        return;

    hover = index;
    setMouseCursor (index >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    repaint();
}

void SpeedSelector::mouseMove (const juce::MouseEvent& e)    { setHover (buttonAt (e.position)); }
void SpeedSelector::mouseExit (const juce::MouseEvent&)      { setHover (-1); }

void SpeedSelector::mouseDown (const juce::MouseEvent& e)
{
    const int i = buttonAt (e.position);

    if (i >= 0 && i != current)
        attachment.setValueAsCompleteGesture ((float) i);
}

void SpeedSelector::paint (juce::Graphics& g)
{
    using namespace theme;
    g.setColour (textDim.brighter (0.15f));
    g.setFont (font (11.5f, true).withExtraKerningFactor (0.06f));
    g.drawText ("SPEED", getLocalBounds().removeFromTop (16), juce::Justification::centred, true);

    for (int i = 0; i < 2; ++i)
    {
        const bool on = i == current;
        auto b = buttonArea (i);

        if (on)
        {
            b = b.translated (0.0f, 1.0f);   // latched down: no shadow, darker cap
        }
        else
        {
            g.setColour (juce::Colours::black.withAlpha (0.5f));
            g.fillRoundedRectangle (b.translated (0.0f, 1.5f), 4.0f);
        }

        auto top = juce::Colour (on ? 0xff22262f : 0xff3a4050), bottom = juce::Colour (on ? 0xff1a1d25 : 0xff272b36);

        if (i == hover && ! on)
        {
            top = top.brighter (0.06f);
            bottom = bottom.brighter (0.06f);
        }

        g.setGradientFill (juce::ColourGradient (top, b.getX(), b.getY(), bottom, b.getX(), b.getBottom(), false));
        g.fillRoundedRectangle (b, 4.0f);
        g.setColour (outline);
        g.drawRoundedRectangle (b, 4.0f, 1.0f);
        g.setColour (on ? juce::Colours::black.withAlpha (0.35f) : juce::Colours::white.withAlpha (0.08f));
        g.drawHorizontalLine ((int) b.getY() + 1, b.getX() + 3.0f, b.getRight() - 3.0f);

        auto lamp = juce::Rectangle<float> (b.getWidth() - 14.0f, 4.0f).withCentre ({ b.getCentreX(), b.getY() + 8.0f });
        g.setColour (on ? amber : amber.withAlpha (0.12f));
        g.fillRoundedRectangle (lamp, 2.0f);

        if (on)
        {
            g.setColour (amber.withAlpha (0.25f));
            g.fillRoundedRectangle (lamp.expanded (3.0f, 2.5f), 3.0f);
        }

        g.setColour (on ? text : (i == hover ? text.withAlpha (0.85f) : textDim.brighter (0.25f)));
        g.setFont (font (15.0f, true));
        g.drawText (i == 0 ? "33" : "45", b.withTrimmedTop (10.0f), juce::Justification::centred, true);
    }

    g.setColour (text);
    g.setFont (monoFont (12.0f));
    g.drawText (current == 0 ? "33 rpm" : "45 rpm", getLocalBounds().removeFromBottom (16), juce::Justification::centred, true);
}

//==============================================================================
namespace
{
    using vinyl::kPi;
    using vinyl::kTwoPi;

    // Deck geometry in the graphic's own coordinates (300 x 368)
    const juce::Point<float> kPlatter { 138.0f, 186.0f };
    const juce::Point<float> kPivot   { 262.0f, 48.0f };
    constexpr float kPlatterR = 120.0f, kMatR = 109.0f;

    // A fixed-length arm: tube from the bearing to the headshell, which is offset
    // toward the groove's tangent; the stylus sits under the end of the headshell
    constexpr float kArmTube = 172.0f, kHeadLength = 22.0f, kHeadOffset = 0.33f;

    struct Disc { float record, label, hole, groove; };
    constexpr Disc kLp     { 108.0f, 36.0f, 0.0f, 76.0f };    // 12-inch LP at 33 1/3
    constexpr Disc kSingle { 64.0f, 31.0f, 13.5f, 50.0f };    // 7-inch single at 45: big hole, adapter

    const Disc& discFor (bool fast)     { return fast ? kSingle : kLp; }

    juce::Point<float> polar (float angle, float radius)
    {
        return { std::cos (angle) * radius, std::sin (angle) * radius };
    }

    juce::Rectangle<float> circle (juce::Point<float> centre, float radius)
    {
        return juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre);
    }

    /** Text set along a circle round the origin: across the top reading clockwise, or
        across the bottom reading left to right with the letters' tops to the centre,
        as printed on record labels. `radius` is the baseline. */
    void drawArcText (juce::Graphics& g, const juce::String& text, const juce::Font& font, float radius, bool bottom)
    {
        juce::GlyphArrangement glyphs;
        glyphs.addLineOfText (font, text, 0.0f, 0.0f);
        const int n = glyphs.getNumGlyphs();

        if (n == 0)
            return;

        const float left = glyphs.getGlyph (0).getLeft();
        const float total = glyphs.getGlyph (n - 1).getRight() - left;

        for (int i = 0; i < n; ++i)
        {
            const auto& glyph = glyphs.getGlyph (i);
            const float mid = 0.5f * (glyph.getLeft() + glyph.getRight());
            const float along = (mid - left - total * 0.5f) / radius;
            const float theta = bottom ? kPi * 0.5f - along : -kPi * 0.5f + along;
            const float rotation = bottom ? theta - kPi * 0.5f : theta + kPi * 0.5f;
            glyph.draw (g, juce::AffineTransform::translation (-mid, 0.0f).rotated (rotation).translated (polar (theta, radius)));
        }
    }

    /** A crescent moon, the Nightshift mark. */
    void drawMoon (juce::Graphics& g, juce::Point<float> centre, float r)
    {
        juce::Path moon;
        moon.addEllipse (circle (centre, r));
        moon.addEllipse (circle (centre + juce::Point<float> (r * 0.45f, -r * 0.3f), r * 0.82f));
        moon.setUsingNonZeroWinding (false);
        g.fillPath (moon);
    }
}

//==============================================================================
VinylEditor::VinylEditor (VinylProcessor& p)
    : LofiEditor (p), vinyl (p)
{
    auto& state = p.getState();
    speedParam = state.getRawParameterValue ("speed");
    warpParam = state.getRawParameterValue ("warp");
    bypassParam = state.getRawParameterValue ("bypass");
    jassert (speedParam != nullptr && warpParam != nullptr && bypassParam != nullptr);

    spin = power = bypassParam->load() >= 0.5f ? 0.0f : 1.0f;
    grooveRadius = discFor (is45()).groove;

    speed = std::make_unique<SpeedSelector> (state, "speed");
    addAndMakeVisible (*speed);

    // One accent per section: rose for the surface, amber for the record, violet out
    noiseKnobs  = { &addKnob ("noise", "Surface", theme::rose), &addKnob ("crackle", "Crackle", theme::rose) };
    recordKnobs = { &addKnob ("wear", "Wear"), &addKnob ("warp", "Warp"), &addKnob ("width", "Width") };
    outKnobs    = { &addKnob ("mix", "Mix", theme::violet), &addKnob ("output", "Output", theme::violet, true) };

    noiseKnobs[0]->getSlider().setTooltip ("Surface static under everything, dBFS RMS");
    noiseKnobs[1]->getSlider().setTooltip ("Pops and ticks: about 0.5 per second at 10 %, 25 at 100 %");
    recordKnobs[0]->getSlider().setTooltip ("Worn groove: rumble cut, duller highs, a little grit");
    recordKnobs[1]->getSlider().setTooltip ("Warped record: pitch bends once per revolution, up to 30 cents");
    recordKnobs[2]->getSlider().setTooltip ("Stereo width: 0 is mono, 100 leaves the stereo untouched");
    outKnobs[0]->getSlider().setTooltip ("Dry / wet; the surface noise is part of the wet");
    outKnobs[1]->getSlider().setTooltip ("Output level");

    setSize (820, 460);
}

void VinylEditor::layoutBody (juce::Rectangle<int> body)
{
    // One gutter everywhere; the panels fill their cells, flush with the deck
    constexpr int gap = 10;
    graphic = body.removeFromLeft (300);
    body.removeFromLeft (gap);

    platterCentre = graphic.getPosition().toFloat() + kPlatter;
    armPivot = graphic.getPosition().toFloat() + kPivot;

    auto top = body.removeFromTop ((body.getHeight() - gap) / 2);
    body.removeFromTop (gap);
    noiseArea = top.removeFromLeft ((top.getWidth() - gap) / 2);
    top.removeFromLeft (gap);
    outArea = top;
    recordArea = body;

    // Four columns shared by both rows, so RECORD's controls sit under the knobs above
    const int columns[] = { noiseArea.getX() + noiseArea.getWidth() / 4, noiseArea.getX() + noiseArea.getWidth() * 3 / 4,
                            outArea.getX() + outArea.getWidth() / 4,     outArea.getX() + outArea.getWidth() * 3 / 4 };

    auto place = [] (juce::Component& c, juce::Rectangle<int> panel, int centreX, int width)
    {
        const auto cell = panel.withTrimmedTop (22);
        c.setBounds (juce::Rectangle<int> (width, Knob::defaultSize + 32).withCentre ({ centreX, cell.getCentreY() }));
    };

    place (*noiseKnobs[0], noiseArea, columns[0], Knob::defaultSize);
    place (*noiseKnobs[1], noiseArea, columns[1], Knob::defaultSize);
    place (*outKnobs[0], outArea, columns[2], Knob::defaultSize);
    place (*outKnobs[1], outArea, columns[3], Knob::defaultSize);
    place (*speed, recordArea, columns[0], 104);

    for (size_t i = 0; i < 3; ++i)
        place (*recordKnobs[i], recordArea, columns[i + 1], Knob::defaultSize);
}

void VinylEditor::drawSectionPanel (juce::Graphics& g, juce::Rectangle<int> area)
{
    auto r = area.toFloat();
    g.setColour (juce::Colours::black.withAlpha (0.17f));
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (juce::Colours::white.withAlpha (0.035f));
    g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, 1.0f);
}

void VinylEditor::paintBody (juce::Graphics& g, juce::Rectangle<int>)
{
    const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();
    const bool fast = is45();

    if (deckImage.isNull() || deckScale != scale || deckSpeed != (int) fast || deckBounds != graphic)
        renderDeck (scale, fast);

    if (labelImage.isNull() || labelScale != scale || labelSpeed != (int) fast)
        renderLabel (scale, fast);

    g.drawImage (deckImage, graphic.toFloat());
    drawRecord (g);
    drawTonearm (g);
    drawLamp (g);

    for (auto& [area, title, colour] : { std::tuple (noiseArea, "Noise", theme::rose),
                                         std::tuple (outArea, "Out", theme::violet),
                                         std::tuple (recordArea, "Record", theme::amber) })
    {
        drawSectionPanel (g, area);
        drawSectionTitle (g, area.reduced (14, 0).withTrimmedTop (10).withHeight (16), title, colour);
    }
}

void VinylEditor::renderDeck (float scale, bool fast)
{
    const int w = graphic.getWidth(), h = graphic.getHeight();
    deckImage = juce::Image (juce::Image::ARGB, std::max (1, juce::roundToInt ((float) w * scale)),
                             std::max (1, juce::roundToInt ((float) h * scale)), true);
    deckScale = scale;
    deckSpeed = (int) fast;
    deckBounds = graphic;

    juce::Graphics g (deckImage);
    g.addTransform (juce::AffineTransform::scale (scale));

    const auto& disc = discFor (fast);
    const auto c = kPlatter;
    const auto pivot = kPivot;
    const float R = kPlatterR;

    // Plinth: dark lacquer with a soft top light
    auto plinth = juce::Rectangle<float> (0.0f, 0.0f, (float) w, (float) h);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff222736), 0.0f, 0.0f, juce::Colour (0xff10131a), 0.0f, (float) h, false));
    g.fillRoundedRectangle (plinth, 8.0f);
    g.setColour (juce::Colours::white.withAlpha (0.05f));
    g.drawRoundedRectangle (plinth.reduced (1.5f), 7.0f, 1.0f);
    g.setColour (theme::outline);
    g.drawRoundedRectangle (plinth.reduced (0.5f), 8.0f, 1.0f);

    // Platter well and platter
    g.setColour (juce::Colour (0xff090a0d));
    g.fillEllipse (circle (c + juce::Point<float> (0.0f, 3.0f), R + 7.0f));
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff4c5262), c.x - R * 0.6f, c.y - R * 0.6f,
                                             juce::Colour (0xff23272f), c.x + R * 0.7f, c.y + R * 0.7f, true));
    g.fillEllipse (circle (c, R));
    g.setColour (juce::Colours::white.withAlpha (0.14f));
    g.drawEllipse (circle (c, R - 1.0f), 1.2f);
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.drawEllipse (circle (c, R), 1.0f);

    // Rubber mat with fine ridges; a 7-inch leaves a wide ring of it showing
    g.setColour (juce::Colour (0xff14161c));
    g.fillEllipse (circle (c, kMatR));

    for (float r = kMatR - 2.0f; r > disc.record; r -= 2.5f)
    {
        g.setColour (juce::Colours::white.withAlpha (0.03f));
        g.drawEllipse (circle (c, r), 0.8f);
    }

    // The record: the grooves are rotationally symmetric, so they live here; the label,
    // dust and strobe ring turn on top each frame
    const float recordR = disc.record, labelR = disc.label;
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.fillEllipse (circle (c + juce::Point<float> (0.0f, 1.5f), recordR + 1.0f));
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1a1a20), c.x, c.y, juce::Colour (0xff08080b), c.x + recordR, c.y, true));
    g.fillEllipse (circle (c, recordR));

    juce::Random rng (77);

    for (float r = labelR + 6.0f; r < recordR - 3.0f; r += 2.0f)
    {
        g.setColour (juce::Colours::white.withAlpha (0.03f + 0.045f * rng.nextFloat()));
        g.drawEllipse (circle (c, r), 0.8f);
    }

    // Track gaps: smooth land between the songs, darker than the grooves around them
    if (! fast)
    {
        for (float r : { 57.0f, 73.0f, 91.0f })
        {
            g.setColour (juce::Colours::black.withAlpha (0.22f));
            g.drawEllipse (circle (c, r), 2.5f);
            g.setColour (juce::Colours::white.withAlpha (0.06f));
            g.drawEllipse (circle (c, r + 1.5f), 0.5f);
        }
    }

    // Run-out by the label and the lead-in at the edge are smooth vinyl that catches the light
    g.setColour (juce::Colours::white.withAlpha (0.05f));
    g.drawEllipse (circle (c, labelR + 3.0f), 2.5f);
    g.setColour (juce::Colours::white.withAlpha (0.08f));
    g.drawEllipse (circle (c, recordR - 1.8f), 1.6f);

    // Two soft sheen blades, static while the grooves turn under them
    for (float blade : { -1.05f, 2.1f })
    {
        for (int i = 0; i < 4; ++i)
        {
            const float half = 0.42f - 0.09f * (float) i;
            juce::Path wedge;
            wedge.addPieSegment (circle (c, recordR), blade - half, blade + half, labelR / recordR);
            g.setColour (juce::Colours::white.withAlpha (0.045f));
            g.fillPath (wedge);
        }
    }

    g.setColour (juce::Colours::white.withAlpha (0.2f));
    g.drawEllipse (circle (c, recordR), 1.0f);

    if (disc.hole > 0.0f)
    {
        g.setColour (juce::Colour (0xff101217));
        g.fillEllipse (circle (c, disc.hole));
    }

    // Tonearm base plate (the bearing and arm are drawn live)
    auto base = circle (pivot, 24.0f);
    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.fillRoundedRectangle (base.translated (1.5f, 2.5f), 6.0f);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff343a48), base.getX(), base.getY(), juce::Colour (0xff1f2330), base.getX(), base.getBottom(), false));
    g.fillRoundedRectangle (base, 6.0f);
    g.setColour (theme::outline);
    g.drawRoundedRectangle (base, 6.0f, 1.0f);

    for (auto corner : { base.getTopLeft(), base.getTopRight(), base.getBottomLeft(), base.getBottomRight() })
        drawScrew (g, corner + juce::Point<float> (corner.x < pivot.x ? 7.0f : -7.0f, corner.y < pivot.y ? 7.0f : -7.0f), 2.6f);

    // Cue lever beside the base, and the empty arm rest below it
    {
        auto lever = juce::Rectangle<float> (12.0f, 22.0f).withCentre ({ 283.0f, 100.0f });
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRoundedRectangle (lever.translated (1.0f, 2.0f), 3.0f);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff343a48), lever.getX(), lever.getY(), juce::Colour (0xff1c2029), lever.getX(), lever.getBottom(), false));
        g.fillRoundedRectangle (lever, 3.0f);
        g.setColour (theme::outline);
        g.drawRoundedRectangle (lever, 3.0f, 1.0f);

        const juce::Point<float> from { 283.0f, 94.0f }, to { 270.0f, 87.0f };
        g.setColour (juce::Colours::black.withAlpha (0.4f));
        g.drawLine ({ from + juce::Point<float> (1.0f, 2.5f), to + juce::Point<float> (1.0f, 2.5f) }, 3.5f);
        g.setColour (juce::Colour (0xff8a909c));
        g.drawLine ({ from, to }, 3.0f);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff9aa0ad), to.x - 2.0f, to.y - 3.0f, juce::Colour (0xff3a3f4b), to.x + 2.0f, to.y + 3.0f, true));
        g.fillEllipse (circle (to, 3.6f));
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawEllipse (circle (to, 3.6f), 0.8f);
    }

    {
        const juce::Point<float> post { 282.0f, 170.0f };
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillEllipse (circle (post + juce::Point<float> (1.0f, 2.5f), 6.5f));
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff4a5060), post.x - 3.0f, post.y - 4.0f, juce::Colour (0xff1c2029), post.x + 4.0f, post.y + 5.0f, true));
        g.fillEllipse (circle (post, 6.5f));
        g.setColour (theme::outline);
        g.drawEllipse (circle (post, 6.5f), 1.0f);

        auto cradle = juce::Rectangle<float> (14.0f, 6.0f).withCentre (post + juce::Point<float> (0.0f, -1.0f));
        g.setColour (juce::Colour (0xff0d0f13));
        g.fillRoundedRectangle (cradle, 3.0f);
        g.setColour (juce::Colours::white.withAlpha (0.12f));
        g.drawHorizontalLine ((int) cradle.getY() + 1, cradle.getX() + 3.0f, cradle.getRight() - 3.0f);
    }

    // Badge and the power lamp's socket along the bottom
    g.setColour (juce::Colours::white.withAlpha (0.05f));
    g.drawHorizontalLine (h - 48, 14.0f, (float) w - 14.0f);
    g.setColour (theme::textDim);
    g.setFont (theme::font (11.5f, true).withExtraKerningFactor (0.12f));
    g.drawText ("NIGHTSHIFT", 18, h - 44, 140, 15, juce::Justification::centredLeft, true);
    g.setFont (theme::font (11.5f).withExtraKerningFactor (0.06f));
    g.setColour (theme::textDim);
    g.drawText ("DIRECT DRIVE  TURNTABLE", 18, h - 28, 220, 15, juce::Justification::centredLeft, true);

    const auto lampCentre = juce::Point<float> ((float) w - 26.0f, (float) h - 30.0f);
    g.setColour (juce::Colour (0xff07080b));
    g.fillEllipse (circle (lampCentre, 6.0f));
    g.setColour (theme::textDim);
    g.setFont (theme::font (11.5f, true).withExtraKerningFactor (0.04f));
    g.drawText ("POWER", juce::Rectangle<float> (56.0f, 15.0f).withCentre ({ lampCentre.x - 40.0f, lampCentre.y }), juce::Justification::centredRight, true);
}

void VinylEditor::renderLabel (float scale, bool fast)
{
    // Twice the physical resolution: the label is drawn rotated every frame, and the
    // supersampling keeps its small type clean through the resampling
    const auto& disc = discFor (fast);
    const float k = 2.0f * scale, half = disc.label + 2.0f;
    const int size = std::max (1, (int) std::ceil (2.0f * half * k));
    labelImage = juce::Image (juce::Image::ARGB, size, size, true);
    labelScale = scale;
    labelSpeed = (int) fast;

    juce::Graphics g (labelImage);
    g.addTransform (juce::AffineTransform::translation (half, half).scaled (k));

    const float R = disc.label;
    juce::Path face;
    face.addEllipse (circle ({}, R));

    if (disc.hole > 0.0f)
    {
        face.addEllipse (circle ({}, disc.hole));
        face.setUsingNonZeroWinding (false);
    }

    g.setGradientFill (juce::ColourGradient (theme::rose.darker (0.25f), -R * 0.4f, -R * 0.5f,
                                             theme::rose.darker (0.6f), R * 0.5f, R * 0.6f, true));
    g.fillPath (face);
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.drawEllipse (circle ({}, R - 0.5f), 1.0f);
    g.setColour (juce::Colours::white.withAlpha (0.25f));
    g.drawEllipse (circle ({}, R - 3.0f), 0.7f);

    const auto ink = juce::Colours::white;

    if (fast)
    {
        g.setColour (juce::Colours::black.withAlpha (0.4f));
        g.drawEllipse (circle ({}, disc.hole + 0.5f), 1.0f);

        g.setColour (ink.withAlpha (0.9f));
        drawArcText (g, "NIGHTSHIFT", theme::font (8.0f, true).withExtraKerningFactor (0.06f), 19.5f, false);
        g.setColour (ink.withAlpha (0.75f));
        drawArcText (g, "45 RPM", theme::monoFont (7.5f, true), 26.0f, true);
    }
    else
    {
        g.setColour (ink.withAlpha (0.9f));
        drawArcText (g, "NIGHTSHIFT", theme::font (8.5f, true).withExtraKerningFactor (0.1f), 23.5f, false);

        g.setColour (ink.withAlpha (0.72f));
        g.setFont (theme::font (7.5f).withExtraKerningFactor (0.1f));
        g.drawText ("LOFI RECORDS", juce::Rectangle<float> (64.0f, 9.0f).withCentre ({ 0.0f, 12.0f }), juce::Justification::centred, true);
        drawArcText (g, "33 1/3 RPM", theme::monoFont (7.5f, true), 29.5f, true);

        g.setColour (ink.withAlpha (0.8f));
        drawMoon (g, { -16.0f, 0.5f }, 3.6f);
        g.setFont (theme::font (8.0f, true));
        g.drawText ("A", juce::Rectangle<float> (10.0f, 10.0f).withCentre ({ 16.0f, 0.5f }), juce::Justification::centred, true);
    }
}

void VinylEditor::drawRecord (juce::Graphics& g)
{
    const bool fast = is45();
    const auto& disc = discFor (fast);
    const float warpAmt = warpParam->load() * 0.01f;
    const auto c = platterCentre;

    // Strobe dots round the platter rim, lit by the strobe lamp: they stand still at the
    // nominal speed and drift only with the speed error, like the real thing
    g.setColour (juce::Colours::white.withAlpha (0.06f + 0.24f * power));

    for (int i = 0; i < 72; ++i)
        g.fillEllipse (circle (c + polar (strobe + (float) i * kTwoPi / 72.0f, kPlatterR - 5.5f), 1.0f));

    // A speck or two of dust riding the grooves
    g.setColour (juce::Colours::white.withAlpha (0.55f));
    const float dustScale = disc.record / kLp.record;

    for (auto [r, theta, size] : { std::tuple (64.0f, 0.4f, 1.6f), std::tuple (87.0f, 2.9f, 1.3f), std::tuple (99.0f, 4.7f, 1.8f) })
        g.fillEllipse (circle (c + polar (angle + theta, r * dustScale), size * 0.5f));

    // The label turns, and an off-centre pressing makes it wander with the warp
    const auto lc = c + polar (angle, 1.8f * warpAmt);
    {
        juce::Graphics::ScopedSaveState state (g);
        const float k = 2.0f * labelScale, half = disc.label + 2.0f;
        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        g.drawImageTransformed (labelImage, juce::AffineTransform::scale (1.0f / k).translated (-half, -half).rotated (angle).translated (lc));
    }

    if (fast)
    {
        // A metal 45 adapter: three spokes from a hub, turning with the single
        for (int i = 0; i < 3; ++i)
        {
            const float a = angle + 0.4f + (float) i * kTwoPi / 3.0f;
            const auto along = polar (a, 1.0f), across = polar (a + kPi * 0.5f, 1.0f);
            juce::Path spoke;
            spoke.startNewSubPath (lc + along * 3.0f + across * 2.4f);
            spoke.lineTo (lc + along * (disc.hole + 0.3f) + across * 3.4f);
            spoke.lineTo (lc + along * (disc.hole + 0.3f) - across * 3.4f);
            spoke.lineTo (lc + along * 3.0f - across * 2.4f);
            spoke.closeSubPath();
            g.setColour (juce::Colour (0xff7f8592));
            g.fillPath (spoke);
            g.setColour (juce::Colours::black.withAlpha (0.55f));
            g.strokePath (spoke, juce::PathStrokeType (0.7f));
        }

        g.setGradientFill (juce::ColourGradient (juce::Colour (0xffc4c8d0), lc.x - 3.0f, lc.y - 4.0f, juce::Colour (0xff4a4f5c), lc.x + 4.0f, lc.y + 5.0f, true));
        g.fillEllipse (circle (lc, 6.5f));
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawEllipse (circle (lc, 6.5f), 0.8f);
        g.setColour (juce::Colours::white.withAlpha (0.3f));
        g.drawEllipse (circle (lc, 4.5f), 0.6f);
    }

    // Spindle, fixed at the true centre
    const float spindleR = fast ? 2.6f : 4.5f;
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xffb8bcc6), c.x - 3.0f, c.y - 3.0f, juce::Colour (0xff4a4f5c), c.x + 3.0f, c.y + 3.0f, true));
    g.fillEllipse (circle (c, spindleR));
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.drawEllipse (circle (c, spindleR), 0.8f);
}

juce::Point<float> VinylEditor::stylusAt (float armAngle) const
{
    return armPivot + polar (armAngle, kArmTube) + polar (armAngle + kHeadOffset, kHeadLength);
}

float VinylEditor::armAngleFor (float radius) const
{
    // The arm has one length, so a groove radius fixes its angle: bisect for it, between
    // pointing at the spindle (stylus near the centre) and swung a radian outward
    const auto toCentre = platterCentre - armPivot;
    float inner = std::atan2 (toCentre.y, toCentre.x), outer = inner - 1.0f;

    for (int i = 0; i < 24; ++i)
    {
        const float mid = 0.5f * (inner + outer);
        (stylusAt (mid).getDistanceFrom (platterCentre) > radius ? outer : inner) = mid;
    }

    return 0.5f * (inner + outer);
}

void VinylEditor::drawTonearm (juce::Graphics& g)
{
    // A warped, eccentric record pushes the stylus in and out once per revolution, in
    // time with the pitch bend you hear (the platter phase comes from the audio thread)
    const float warpAmt = warpParam->load() * 0.01f;
    const float sway = 0.035f * warpAmt * spin * std::sin (kTwoPi * vinyl.getPlatterPhase());
    const float armAngle = armAngleFor (grooveRadius) + sway;
    const float headAngle = armAngle + kHeadOffset;
    const auto pivot = armPivot;
    const auto neck = pivot + polar (armAngle, kArmTube);
    const auto tip = neck + polar (headAngle, kHeadLength);

    // Shadow on the record
    g.setColour (juce::Colours::black.withAlpha (0.4f));
    g.drawLine ({ pivot + juce::Point<float> (3.0f, 5.0f), neck + juce::Point<float> (3.0f, 5.0f) }, 5.0f);
    g.setColour (juce::Colours::black.withAlpha (0.25f));
    g.fillEllipse (juce::Rectangle<float> (16.0f, 8.0f).withCentre (tip + juce::Point<float> (2.0f, 4.0f)));

    // Counterweight behind the pivot, and the tube
    {
        juce::Graphics::ScopedSaveState state (g);
        g.addTransform (juce::AffineTransform::rotation (armAngle, pivot.x, pivot.y));
        auto weight = juce::Rectangle<float> (pivot.x - 34.0f, pivot.y - 7.5f, 20.0f, 15.0f);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff7d8494), weight.getX(), weight.getY(), juce::Colour (0xff2b2f3a), weight.getX(), weight.getBottom(), false));
        g.fillRoundedRectangle (weight, 3.0f);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawRoundedRectangle (weight, 3.0f, 0.8f);
        g.setColour (juce::Colours::white.withAlpha (0.12f));

        for (float x = weight.getX() + 4.0f; x < weight.getRight() - 2.0f; x += 4.0f)
            g.drawVerticalLine ((int) x, weight.getY() + 2.0f, weight.getBottom() - 2.0f);

        g.setColour (juce::Colour (0xff8a909c));
        g.drawLine (pivot.x - 14.0f, pivot.y, pivot.x + kArmTube + 2.0f, pivot.y, 5.0f);
        g.setColour (juce::Colours::white.withAlpha (0.45f));
        g.drawLine (pivot.x - 12.0f, pivot.y - 1.3f, pivot.x + kArmTube, pivot.y - 1.3f, 1.2f);
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.drawLine (pivot.x - 12.0f, pivot.y + 1.8f, pivot.x + kArmTube, pivot.y + 1.8f, 0.8f);
    }

    // Headshell, angled toward the groove's tangent, with the cartridge and a finger lift
    {
        juce::Graphics::ScopedSaveState state (g);
        g.addTransform (juce::AffineTransform::rotation (headAngle, neck.x, neck.y));
        auto shell = juce::Rectangle<float> (neck.x - 3.0f, neck.y - 5.5f, kHeadLength + 7.0f, 11.0f);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3b4150), shell.getX(), shell.getY(), juce::Colour (0xff1c1f28), shell.getX(), shell.getBottom(), false));
        g.fillRoundedRectangle (shell, 2.5f);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawRoundedRectangle (shell, 2.5f, 0.8f);

        auto lift = juce::Rectangle<float> (neck.x + kHeadLength - 1.0f, neck.y - 11.0f, 3.0f, 6.0f);
        g.setColour (juce::Colour (0xff6f7686));
        g.fillRoundedRectangle (lift, 1.2f);

        g.setColour (juce::Colour (0xff2a2e38));
        g.fillRect (juce::Rectangle<float> (neck.x + kHeadLength - 7.0f, neck.y - 3.5f, 9.0f, 7.0f));
        g.setColour (theme::rose.withAlpha (0.85f));
        g.fillEllipse (circle ({ neck.x + kHeadLength - 3.0f, neck.y }, 1.25f));
        g.setColour (juce::Colours::white.withAlpha (0.2f));
        g.drawHorizontalLine ((int) shell.getY() + 1, shell.getX() + 3.0f, shell.getRight() - 3.0f);
    }

    g.setColour (juce::Colours::white.withAlpha (0.8f));
    g.fillEllipse (circle (tip, 1.2f));

    // Bearing on top of the base
    auto bearing = circle (pivot, 13.0f);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff6f7686), pivot.x - 8.0f, pivot.y - 8.0f, juce::Colour (0xff242831), pivot.x + 9.0f, pivot.y + 9.0f, true));
    g.fillEllipse (bearing);
    g.setColour (juce::Colours::black.withAlpha (0.65f));
    g.drawEllipse (bearing, 1.0f);
    g.setColour (juce::Colour (0xff15181f));
    g.fillEllipse (bearing.reduced (8.0f));
    g.setColour (juce::Colours::white.withAlpha (0.18f));
    g.drawEllipse (bearing.reduced (8.0f), 0.8f);
}

juce::Rectangle<int> VinylEditor::lampArea() const
{
    const auto centre = graphic.getBottomRight().toFloat() - juce::Point<float> (26.0f, 30.0f);
    return circle (centre, 9.0f).getSmallestIntegerContainer();
}

void VinylEditor::drawLamp (juce::Graphics& g)
{
    // Lit while the deck is running; bypass switches it off
    const auto centre = lampArea().toFloat().getCentre();
    g.setColour (theme::rose.withAlpha (0.3f * power));
    g.fillEllipse (circle (centre, 7.0f));
    g.setColour (theme::rose.withAlpha (0.15f + 0.85f * power));
    g.fillEllipse (circle (centre, 3.0f));
}

juce::Rectangle<int> VinylEditor::movingArea() const
{
    // The platter well, plus the arm's bearing and counterweight in the corner above it
    const auto well = circle (platterCentre + juce::Point<float> (0.0f, 3.0f), kPlatterR + 9.0f);
    const auto arm = circle (armPivot, 40.0f);
    return well.getUnion (arm).getSmallestIntegerContainer().getIntersection (graphic);
}

void VinylEditor::animate (float seconds)
{
    const bool fast = is45();
    const bool bypassed = bypassParam->load() >= 0.5f;
    const float rpm = fast ? 45.0f : 100.0f / 3.0f;

    wobbleSmoothed += (vinyl.getPlatterWobble() - wobbleSmoothed) * 0.5f;
    spin += ((bypassed ? 0.0f : 1.0f) - spin) * (bypassed ? 0.05f : 0.15f);   // coasts down in about a second
    power += ((bypassed ? 0.0f : 1.0f) - power) * 0.35f;
    grooveRadius += (discFor (fast).groove - grooveRadius) * 0.2f;            // the arm glides to the new record

    // The real speed error is a fraction of a percent: magnify it for the eye, then
    // soft-limit it so warp 100 reads as a wobble (+-25 %), not a failing motor
    const float error = 40.0f * (wobbleSmoothed - 1.0f);
    const float speedFactor = 1.0f + 0.25f * std::tanh (error * 4.0f);
    const float nominal = seconds * kTwoPi * rpm / 60.0f;

    angle = std::fmod (angle + nominal * speedFactor * spin, kTwoPi);
    strobe = std::fmod (strobe + nominal * (speedFactor - 1.0f) * spin + kTwoPi, kTwoPi);

    repaint (movingArea());

    if (std::abs (power - shownPower) > 0.002f)
    {
        shownPower = power;
        repaint (lampArea());
    }
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new VinylProcessor();
}
