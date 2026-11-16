#include "Haze.h"
#include "common/ScaledEditor.h"

using namespace lofi;
using namespace lofi::params;

//==============================================================================
HazeProcessor::HazeProcessor()
    : LofiProcessor ({ "Nightshift Haze", "chorus + spring", theme::teal }, createLayout())
{
    auto raw = [this] (const char* id)
    {
        auto* value = apvts.getRawParameterValue (id);
        jassert (value != nullptr);   // an ID out of step with createLayout()
        return value;
    };

    rateParam      = raw ("rate");
    depthParam     = raw ("depth");
    voicesParam    = raw ("voices");
    chorusMixParam = raw ("chorusMix");
    sizeParam      = raw ("size");
    toneParam      = raw ("tone");
    dripParam      = raw ("drip");
    springMixParam = raw ("springMix");
    widthParam     = raw ("width");
}

LofiProcessor::Layout HazeProcessor::createLayout()
{
    // Names are what `lofitool --set` matches, so the two mixes carry their IDs as names
    Layout layout;
    layout.add (hertz   ("rate",       "Rate",      0.1f, 5.0f, 0.6f, 1.0f));
    layout.add (percent ("depth",      "Depth",     40.0f));
    layout.add (integer ("voices",     "Voices",    1, 3, 2));
    layout.add (percent ("chorusMix",  "ChorusMix", 50.0f));
    layout.add (percent ("size",       "Size",      50.0f));
    layout.add (percent ("tone",       "Tone",      50.0f));
    layout.add (percent ("drip",       "Drip",      40.0f));
    layout.add (percent ("springMix",  "SpringMix", 35.0f));
    layout.add (percent ("width",      "Width",     100.0f));
    addCommonParameters (layout);
    return layout;
}

juce::AudioProcessorEditor* HazeProcessor::createEditor()
{
    return new lofi::ScaledEditor (*this, new HazeEditor (*this));
}

//==============================================================================
void HazeProcessor::prepareDsp (const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate;
    numChannels = juce::jlimit (1, 2, (int) spec.numChannels);

    lfo.prepare (sampleRate, 0x48415a45u);

    // Four springs: two per channel, each a different length and dispersion band, so
    // the tank is dense and the sides decorrelate without any stereo trickery
    static constexpr haze::Spring::Shape shapes[2][2]
    {
        { { 1.00f, 700.0f, 2000.0f, 4.1f }, { 1.23f, 820.0f, 2300.0f, 5.3f } },
        { { 1.07f, 760.0f, 2150.0f, 4.6f }, { 1.31f, 880.0f, 2450.0f, 5.9f } },
    };

    for (int ch = 0; ch < 2; ++ch)
    {
        chorusDelay[ch].prepare (sampleRate, 32.0f);
        chorusSplit[ch].prepare (sampleRate);
        chorusSplit[ch].setCutoff (120.0f);
        voiceFeed[ch].prepare (sampleRate, 120.0f);
        voiceTone[ch].prepare (sampleRate);
        voiceTone[ch].setCutoff (10000.0f);

        for (int s = 0; s < 2; ++s)
            springs[ch][s].prepare (sampleRate, 0x5350524eu + (juce::uint32) (ch * 2 + s) * 0x9e3779b9u, shapes[ch][s]);

        springInLow[ch].prepare (sampleRate);
        springInLow[ch].setCutoff (8000.0f);
        springInHigh[ch].prepare (sampleRate);
        springInHigh[ch].setCutoff (100.0f);
    }

    for (int v = 0; v < maxVoices; ++v)
    {
        lofi::dsp::prepareSmoothed (voiceGain[v], sampleRate, 0.0f, 0.03);
        lofi::dsp::prepareSmoothed (voicePan[v], sampleRate, 0.0f, 0.03);
    }

    lofi::dsp::prepareSmoothed (depthMs, sampleRate, 0.0f, 0.03);
    lofi::dsp::prepareSmoothed (chorusMix, sampleRate, 0.0f, 0.03);
    lofi::dsp::prepareSmoothed (springMix, sampleRate, 0.0f, 0.03);
    lofi::dsp::prepareSmoothed (springTrim, sampleRate, 1.0f, 0.05);
    lofi::dsp::prepareSmoothed (feedIn, sampleRate, 1.0f, 0.02);

    updateTargets();
    snapSmoothers();

    // A host may restore the state after prepareToPlay; the first block lands on whatever
    // the parameters say then, instead of gliding there from the defaults
    snapPending = true;
    setTailSeconds (6.0);
}

void HazeProcessor::updateTargets()
{
    // Audio thread: cached atomics only, no lookups by name (see the members)
    const int voices = getVoiceCount();
    const float width = numChannels > 1 ? getWidth() : 0.0f;

    lfo.setRate (rateParam->load());
    lfo.setVoiceCount (voices);
    depthMs.setTargetValue (depthParam->load() * 0.05f);       // 100 % = +-5 ms
    chorusMix.setTargetValue (chorusMixParam->load() * 0.01f);
    springMix.setTargetValue (springMixParam->load() * 0.01f);

    for (int v = 0; v < maxVoices; ++v)
    {
        voiceGain[v].setTargetValue (v < voices ? 1.0f : 0.0f);
        voicePan[v].setTargetValue (voicePanFor (v, voices, width));
    }

    const float size = getSize(), tone = toneParam->load() * 0.01f, drip = dripParam->load() * 0.01f;

    for (auto& channel : springs)
        for (auto& spring : channel)
            spring.setTargets (size, tone, drip);

    // Longer tails pile up more energy in the loop; trim so the wet level only grows a little
    const float g = springs[0][0].getFeedback();
    springTrim.setTargetValue (1.25f * std::sqrt (1.0f - 0.4f * g * g));
}

void HazeProcessor::snapSmoothers()
{
    for (auto* s : { &depthMs, &chorusMix, &springMix, &springTrim, &feedIn })
        s->setCurrentAndTargetValue (s->getTargetValue());

    for (int v = 0; v < maxVoices; ++v)
    {
        voiceGain[v].setCurrentAndTargetValue (voiceGain[v].getTargetValue());
        voicePan[v].setCurrentAndTargetValue (voicePan[v].getTargetValue());
    }

    lfo.snap();

    for (auto& channel : springs)
        for (auto& spring : channel)
            spring.snap();
}

void HazeProcessor::resetDsp()
{
    // Audio thread, on the way out of bypass: silence everything that remembers the past,
    // land the ramps on the current settings, then fade the input back into the empty lines
    for (int ch = 0; ch < 2; ++ch)
    {
        chorusDelay[ch].reset();
        chorusSplit[ch].reset();
        voiceFeed[ch].reset();
        voiceTone[ch].reset();
        springInLow[ch].reset();
        springInHigh[ch].reset();

        for (auto& spring : springs[ch])
            spring.reset();
    }

    updateTargets();
    snapSmoothers();
    feedIn.setCurrentAndTargetValue (0.0f);
    feedIn.setTargetValue (1.0f);
    snapPending = false;
}

void HazeProcessor::processDsp (juce::AudioBuffer<float>& buffer)
{
    const int n = buffer.getNumSamples();
    const int nch = std::min (numChannels, buffer.getNumChannels());

    // Targets, once per block; everything audible ramps per sample below
    updateTargets();

    if (std::exchange (snapPending, false))
        snapSmoothers();

    constexpr float quarterPi = juce::MathConstants<float>::pi * 0.25f;
    constexpr float halfPi = juce::MathConstants<float>::pi * 0.5f;
    float springPeak = 0.0f;
    float mod[maxVoices] {};

    for (int i = 0; i < n; ++i)
    {
        lfo.next (mod);
        const float depth = depthMs.getNextValue();

        // Voice gains per output channel: equal-power pans spread by width, and a
        // normalisation so adding voices or widening never changes the level
        float gain[2][maxVoices], norm[2] { 0.0f, 0.0f };

        for (int v = 0; v < maxVoices; ++v)
        {
            const float a = voiceGain[v].getNextValue();
            const float angle = (voicePan[v].getNextValue() + 1.0f) * quarterPi;
            gain[0][v] = a * std::cos (angle);
            gain[1][v] = a * std::sin (angle);
            norm[0] += gain[0][v] * gain[0][v];
            norm[1] += gain[1][v] * gain[1][v];
        }

        for (auto& nrm : norm)
            nrm = 1.0f / std::sqrt (std::max (1.0e-4f, nrm));

        const float cm = chorusMix.getNextValue() * halfPi;
        const float dryC = std::cos (cm), wetC = std::sin (cm);
        const float sm = springMix.getNextValue() * halfPi;
        const float dryS = std::cos (sm), wetS = std::sin (sm);
        const float trim = springTrim.getNextValue();
        const float in = feedIn.getNextValue();

        for (int ch = 0; ch < nch; ++ch)
        {
            float* data = buffer.getWritePointer (ch);
            const float x = data[i];

            // Below 120 Hz the voices would only comb-filter the kick, so that part stays
            // dry; the split sums back exactly, so at mix 0 the signal is untouched
            const float low = chorusSplit[ch].lowpass (x);
            const float high = x - low;
            auto& delay = chorusDelay[ch];
            delay.push (voiceFeed[ch].process (high) * in);

            float wet = 0.0f;

            for (int v = 0; v < maxVoices; ++v)
                if (gain[ch][v] > 0.0f)
                    wet += gain[ch][v] * delay.read (delay.msToSamples (voiceBaseMs[v] + depth * mod[v]));

            const float chorused = low + high * dryC + voiceTone[ch].lowpass (wet * norm[ch]) * wetC;

            // The tank: no sub, nothing much above the springs' transition band
            const float fed = springInHigh[ch].highpass (springInLow[ch].lowpass (chorused * in));
            const float spring = 0.707f * (springs[ch][0].process (fed) + springs[ch][1].process (fed)) * trim;
            const float wetSpring = spring * wetS;

            data[i] = chorused * dryS + wetSpring;
            springPeak = std::max (springPeak, std::abs (wetSpring));
        }
    }

    lfoPosition.store (lfo.getPosition());
    springLevel.store (std::max (springLevel.load(), springPeak));

    for (int v = 0; v < maxVoices; ++v)
    {
        voicePosition[v].store (mod[v]);
        voiceLevel[v].store (voiceGain[v].getCurrentValue());
    }
}

//==============================================================================
VoiceSwitch::VoiceSwitch (juce::AudioProcessorValueTreeState& state, const juce::String& paramId)
    : parameter (*state.getParameter (paramId)),
      attachment (parameter, [this] (float v) { current = juce::jlimit (1, 3, juce::roundToInt (v)); repaint(); }, state.undoManager)
{
    attachment.sendInitialUpdate();
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setTooltip ("Chorus voices: 1 sweeps alone, 2 move in opposite phase, 3 sit 120 degrees apart");
}

juce::Rectangle<float> VoiceSwitch::keyBounds (int index) const
{
    // Between the caption and value rows, like a knob's dial: an LED over each key
    constexpr float keyW = 21.0f, keyH = 25.0f, gap = 5.0f;
    const auto middle = getLocalBounds().toFloat().withTrimmedTop (16.0f).withTrimmedBottom (16.0f);
    const float x0 = middle.getCentreX() - (3.0f * keyW + 2.0f * gap) * 0.5f;
    const float y0 = middle.getCentreY() - keyH * 0.5f + 5.0f;
    return { x0 + (float) index * (keyW + gap), y0, keyW, keyH };
}

int VoiceSwitch::keyAt (juce::Point<float> p) const
{
    int best = 0;

    for (int i = 1; i < 3; ++i)
        if (std::abs (p.x - keyBounds (i).getCentreX()) < std::abs (p.x - keyBounds (best).getCentreX()))
            best = i;

    return best + 1;
}

void VoiceSwitch::mouseDown (const juce::MouseEvent& e)
{
    const int voices = keyAt (e.position);

    if (voices != current)
        attachment.setValueAsCompleteGesture ((float) voices);
}

void VoiceSwitch::mouseDoubleClick (const juce::MouseEvent&)
{
    attachment.setValueAsCompleteGesture (parameter.convertFrom0to1 (parameter.getDefaultValue()));
}

void VoiceSwitch::paint (juce::Graphics& g)
{
    using namespace lofi::theme;

    // Caption and value rows exactly as a Knob draws them, so the baselines line up
    g.setColour (textDim.brighter (0.15f));
    g.setFont (font (11.5f, true).withExtraKerningFactor (0.06f));
    g.drawText ("VOICES", getLocalBounds().removeFromTop (16), juce::Justification::centred, true);
    g.setColour (text);
    g.setFont (monoFont (12.0f));
    g.drawText (juce::String (current) + (current == 1 ? " voice" : " voices"), getLocalBounds().removeFromBottom (16),
                juce::Justification::centred, true);

    // A recessed plate holding the three keys and their LEDs
    auto plate = keyBounds (0).getUnion (keyBounds (2)).withTop (keyBounds (0).getY() - 12.0f).expanded (4.0f, 4.0f);
    g.setColour (juce::Colour (0xff0b0d11));
    g.fillRoundedRectangle (plate, 5.0f);
    g.setColour (juce::Colours::white.withAlpha (0.05f));
    g.drawRoundedRectangle (plate.translated (0.0f, 1.0f), 5.0f, 1.0f);
    g.setColour (outline);
    g.drawRoundedRectangle (plate, 5.0f, 1.0f);

    for (int i = 0; i < 3; ++i)
    {
        const bool on = i + 1 == current;
        auto key = keyBounds (i);

        // LED over the key
        auto led = juce::Rectangle<float> (5.0f, 5.0f).withCentre ({ key.getCentreX(), key.getY() - 6.0f });

        if (on)
        {
            g.setColour (teal.withAlpha (0.3f));
            g.fillEllipse (led.expanded (2.5f));
        }

        g.setColour (on ? teal : teal.withAlpha (0.14f));
        g.fillEllipse (led);

        // The key: raised when off, pressed in (darker, one pixel down) when latched
        if (on)
            key.translate (0.0f, 1.0f);

        g.setGradientFill (juce::ColourGradient (on ? juce::Colour (0xff2c323e) : juce::Colour (0xff434b5b), key.getCentreX(), key.getY(),
                                                 on ? juce::Colour (0xff1a1e26) : juce::Colour (0xff252a34), key.getCentreX(), key.getBottom(), false));
        g.fillRoundedRectangle (key, 3.0f);
        g.setColour (juce::Colours::black.withAlpha (0.75f));
        g.drawRoundedRectangle (key, 3.0f, 1.0f);

        if (! on)
        {
            g.setColour (juce::Colours::white.withAlpha (0.12f));
            g.drawHorizontalLine ((int) key.getY() + 1, key.getX() + 3.0f, key.getRight() - 3.0f);
        }

        g.setColour (on ? teal.brighter (0.25f) : textDim.brighter (0.35f));
        g.setFont (font (12.5f, true));
        g.drawText (juce::String (i + 1), key, juce::Justification::centred, true);
    }
}

//==============================================================================
HazeEditor::HazeEditor (HazeProcessor& p)
    : LofiEditor (p), haze (p), voiceSwitch (p.getState(), "voices")
{
    addAndMakeVisible (voiceSwitch);

    // The two section blends are labelled for what they blend; the section title says which
    auto& rate = addKnob ("rate", "Rate", theme::teal);
    auto& chorusBlend = addKnob ("chorusMix", "Blend", theme::teal);
    auto& springBlend = addKnob ("springMix", "Blend");
    spreadKnob = &addKnob ("width", "Spread", theme::violet);
    chorusBlend.getSlider().setTooltip ("Chorus blend: dry to chorus voices");
    springBlend.getSlider().setTooltip ("Spring blend: chorused signal to spring tank");
    spreadKnob->getSlider().setTooltip ("How far the chorus voices spread across left and right (needs 2 or 3 voices)");
    rateKnob = &rate;

    chorusCells = { &rate, &addKnob ("depth", "Depth", theme::teal), &voiceSwitch, &chorusBlend };
    springCells = { &addKnob ("size", "Size"), &addKnob ("tone", "Tone"), &addKnob ("drip", "Drip"), &springBlend };

    outCells = { nullptr, spreadKnob, &addKnob ("mix", "Mix", theme::violet), &addKnob ("output", "Output", theme::violet, true) };
    setSize (820, 460);
}

void HazeEditor::layoutBody (juce::Rectangle<int> body)
{
    graphic = body.removeFromLeft (236);
    body.removeFromLeft (16);

    const int rowH = body.getHeight() / 3;
    chorusRow = body.removeFromTop (rowH);
    springRow = body.removeFromTop (rowH);
    outRow = body;

    // One four-column grid for every row, so the controls line up down the panel and the
    // right-hand column is each row's level: chorus blend, spring blend, output
    const int x0 = chorusRow.getX(), colW = chorusRow.getWidth() / 4;

    auto cell = [&] (juce::Rectangle<int> row, int col)
    {
        // Clear of the section title on top, and of the panel's border below the values
        const auto a = row.withTrimmedTop (18).withTrimmedBottom (9);
        return juce::Rectangle<int> (x0 + col * colW, a.getY(), colW, a.getHeight())
                   .withSizeKeepingCentre (Knob::defaultSize, std::min (a.getHeight(), Knob::defaultSize + 32));
    };

    auto place = [&] (const std::vector<juce::Component*>& cells, juce::Rectangle<int> row)
    {
        for (size_t col = 0; col < cells.size(); ++col)
            if (cells[col] != nullptr)
                cells[col]->setBounds (cell (row, (int) col));
    };

    place (chorusCells, chorusRow);
    place (springCells, springRow);
    place (outCells, outRow);

    // The Out row's first cell shows where the chorus voices sit between L and R
    field = cell (outRow, 0).withSizeKeepingCentre (112, cell (outRow, 0).getHeight());

    // The rate LED sits off the Rate dial's upper right, as on a hardware chorus
    const auto dial = rateKnob->getSlider().getBounds().toFloat() + rateKnob->getPosition().toFloat();
    const float reach = (dial.getWidth() * 0.5f + 8.0f) * 0.7071f;
    lampCentre = dial.getCentre() + juce::Point<float> (reach, -reach);
    lfoLamp = juce::Rectangle<float> (20.0f, 20.0f).withCentre (lampCentre).getSmallestIntegerContainer();
}

void HazeEditor::paintBody (juce::Graphics& g, juce::Rectangle<int>)
{
    paintTank (g, graphic);

    auto section = [&] (juce::Rectangle<int> r, const juce::String& title, juce::Colour accent)
    {
        auto panel = r.reduced (0, 3).toFloat();
        g.setColour (theme::panel.withAlpha (0.55f));
        g.fillRoundedRectangle (panel, 8.0f);
        g.setColour (juce::Colours::white.withAlpha (0.04f));
        g.drawRoundedRectangle (panel.reduced (0.5f), 8.0f, 1.0f);
        drawSectionTitle (g, r.withHeight (18).translated (0, 5).reduced (14, 0), title, accent);
    };

    section (chorusRow, "Chorus", theme::teal);
    section (springRow, "Spring", theme::amber);
    section (outRow, "Out", theme::violet);

    paintField (g);
    paintLfoLamp (g);
}

void HazeEditor::paintLfoLamp (juce::Graphics& g)
{
    // Breathes with voice one; everything drawn here stays inside lfoLamp
    using namespace theme;
    const float lit = 0.5f + 0.5f * lfoSmooth;
    auto lamp = juce::Rectangle<float> (8.0f, 8.0f).withCentre (lampCentre);

    g.setColour (teal.withAlpha (0.08f + 0.3f * lit));
    g.fillEllipse (lamp.expanded (3.5f));
    g.setColour (juce::Colours::black.withAlpha (0.7f));
    g.fillEllipse (lamp.expanded (1.0f));
    g.setColour (teal.withAlpha (0.18f + 0.82f * lit));
    g.fillEllipse (lamp);
    g.setColour (juce::Colours::white.withAlpha (0.08f + 0.3f * lit));
    g.fillEllipse (juce::Rectangle<float> (3.0f, 2.0f).withCentre (lamp.getCentre().translated (-1.0f, -1.5f)));
}

void HazeEditor::paintField (juce::Graphics& g)
{
    using namespace theme;
    auto r = field.toFloat();

    g.setColour (textDim.brighter (0.15f));
    g.setFont (font (11.5f, true).withExtraKerningFactor (0.06f));
    g.drawText ("FIELD", r.removeFromTop (16.0f), juce::Justification::centred, true);

    const int voices = haze.getVoiceCount();
    const bool mono = voices == 1 || haze.getWidth() < 0.005f;
    g.setColour (text);
    g.setFont (monoFont (12.0f));
    g.drawText (mono ? "centre" : "stereo", r.removeFromBottom (16.0f), juce::Justification::centred, true);

    // A small dark screen: the L-R scale, and one dot per voice at its pan, bobbing with
    // that voice's delay sweep
    auto screen = juce::Rectangle<float> (r.getWidth(), 42.0f).withCentre (r.getCentre());
    g.setColour (juce::Colour (0xff07090c));
    g.fillRoundedRectangle (screen, 4.0f);
    g.setColour (juce::Colours::white.withAlpha (0.06f));
    g.drawRoundedRectangle (screen.translated (0.0f, 1.0f), 4.0f, 1.0f);
    g.setColour (outline);
    g.drawRoundedRectangle (screen, 4.0f, 1.0f);

    const float cy = screen.getCentreY(), left = screen.getX() + 18.0f, right = screen.getRight() - 18.0f;
    g.setColour (juce::Colours::white.withAlpha (0.09f));
    g.fillRect (left, cy - 0.5f, right - left, 1.0f);

    for (float x : { left, (left + right) * 0.5f, right })
        g.fillRect (x - 0.5f, cy - 3.5f, 1.0f, 7.0f);

    g.setColour (textDim);
    g.setFont (font (12.0f, true));
    g.drawText ("L", juce::Rectangle<float> (12.0f, 12.0f).withCentre ({ screen.getX() + 8.0f, cy }), juce::Justification::centred, true);
    g.drawText ("R", juce::Rectangle<float> (12.0f, 12.0f).withCentre ({ screen.getRight() - 8.0f, cy }), juce::Justification::centred, true);

    juce::Graphics::ScopedSaveState state (g);
    g.reduceClipRegion (screen.reduced (1.0f).toNearestInt());

    for (int v = 0; v < 3; ++v)
    {
        const float level = voiceLit[v];

        if (level < 0.02f)
            continue;

        const auto at = juce::Point<float> (juce::jmap (voicePanSmooth[v], -1.0f, 1.0f, left, right), cy - voiceSmooth[v] * 9.0f);
        g.setGradientFill (juce::ColourGradient (teal.withAlpha (0.35f * level), at, teal.withAlpha (0.0f), at.translated (9.0f, 0.0f), true));
        g.fillEllipse (juce::Rectangle<float> (18.0f, 18.0f).withCentre (at));
        g.setColour (teal.withAlpha (level));
        g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre (at));
        g.setColour (juce::Colour (0xffe8fffb).withAlpha (0.8f * level));
        g.fillEllipse (juce::Rectangle<float> (2.0f, 2.0f).withCentre (at.translated (-0.8f, -0.8f)));
    }
}

void HazeEditor::paintTank (juce::Graphics& g, juce::Rectangle<int> area)
{
    using namespace theme;
    auto tank = area.reduced (4, 3).toFloat();   // same top and bottom edges as the panels

    // Chassis: folded, painted steel with a light from above and screws in the corners
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3d4452), tank.getX(), tank.getY(),
                                             juce::Colour (0xff1f232b), tank.getX(), tank.getBottom(), false));
    g.fillRoundedRectangle (tank, 10.0f);
    g.setColour (juce::Colours::white.withAlpha (0.08f));
    g.drawRoundedRectangle (tank.reduced (1.5f), 9.0f, 1.0f);
    g.setColour (outline);
    g.drawRoundedRectangle (tank.reduced (0.5f), 10.0f, 1.2f);

    for (auto corner : { tank.getTopLeft(), tank.getTopRight(), tank.getBottomLeft(), tank.getBottomRight() })
        drawScrew (g, corner + juce::Point<float> (corner.x == tank.getX() ? 10.0f : -10.0f, corner.y == tank.getY() ? 10.0f : -10.0f), 3.2f);

    // Label strip, inset clear of the screws like the footer below
    auto label = tank.removeFromTop (34.0f).reduced (28.0f, 0.0f).withTrimmedTop (9.0f);
    g.setColour (text);
    g.setFont (font (11.5f, true).withExtraKerningFactor (0.12f));
    g.drawText ("SPRING TANK", label, juce::Justification::centredLeft, true);
    g.setColour (textDim.brighter (0.4f));
    g.setFont (monoFont (12.0f));
    g.drawText ("HZ-3 3SPR", label, juce::Justification::centredRight, true);

    // The open pan the springs hang in: a recessed, brushed steel well
    auto window = tank.reduced (16.0f, 2.0f).withTrimmedBottom (36.0f);
    g.setColour (juce::Colour (0xff040508));
    g.fillRoundedRectangle (window, 6.0f);
    auto well = window.reduced (1.5f);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff181e27), well.getCentreX(), well.getCentreY(),
                                             juce::Colour (0xff080a0e), well.getX(), well.getY(), true));
    g.fillRoundedRectangle (well, 5.0f);

    {
        juce::Random rng (77);   // deterministic grain so repaints do not shimmer
        g.setColour (juce::Colours::white.withAlpha (0.016f));

        for (float x = well.getX() + 3.0f; x < well.getRight() - 3.0f; x += 2.0f)
            g.drawVerticalLine ((int) x, well.getY() + 4.0f + rng.nextFloat() * 10.0f, well.getBottom() - 4.0f - rng.nextFloat() * 10.0f);
    }

    // Inner shadow under the chassis lip
    g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.55f), well.getX(), well.getY(),
                                             juce::Colours::transparentBlack, well.getX(), well.getY() + 16.0f, false));
    g.fillRoundedRectangle (well.withHeight (16.0f), 5.0f);

    // Transducer rails at both ends, springs hooked between them
    auto inner = well.reduced (8.0f, 6.0f);
    auto topRail = inner.removeFromTop (22.0f), bottomRail = inner.removeFromBottom (22.0f);
    const float laneW = inner.getWidth() / 3.0f;
    const float canX[3] { inner.getX() + laneW * 0.5f, inner.getX() + laneW * 1.5f, inner.getX() + laneW * 2.5f };

    for (int i = 0; i < 3; ++i)
        paintSpring (g, canX[i], topRail.getBottom() - 3.0f, bottomRail.getY() + 3.0f, i);

    paintRail (g, topRail, canX);
    paintRail (g, bottomRail, canX);

    // Glass: one soft diagonal glare, no hard edges
    g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.075f), well.getX(), well.getY(),
                                             juce::Colours::white.withAlpha (0.0f), well.getX() + well.getWidth() * 0.7f, well.getY() + well.getHeight() * 0.32f, false));
    g.fillRoundedRectangle (well, 5.0f);
    g.setColour (juce::Colours::white.withAlpha (0.06f));
    g.drawRoundedRectangle (window.reduced (0.5f).translated (0.0f, 1.0f), 6.0f, 1.0f);
    g.setColour (outline);
    g.drawRoundedRectangle (window.reduced (0.5f), 6.0f, 1.0f);

    // Footer: a lamp that flashes with the wet spring level, and the decay on a small LCD,
    // inset and lifted clear of the corner screws
    auto footer = tank.removeFromBottom (34.0f).reduced (28.0f, 7.0f).translated (0.0f, -2.0f);
    const float lit = juce::jlimit (0.0f, 1.0f, tankLevel * 2.5f);
    auto led = juce::Rectangle<float> (8.0f, 8.0f).withCentre ({ footer.getX() + 4.0f, footer.getCentreY() });
    g.setColour (amber.withAlpha (0.08f + 0.35f * lit));
    g.fillEllipse (led.expanded (3.5f));
    g.setColour (amber.withAlpha (0.2f + 0.8f * lit));
    g.fillEllipse (led);
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.drawEllipse (led, 0.8f);

    const auto caption = textDim.brighter (0.15f);
    const auto captionFont = font (11.5f, true).withExtraKerningFactor (0.08f);
    g.setColour (caption);
    g.setFont (captionFont);
    g.drawText ("WET", footer.withTrimmedLeft (15.0f), juce::Justification::centredLeft, true);

    auto lcd = footer.removeFromRight (56.0f);
    g.setColour (juce::Colour (0xff07090c));
    g.fillRoundedRectangle (lcd, 3.0f);
    g.setColour (juce::Colours::white.withAlpha (0.07f));
    g.drawRoundedRectangle (lcd.translated (0.0f, 1.0f), 3.0f, 1.0f);
    g.setColour (outline);
    g.drawRoundedRectangle (lcd, 3.0f, 1.0f);

    g.setColour (amber.withAlpha (0.92f));
    g.setFont (monoFont (12.0f, true));
    g.drawText (juce::String (haze::Spring::nominalT60 (haze.getSize()), 1) + " s", lcd, juce::Justification::centred, true);

    g.setColour (caption);
    g.setFont (captionFont);
    g.drawText ("DECAY", footer.withTrimmedRight (7.0f), juce::Justification::centredRight, true);
}

void HazeEditor::paintRail (juce::Graphics& g, juce::Rectangle<float> rail, const float* canX)
{
    // The bar, then one transducer can per spring: a steel housing showing its copper coil
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff666f80), rail.getX(), rail.getY(),
                                             juce::Colour (0xff272c36), rail.getX(), rail.getBottom(), false));
    g.fillRoundedRectangle (rail.reduced (0.0f, 4.0f), 2.5f);
    g.setColour (juce::Colours::black.withAlpha (0.65f));
    g.drawRoundedRectangle (rail.reduced (0.0f, 4.0f), 2.5f, 1.0f);

    for (int i = 0; i < 3; ++i)
    {
        auto can = juce::Rectangle<float> (28.0f, rail.getHeight()).withCentre ({ canX[i], rail.getCentreY() });
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRoundedRectangle (can.translated (1.5f, 2.0f), 3.0f);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff4a5261), can.getX(), can.getY(),
                                                 juce::Colour (0xff181b22), can.getX(), can.getBottom(), false));
        g.fillRoundedRectangle (can, 3.0f);
        g.setColour (juce::Colours::black.withAlpha (0.7f));
        g.drawRoundedRectangle (can, 3.0f, 1.0f);

        auto coil = can.reduced (5.0f, 5.0f);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xffd08a4a), coil.getX(), coil.getY(),
                                                 juce::Colour (0xff6b3a18), coil.getX(), coil.getBottom(), false));
        g.fillRoundedRectangle (coil, 1.5f);
        g.setColour (juce::Colours::black.withAlpha (0.35f));

        for (float y = coil.getY() + 1.5f; y < coil.getBottom(); y += 2.0f)
            g.drawHorizontalLine ((int) y, coil.getX(), coil.getRight());

        g.setColour (juce::Colours::white.withAlpha (0.12f));
        g.drawHorizontalLine ((int) can.getY() + 1, can.getX() + 3.0f, can.getRight() - 3.0f);
    }
}

void HazeEditor::paintSpring (juce::Graphics& g, float cx, float top, float bottom, int index)
{
    using namespace theme;
    const float radius = 9.5f, pitch = 7.0f, tilt = 3.2f;   // each coil is a tilted ellipse, so the helix reads in 3D
    const float y0 = top + 8.0f, y1 = bottom - 8.0f;
    const float coils = (y1 - y0) / pitch;
    const int steps = (int) (coils * 16.0f);
    const float sway = 0.7f + 5.5f * juce::jlimit (0.0f, 1.0f, tankLevel * 2.0f);
    const float offset = (float) index * 2.1f;

    auto bendAt = [&] (float t)
    {
        return std::sin (t * 6.3f + wobblePhase + offset) * sway * std::sin (t * juce::MathConstants<float>::pi);
    };

    // Hook wires from the transducers to the coil
    g.setColour (juce::Colour (0xff9aa4b3));
    g.drawLine (cx, top, cx + bendAt (0.0f), y0, 1.3f);
    g.drawLine (cx, bottom, cx + bendAt (1.0f), y1, 1.3f);

    // The helix: back half dim, front half bright, built as two sets of subpaths; plus a
    // soft shadow of the whole coil on the back of the pan
    juce::Path back, front, shadow;
    bool frontOpen = false, backOpen = false;
    float prevX = cx + radius, prevY = y0;
    shadow.startNewSubPath (cx + 5.0f, y0);

    for (int k = 0; k <= steps; ++k)
    {
        const float t = (float) k / (float) steps;
        const float theta = t * coils * juce::MathConstants<float>::twoPi + (float) index * 1.3f;
        const float depth = std::sin (theta);
        const float bend = bendAt (t);
        const float y = y0 + t * (y1 - y0) + depth * tilt;
        const float x = cx + bend + std::cos (theta) * radius;
        const bool isFront = depth > 0.0f;
        auto& path = isFront ? front : back;
        bool& open = isFront ? frontOpen : backOpen;
        bool& other = isFront ? backOpen : frontOpen;

        if (! open)
        {
            path.startNewSubPath (prevX, prevY);
            open = true;
            other = false;
        }

        path.lineTo (x, y);
        prevX = x;
        prevY = y;

        if (k % 8 == 0)
            shadow.lineTo (cx + 5.0f + bend, y0 + t * (y1 - y0));
    }

    g.setColour (juce::Colours::black.withAlpha (0.28f));
    g.strokePath (shadow, juce::PathStrokeType (radius * 2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (juce::Colours::black.withAlpha (0.22f));
    g.strokePath (shadow, juce::PathStrokeType (radius * 2.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Back wire bright enough to close each turn into an ellipse against the dark well
    g.setColour (juce::Colour (0xff4e5868).withAlpha (0.9f));
    g.strokePath (back, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Front wire: a steel body and a thin specular line on its upper-left edge
    g.setColour (juce::Colour (0xff6f7a8b));
    g.strokePath (front, juce::PathStrokeType (2.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (juce::Colour (0xffe9eef5).withAlpha (0.85f));
    g.strokePath (front, juce::PathStrokeType (0.9f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded),
                  juce::AffineTransform::translation (-0.6f, -0.6f));

    // Shimmer: each spring carries one chorus voice; its glint rides that voice's LFO and
    // fades out when the voice is switched off
    const float level = voiceLit[index];

    if (level < 0.02f)
        return;

    const float glintY = (y0 + y1) * 0.5f + voiceSmooth[index] * (y1 - y0) * 0.38f;
    {
        juce::Graphics::ScopedSaveState state (g);
        g.addTransform (juce::AffineTransform::scale (1.0f, 2.4f, cx, glintY));
        g.setGradientFill (juce::ColourGradient (teal.withAlpha (0.26f * level), cx, glintY,
                                                 teal.withAlpha (0.0f), cx + 26.0f, glintY, true));
        g.fillEllipse (juce::Rectangle<float> (52.0f, 52.0f).withCentre ({ cx, glintY }));
    }

    juce::Graphics::ScopedSaveState state (g);
    auto glint = juce::Rectangle<float> (radius * 2.0f + 14.0f, 64.0f).withCentre ({ cx, glintY });
    g.reduceClipRegion (glint.toNearestInt());
    juce::ColourGradient sheen (teal.withAlpha (0.0f), cx, glint.getY(), teal.withAlpha (0.0f), cx, glint.getBottom(), false);
    sheen.addColour (0.5, juce::Colour (0xffe8fffb).withAlpha (0.95f * level));
    g.setGradientFill (sheen);
    g.strokePath (front, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded),
                  juce::AffineTransform::translation (-0.4f, -0.4f));
}

void HazeEditor::animate (float seconds)
{
    const float follow = std::min (1.0f, seconds * 14.0f);
    const int voices = haze.getVoiceCount();
    const float width = haze.getWidth();
    lfoSmooth += (haze.getLfoPosition() - lfoSmooth) * follow;

    for (int v = 0; v < 3; ++v)
    {
        // Voice presence follows the setting directly, so the art answers the Voices keys
        // even while the host is stopped; the sweep positions come from the audio thread
        voiceSmooth[v] += (haze.getVoicePosition (v) - voiceSmooth[v]) * follow;
        voiceLit[v] += ((v < voices ? 1.0f : 0.0f) - voiceLit[v]) * std::min (1.0f, seconds * 8.0f);
        voicePanSmooth[v] += (HazeProcessor::voicePanFor (v, voices, width) - voicePanSmooth[v]) * std::min (1.0f, seconds * 10.0f);
    }

    tankLevel = std::max (haze.getSpringLevel(), tankLevel * std::exp (-seconds * 5.0f));
    wobblePhase += seconds * (2.5f + 30.0f * juce::jlimit (0.0f, 1.0f, tankLevel * 2.0f));

    // Spread does nothing with a single voice: its arc goes dim then, though it stays
    // adjustable so it can be set before adding voices
    if (const bool active = voices > 1; active != spreadActive)
    {
        spreadActive = active;
        spreadKnob->getSlider().setColour (juce::Slider::rotarySliderFillColourId, active ? theme::violet : theme::textDim.darker (0.3f));
        spreadKnob->repaint();
    }

    repaint (graphic);
    repaint (lfoLamp);
    repaint (field);
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new HazeProcessor();
}
