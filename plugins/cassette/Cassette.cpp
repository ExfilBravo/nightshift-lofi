#include "Cassette.h"
#include "common/ScaledEditor.h"

using namespace lofi;
using namespace lofi::params;

//==============================================================================
// Type character. Ferric is soft and dull with more hiss, chrome is the balanced
// reference, metal takes more level before it folds and stays bright.
const CassetteProcessor::TypeProfile CassetteProcessor::profiles[3] =
{
    //  preEmphDb  tiltDb  blend  biasScale  hissLpHz  hissDb
    {   4.0f,     -1.5f,  0.0f,  1.3f,      7000.0f,  2.5f },   // I   ferric
    {   2.5f,      0.0f,  0.5f,  1.0f,      9000.0f,  0.0f },   // II  chrome
    {   1.5f,      1.0f,  1.0f,  0.7f,     12000.0f, -3.0f },   // IV  metal
};

CassetteProcessor::CassetteProcessor()
    : LofiProcessor ({ "Nightshift Cassette", "tape machine", theme::amber }, createLayout())
{
    pType     = apvts.getRawParameterValue ("type");
    pDrive    = apvts.getRawParameterValue ("drive");
    pAge      = apvts.getRawParameterValue ("age");
    pBump     = apvts.getRawParameterValue ("bump");
    pWow      = apvts.getRawParameterValue ("wow");
    pFlutter  = apvts.getRawParameterValue ("flutter");
    pWidth    = apvts.getRawParameterValue ("width");
    pHiss     = apvts.getRawParameterValue ("hiss");
    pDropouts = apvts.getRawParameterValue ("dropouts");
}

LofiProcessor::Layout CassetteProcessor::createLayout()
{
    Layout layout;
    layout.add (choice   ("type",     "Tape",     { "Type I  ferric", "Type II  chrome", "Type IV  metal" }, 1));
    layout.add (percent  ("drive",    "Drive",    35.0f));
    layout.add (percent  ("age",      "Age",      30.0f));
    layout.add (percent  ("bump",     "Bump",     40.0f));
    layout.add (percent  ("wow",      "Wow",      25.0f));
    layout.add (percent  ("flutter",  "Flutter",  20.0f));
    layout.add (percent  ("width",    "Width",    50.0f));
    layout.add (decibels ("hiss",     "Hiss",     -90.0f, -30.0f, -58.0f));
    layout.add (percent  ("dropouts", "Dropouts", 10.0f));
    addCommonParameters (layout);
    return layout;
}

juce::AudioProcessorEditor* CassetteProcessor::createEditor()
{
    return new lofi::ScaledEditor (*this, new CassetteEditor (*this));
}

//==============================================================================
void CassetteProcessor::prepareDsp (const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate;
    numChannels = (int) juce::jlimit (1u, 2u, spec.numChannels);

    // 4x oversampled saturation, linear phase, with the fractional part of the FIR
    // latency absorbed so the reported latency is a whole number of samples
    oversampling = std::make_unique<juce::dsp::Oversampling<float>> ((size_t) numChannels, 2,
                        juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true);
    oversampling->initProcessing ((size_t) chunkSize);
    oversampling->reset();

    // The wow & flutter delay wanders around baseDelay; ModDelay::read (d) delays by
    // exactly d, so the wet path's latency is the FIR's plus baseDelay
    const int osLatency = juce::roundToInt (oversampling->getLatencyInSamples());
    baseDelay = juce::roundToInt (baseDelaySeconds * sampleRate);
    latency = osLatency + baseDelay;

    // Room for the position to move: wow 100 needs about +-200 samples at 48 kHz, so the
    // soft wall at 0.72 x 360 is a safety, not part of the sound. A 0.12 Hz leak keeps
    // the position centred (it costs the 0.41 Hz wow line about 0.4 dB).
    posSoft = baseDelay * 0.72;
    posLimit = baseDelay - 4.0;
    posRestore = 0.002 * 48000.0 / sampleRate;
    leak = 1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * 0.12 / sampleRate);

    const double sr = sampleRate;
    envAttack     = (float) (1.0 - std::exp (-1.0 / (0.0005 * sr)));
    envRelease    = (float) (1.0 - std::exp (-1.0 / (0.040 * sr)));
    envSlowCoef   = (float) (1.0 - std::exp (-1.0 / (0.150 * sr)));
    transientCoef = (float) (1.0 - std::exp (-1.0 / (0.004 * sr)));

    const juce::uint32 seeds[2][2] { { 0x1f2e3d4cu, 0x7a6b5c4du }, { 0x9e8d7c6bu, 0x3c2b1a09u } };

    for (int c = 0; c < 2; ++c)
    {
        auto& k = ch[c];

        for (auto* f : { &k.preEmph, &k.deEmph, &k.tilt, &k.bumpShelf, &k.bumpBell, &k.age })
            f->prepare (sr);

        k.dc.prepare (sr);
        k.delay.prepare (sr, (float) (2000.0 * baseDelaySeconds + 2.0));   // base + the full excursion, ms
        k.dropLp.prepare (sr);
        k.dropLp.setCutoff (2000.0f);
        k.hiss.prepare (sr, seeds[c][0]);
        k.wobble.prepare (sr, seeds[c][1]);
        k.pos = 0.0;
        k.envFast = k.envSlow = k.transient = 0.0f;
        k.ageFc = -1.0f;
    }

    dropouts.prepare (sr, 0x5eedbeefu);

    for (int t = 0; t < 3; ++t)
        hissCalib[t] = 1.0f / std::max (1.0e-6f, cassette::Hiss::measureRms (sr, profiles[t].hissLpHz));

    for (auto* s : { &bias, &blend, &preEmphDb, &tiltDb, &bumpDb, &ageFc, &ageAmount, &wowCents, &flutterCents, &widthAngle, &hissTone })
        lofi::dsp::prepareSmoothed (*s, sr, 0.0f, 0.03);

    for (auto* s : { &driveGain, &postGain, &hissGain })
    {
        s->reset (sr, 0.03);
        s->setCurrentAndTargetValue (1.0f);
    }

    lastDriveForPost = -1.0f;
    lastTypeForPost = -1;
    updateTargets();
    snapToTargets();
    snapPending = true;      // and again on the first block, for parameters set after this
    resumeHold = 0;
    resumePos = resumeLength = 1;
    setTailSeconds (0.5);
}

void CassetteProcessor::resetDsp()
{
    // Called on the audio thread when leaving bypass: return the wet path to silence
    // without allocating. Noise generators and the wobble's phases just carry on.
    if (oversampling != nullptr)
        oversampling->reset();

    for (auto& k : ch)
    {
        for (auto* f : { &k.preEmph, &k.deEmph, &k.tilt, &k.bumpShelf, &k.bumpBell, &k.age })
            f->reset();

        k.dc.reset();
        k.delay.reset();
        k.dropLp.prime (0.0f);
        k.pos = 0.0;
        k.envFast = k.envSlow = k.transient = 0.0f;
        k.ageFc = -1.0f;
    }

    dropouts.cancel();
    dropoutActivity.store (0.0f);
    snapPending = true;

    resumeHold = latency;
    resumePos = 0;
    resumeLength = std::max (1, juce::roundToInt (0.006 * sampleRate));
}

void CassetteProcessor::updateTargets()
{
    const int type = juce::jlimit (0, 2, juce::roundToInt (pType->load()));
    const auto& prof = profiles[type];
    const float drive = pDrive->load();

    // Drive: -6 dB .. +18 dB into the curve; the output gain holds the loudness of a
    // -12 dBFS signal, so turning it up squashes rather than just gets louder
    const float g = lofi::dsp::dbToGain (-6.0f + 0.24f * drive) * saturatorScale;
    const float biasValue = 0.03f * prof.biasScale;

    if (drive != lastDriveForPost || type != lastTypeForPost)
    {
        postTarget = referenceAmp / cassette::tapeFundamental (referenceAmp * g, prof.blend, biasValue);
        lastDriveForPost = drive;
        lastTypeForPost = type;
    }

    driveGain.setTargetValue (g);
    postGain.setTargetValue (postTarget);
    bias.setTargetValue (biasValue);
    blend.setTargetValue (prof.blend);
    preEmphDb.setTargetValue (prof.preEmphDb);
    tiltDb.setTargetValue (prof.tiltDb);

    bumpDb.setTargetValue (5.0f * pBump->load() * 0.01f);

    const float age = pAge->load() * 0.01f;
    ageFc.setTargetValue (20000.0f * std::pow (0.25f, age));          // 20 kHz .. 5 kHz
    ageAmount.setTargetValue (age);

    // Nominal depths; the mechanism's sines and wander add up to about 1.2x these at
    // their peaks, which is what --pitch reads: 25 % wow -> about 5 cents, 100 -> 25
    wowCents.setTargetValue (22.0f * std::pow (pWow->load() * 0.01f, 1.15f));
    flutterCents.setTargetValue (6.2f * pFlutter->load() * 0.01f);              // default 20 % -> about 1.5 cents, 100 % -> about 8
    widthAngle.setTargetValue (pWidth->load() * 0.01f * juce::MathConstants<float>::halfPi);

    hissGain.setTargetValue (std::max (1.0e-7f, lofi::dsp::dbToGain (pHiss->load() + prof.hissDb) * hissCalib[type]));
    hissTone.setTargetValue (prof.hissLpHz);

    dropouts.setAmount (pDropouts->load() * 0.01f);
}

void CassetteProcessor::snapToTargets()
{
    for (auto* s : { &bias, &blend, &preEmphDb, &tiltDb, &bumpDb, &ageFc, &ageAmount, &wowCents, &flutterCents, &widthAngle, &hissTone })
        s->setCurrentAndTargetValue (s->getTargetValue());

    for (auto* s : { &driveGain, &postGain, &hissGain })
        s->setCurrentAndTargetValue (s->getTargetValue());

    lastPre = lastTilt = lastBump = -1000.0f;
    lastHissHz = -1.0f;

    for (auto& k : ch)
        k.ageFc = -1.0f;

    coeffPhase = 0;
    updateCoefficients (numChannels);

    // Start the wow & flutter position where the transport's sines put it, so the
    // wandering delay is centred on the reported latency from the first sample
    const double wowNow = wowCents.getCurrentValue(), flutterNow = flutterCents.getCurrentValue();
    const double posA = ch[0].wobble.centredPosition (wowNow, flutterNow);
    const double posB = ch[1].wobble.centredPosition (wowNow, flutterNow);
    ch[0].pos = juce::jlimit (-posSoft, posSoft, posA);
    ch[1].pos = juce::jlimit (-posSoft, posSoft, widthCos * posA + widthSin * posB);
}

void CassetteProcessor::updateCoefficients (int numCh)
{
    const float pre = preEmphDb.getCurrentValue(), tl = tiltDb.getCurrentValue(), bd = bumpDb.getCurrentValue();
    const float hissHz = hissTone.getCurrentValue();
    const float ageHz = ageFc.getCurrentValue(), ageAmt = ageAmount.getCurrentValue();
    const bool eqChanged = pre != lastPre || tl != lastTilt || bd != lastBump || hissHz != lastHissHz;

    for (int c = 0; c < numCh; ++c)
    {
        auto& k = ch[c];

        if (eqChanged)
        {
            k.preEmph.set (cassette::Svf::Mode::highShelf, 4000.0f, 0.6f, pre);
            k.deEmph.set (cassette::Svf::Mode::highShelf, 4000.0f, 0.6f, -pre);
            k.tilt.set (cassette::Svf::Mode::highShelf, 7000.0f, 0.6f, tl);
            k.bumpShelf.set (cassette::Svf::Mode::lowShelf, 120.0f, 0.8f, bd * 0.8f);
            k.bumpBell.set (cassette::Svf::Mode::bell, 80.0f, 0.7f, bd * 0.4f);
            k.hiss.setTone (hissHz);
        }

        // Age: a gentle 2-pole lowpass that dips further on transients (worn oxide smears attacks)
        const float fc = ageHz / (1.0f + 1.5f * ageAmt * k.transient);

        if (fc != k.ageFc)
        {
            k.age.set (cassette::Svf::Mode::lowpass, fc, 0.6f);
            k.ageFc = fc;
        }
    }

    lastPre = pre;
    lastTilt = tl;
    lastBump = bd;
    lastHissHz = hissHz;

    const float angle = widthAngle.getCurrentValue();
    widthCos = std::cos (angle);
    widthSin = std::sin (angle);
}

void CassetteProcessor::processDsp (juce::AudioBuffer<float>& buffer)
{
    const int numCh = std::min (buffer.getNumChannels(), numChannels);
    const int numSamples = buffer.getNumSamples();

    if (numCh <= 0 || numSamples <= 0)
        return;

    // The editor's reels follow the host transport when it reports one
    int playing = -1;

    if (auto* head = getPlayHead())
        if (const auto position = head->getPosition())
            playing = position->getIsPlaying() ? 1 : 0;

    hostPlaying.store (playing);

    updateTargets();

    if (snapPending)
    {
        snapToTargets();
        snapPending = false;
    }

    double wobbleSum = 0.0, centsSum = 0.0;
    float dropPeak = 0.0f;
    float* chunkPtrs[2] {};

    for (int start = 0; start < numSamples; start += chunkSize)
    {
        const int len = std::min (chunkSize, numSamples - start);

        for (int c = 0; c < numCh; ++c)
            chunkPtrs[c] = buffer.getWritePointer (c) + start;

        chunkStart = start;
        processChunk (chunkPtrs, numCh, len, wobbleSum, centsSum, dropPeak);
    }

    transportWobble.store (juce::jlimit (0.4f, 1.6f, 1.0f + 40.0f * (float) (wobbleSum / numSamples)));
    speedCents.store ((float) (centsSum / numSamples));
    dropoutActivity.store (dropPeak);
}

void CassetteProcessor::processChunk (float* const* data, int numCh, int len, double& wobbleSum, double& centsSum, float& dropPeak)
{
    // 1. Drive and pre-emphasis at the base rate; the curve's bias and knee per sample
    for (int i = 0; i < len; ++i)
    {
        const float g = driveGain.getNextValue();
        const float b = bias.getNextValue();
        const float bl = blend.getNextValue();
        biasBuf[i] = b;
        blendBuf[i] = bl;
        offsetBuf[i] = cassette::tapeCurve (b, bl);

        for (int c = 0; c < numCh; ++c)
            data[c][i] = ch[c].preEmph.process (data[c][i] * g);
    }

    // 2. The tape curve, 4x oversampled so its harmonics stay below Nyquist
    juce::dsp::AudioBlock<float> block (data, (size_t) numCh, 0, (size_t) len);
    auto up = oversampling->processSamplesUp (block);
    const int upSamples = (int) up.getNumSamples();

    for (int c = 0; c < numCh; ++c)
    {
        float* s = up.getChannelPointer ((size_t) c);

        for (int j = 0; j < upSamples; ++j)
        {
            const int i = std::min (len - 1, j >> 2);
            s[j] = cassette::tapeCurve (s[j] + biasBuf[i], blendBuf[i]) - offsetBuf[i];
        }
    }

    oversampling->processSamplesDown (block);

    // 3. Everything after the heads, per sample
    for (int i = 0; i < len; ++i)
    {
        if (coeffPhase == 0)
            updateCoefficients (numCh);

        coeffPhase = (coeffPhase + 1) & 15;

        const float pg = postGain.getNextValue();
        const float wowC = wowCents.getNextValue();
        const float flutC = flutterCents.getNextValue();
        const float hg = hissGain.getNextValue();
        preEmphDb.getNextValue(); tiltDb.getNextValue(); bumpDb.getNextValue();
        ageFc.getNextValue(); ageAmount.getNextValue(); widthAngle.getNextValue(); hissTone.getNextValue();

        // Coming back from a reset: silence until the live signal reaches the output,
        // then a short raised-cosine rise (see resumeHold in the header)
        float fade = 1.0f;

        if (resumeHold > 0)
        {
            --resumeHold;
            fade = 0.0f;
        }
        else if (resumePos < resumeLength)
        {
            fade = 0.5f - 0.5f * std::cos (cassette::kPi * (float) resumePos / (float) resumeLength);
            ++resumePos;
        }

        float dipL = 0.0f, dipR = 0.0f;
        dropouts.next (dipL, dipR);

        float wA = 0.0f, fA = 0.0f, wB = 0.0f, fB = 0.0f;
        ch[0].wobble.next (wA, fA);

        if (numCh > 1)
            ch[1].wobble.next (wB, fB);

        for (int c = 0; c < numCh; ++c)
        {
            auto& k = ch[c];
            float x = data[c][i] * pg;

            x = k.dc.process (x);
            x = k.deEmph.process (x);
            x = k.tilt.process (x);
            x = k.bumpShelf.process (x);
            x = k.bumpBell.process (x);

            // Transient detector feeding the age filter's cutoff
            const float a = std::abs (x);
            k.envFast += (a - k.envFast) * (a > k.envFast ? envAttack : envRelease);
            k.envSlow += (k.envFast - k.envSlow) * envSlowCoef;
            const float ratio = juce::jlimit (0.0f, 1.0f, (k.envFast - k.envSlow) / (k.envSlow + 0.01f));
            k.transient += (ratio - k.transient) * transientCoef;

            x = k.age.process (x);

            // Wow & flutter: a speed error in cents, integrated into the read position
            const float w = c == 0 ? wA : widthCos * wA + widthSin * wB;
            const float f = c == 0 ? fA : widthCos * fA + widthSin * fB;
            const float cents = wowC * w + flutC * f;
            const double r = cents * 5.776227e-4;                 // ln 2 / 1200
            const double e = r * (1.0 + 0.5 * r);                 // 2^(cents/1200) - 1
            k.pos += e - k.pos * leak;

            const double over = std::abs (k.pos) - posSoft;

            if (over > 0.0)
                k.pos -= (k.pos > 0.0 ? over : -over) * posRestore;

            k.pos = juce::jlimit (-posLimit, posLimit, k.pos);
            k.delay.push (x);
            x = k.delay.read ((float) ((double) baseDelay - k.pos));

            if (c == 0)
            {
                wobbleSum += e;
                centsSum += cents;
            }

            // Dropout: level dip with the highs going first
            const float dip = c == 0 ? dipL : dipR;
            const float dull = k.dropLp.process (x);
            x = (x + dip * (dull - x)) * (1.0f - dip);

            // Hiss, after the dropouts (it is the tape's own noise floor); quiet while the
            // host is stopped
            x += k.hiss.next() * hg * transportGateAt (chunkStart + i);
            data[c][i] = x * fade;
        }

        dropPeak = std::max (dropPeak, std::max (dipL, dipR));
    }
}

//==============================================================================
TypeSelector::TypeSelector (juce::AudioProcessorValueTreeState& state, const juce::String& paramId)
    : parameter (*state.getParameter (paramId)),
      attachment (parameter, [this] (float v) { current = juce::jlimit (0, 2, juce::roundToInt (v)); repaint(); }, state.undoManager)
{
    attachment.sendInitialUpdate();
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setTooltip ("Tape formulation: I ferric (soft, dull, hissy), II chrome (balanced), IV metal (bright, hard, quiet)");
}

juce::Colour TypeSelector::accentFor (int type)
{
    switch (type)
    {
        case 0:  return theme::amber.darker (0.15f);
        case 2:  return theme::violet;
        default: return theme::teal;
    }
}

int TypeSelector::positionAt (int y) const
{
    const float top = 30.0f, step = (getHeight() - 46.0f) / 2.0f;
    return juce::jlimit (0, 2, juce::roundToInt ((y - top) / step));
}

void TypeSelector::mouseDown (const juce::MouseEvent& e)
{
    const int index = positionAt (e.y);

    if (index != current)
        attachment.setValueAsCompleteGesture ((float) index);
}

void TypeSelector::paint (juce::Graphics& g)
{
    using namespace theme;
    auto r = getLocalBounds();

    g.setColour (textDim.brighter (0.15f));
    g.setFont (font (11.5f, true).withExtraKerningFactor (0.06f));
    g.drawText ("TYPE", r.removeFromTop (16), juce::Justification::centred, true);

    // The slide switch: a dark slot with a sliding cap, three detents
    const float top = 30.0f, step = (getHeight() - 46.0f) / 2.0f;
    auto slot = juce::Rectangle<float> (5.0f, top - 12.0f, 21.0f, step * 2.0f + 24.0f);
    g.setColour (juce::Colour (0xff0b0d11));
    g.fillRoundedRectangle (slot, 5.0f);
    g.setColour (outline);
    g.drawRoundedRectangle (slot, 5.0f, 1.0f);

    static const char* numerals[] = { "I", "II", "IV" };
    static const char* names[] = { "FERRIC", "CHROME", "METAL" };
    const float textX = slot.getRight() + 4.0f, textW = (float) getWidth() - textX;

    for (int i = 0; i < 3; ++i)
    {
        const float cy = top + i * step;
        g.setColour (juce::Colours::white.withAlpha (0.12f));
        g.fillRect (slot.getX() + 6.0f, cy - 0.5f, slot.getWidth() - 12.0f, 1.0f);

        const bool active = i == current;
        g.setColour (active ? accentFor (i) : textDim);
        g.setFont (font (12.5f, true));
        g.drawText (numerals[i], juce::Rectangle<float> (textX, cy - 11.0f, textW, 12.0f), juce::Justification::centredLeft, true);
        g.setColour (active ? text : textDim);
        g.setFont (font (11.5f, true).withExtraKerningFactor (0.04f));
        g.drawText (names[i], juce::Rectangle<float> (textX, cy + 1.0f, textW, 14.0f), juce::Justification::centredLeft, true);
    }

    // The cap, lit by the chosen type
    const float cy = top + current * step;
    auto cap = juce::Rectangle<float> (slot.getX() + 3.0f, cy - 9.0f, slot.getWidth() - 6.0f, 18.0f);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff4a5163), cap.getCentreX(), cap.getY(),
                                             juce::Colour (0xff22262f), cap.getCentreX(), cap.getBottom(), false));
    g.fillRoundedRectangle (cap, 3.0f);
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.drawRoundedRectangle (cap, 3.0f, 1.0f);
    g.setColour (accentFor (current));
    g.fillRoundedRectangle (cap.getX() + 3.0f, cy - 1.0f, cap.getWidth() - 6.0f, 2.0f, 1.0f);
}

//==============================================================================
TransportKeys::TransportKeys (juce::AudioProcessorValueTreeState& state)
    : bypass (*state.getParameter ("bypass")),
      attachment (bypass, [this] (float v) { bypassed = v >= 0.5f; repaint(); }, state.undoManager)
{
    attachment.sendInitialUpdate();
    setWantsKeyboardFocus (false);
}

juce::Rectangle<float> TransportKeys::keyBounds (int index) const
{
    const float gap = 4.0f, w = ((float) getWidth() - 2.0f * gap) / 3.0f;
    return { (float) index * (w + gap), 6.0f, w, (float) getHeight() - 12.0f };
}

int TransportKeys::keyAt (juce::Point<int> p) const
{
    for (int i = 0; i < 3; ++i)
        if (keyBounds (i).contains (p.toFloat()))
            return i;

    return -1;
}

void TransportKeys::mouseMove (const juce::MouseEvent& e)
{
    const int k = keyAt (e.getPosition());

    if (k == hover)
        return;

    hover = k;
    static const char* tips[] = { "Rewind: hold to spool the reels back (animation only, the audio is untouched)",
                                  "Play: lit, the tape is engaged; click to bypass it, click again to engage",
                                  "Fast forward: hold to spool the reels on (animation only, the audio is untouched)" };
    setTooltip (k >= 0 ? tips[k] : "");
    setMouseCursor (k >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    repaint();
}

void TransportKeys::mouseExit (const juce::MouseEvent&)
{
    hover = -1;
    repaint();
}

void TransportKeys::mouseDown (const juce::MouseEvent& e)
{
    pressed = keyAt (e.getPosition());

    if (pressed == 1)
        attachment.setValueAsCompleteGesture (bypassed ? 0.0f : 1.0f);
    else
        spool = pressed == 0 ? -1 : (pressed == 2 ? 1 : 0);

    repaint();
}

void TransportKeys::mouseUp (const juce::MouseEvent&)
{
    pressed = -1;
    spool = 0;
    repaint();
}

void TransportKeys::paint (juce::Graphics& g)
{
    using namespace theme;

    for (int i = 0; i < 3; ++i)
    {
        const bool latched = i == 1 && ! bypassed;
        const bool down = pressed == i || latched;
        auto key = keyBounds (i).translated (0.0f, down ? 1.0f : 0.0f);
        auto top = juce::Colour (down ? 0xff20242d : 0xff262b36), bottom = juce::Colour (down ? 0xff13151b : 0xff171a21);

        if (hover == i)
        {
            top = top.brighter (0.08f);
            bottom = bottom.brighter (0.08f);
        }

        g.setGradientFill (juce::ColourGradient (top, key.getCentreX(), key.getY(), bottom, key.getCentreX(), key.getBottom(), false));
        g.fillRoundedRectangle (key, 5.0f);

        if (down)
        {
            // Pressed in: shadow along the top edge instead of the highlight
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.fillRoundedRectangle (key.withHeight (3.0f).reduced (2.0f, 0.0f).translated (0.0f, 1.0f), 1.5f);
        }
        else
        {
            g.setColour (juce::Colours::white.withAlpha (0.06f));
            g.fillRoundedRectangle (key.withHeight (key.getHeight() * 0.5f).reduced (1.0f, 1.0f), 4.0f);
        }

        g.setColour (juce::Colours::black.withAlpha (0.8f));
        g.drawRoundedRectangle (key, 5.0f, 1.0f);

        const auto c = key.getCentre();
        juce::Path symbol;

        if (i == 0)
        {
            symbol.addTriangle (c.x - 2.0f, c.y, c.x + 8.0f, c.y - 7.0f, c.x + 8.0f, c.y + 7.0f);
            symbol.addTriangle (c.x - 12.0f, c.y, c.x - 2.0f, c.y - 7.0f, c.x - 2.0f, c.y + 7.0f);
        }
        else if (i == 1)
        {
            symbol.addTriangle (c.x - 6.0f, c.y - 8.0f, c.x + 8.0f, c.y, c.x - 6.0f, c.y + 8.0f);
        }
        else
        {
            symbol.addTriangle (c.x - 8.0f, c.y - 7.0f, c.x + 2.0f, c.y, c.x - 8.0f, c.y + 7.0f);
            symbol.addTriangle (c.x + 2.0f, c.y - 7.0f, c.x + 12.0f, c.y, c.x + 2.0f, c.y + 7.0f);
        }

        if (latched)
        {
            g.setColour (ledOn.withAlpha (0.14f));
            g.fillRoundedRectangle (key.reduced (1.0f), 4.0f);
            g.setColour (ledOn);
        }
        else
        {
            g.setColour (pressed == i ? text : textDim.withAlpha (hover == i ? 0.8f : 0.55f));
        }

        g.fillPath (symbol);
    }
}

//==============================================================================
CassetteEditor::CassetteEditor (CassetteProcessor& p)
    : LofiEditor (p), cassette (p)
{
    typeSelector = std::make_unique<TypeSelector> (p.getState(), "type");
    addAndMakeVisible (*typeSelector);

    transportKeys = std::make_unique<TransportKeys> (p.getState());
    addAndMakeVisible (*transportKeys);

    tapeKnobs   = { &addKnob ("drive", "Drive"), &addKnob ("age", "Age"), &addKnob ("bump", "Bump") };
    motionKnobs = { &addKnob ("wow", "Wow", theme::teal), &addKnob ("flutter", "Flutter", theme::teal), &addKnob ("width", "Width", theme::teal) };
    noiseKnobs  = { &addKnob ("hiss", "Hiss", theme::rose), &addKnob ("dropouts", "Dropouts", theme::rose) };

    auto& mix = addKnob ("mix", "Mix", theme::violet);
    auto& output = addKnob ("output", "Output", theme::violet, true);
    outKnobs = { &mix, &output };

    setSize (820, 460);
}

void CassetteEditor::layoutBody (juce::Rectangle<int> body)
{
    graphic = body.removeFromLeft (292);
    body.removeFromLeft (10);

    auto row1 = body.removeFromTop (body.getHeight() / 2);
    auto row2 = body;

    // Sections a clear gutter apart, so the titles are not the only thing grouping them
    tapeArea = row1.removeFromLeft (312);
    row1.removeFromLeft (18);
    noiseArea = row1;
    motionArea = row2.removeFromLeft (312);
    row2.removeFromLeft (18);
    outArea = row2;

    // A section: title on top, the knobs hanging just under it (the slack goes below),
    // an optional slot on the left, knobs in equal cells
    auto place = [] (std::vector<Knob*>& knobs, juce::Rectangle<int> area, int leftSlot)
    {
        area.removeFromTop (18);
        area = area.removeFromTop (122).withTrimmedTop (6);
        auto slot = area.removeFromLeft (leftSlot);
        const int w = area.getWidth() / std::max (1, (int) knobs.size());

        for (auto* k : knobs)
            k->setBounds (area.removeFromLeft (w).withSizeKeepingCentre (78, 112));

        return slot.withSizeKeepingCentre (leftSlot, 112);
    };

    // 78: room for CHROME in Helvetica Neue (a Mac's), and still a knob's width (78) for each of the three
    typeSelector->setBounds (place (tapeKnobs, tapeArea, 78));
    meterArea = place (motionKnobs, motionArea, 74);
    place (noiseKnobs, noiseArea, 0);
    place (outKnobs, outArea, 0);

    // Cassette geometry, in graphic-local pixels
    shell = { 14.0f, 20.0f, 264.0f, 190.0f };
    label = { shell.getX() + 15.0f, shell.getY() + 10.0f, shell.getWidth() - 30.0f, 139.0f };
    window = { shell.getCentreX() - 86.0f, label.getY() + 52.0f, 172.0f, 60.0f };
    reelL = { shell.getCentreX() - 57.0f, window.getCentreY() };
    reelR = { shell.getCentreX() + 57.0f, window.getCentreY() };
    tapeY = shell.getBottom() - 6.0f;
    rollerL = { shell.getX() + 32.0f, tapeY - 4.5f };
    rollerR = { shell.getRight() - 32.0f, tapeY - 4.5f };
    deckStrip = { 14.0f, 222.0f, 264.0f, 56.0f };
    counterBox = { deckStrip.getRight() - 118.0f, deckStrip.getY() + 7.0f, 106.0f, 32.0f };
    keys = { 14.0f, 290.0f, 264.0f, 60.0f };

    transportKeys->setBounds (keys.translated ((float) graphic.getX(), (float) graphic.getY()).toNearestInt());
    shellImage = juce::Image();
}

float CassetteEditor::packRadius (float position)
{
    // Area of tape wound on a hub grows linearly with the length on it
    const float hub = 23.0f, full = 52.0f;
    return std::sqrt (hub * hub + juce::jlimit (0.0f, 1.0f, position) * (full * full - hub * hub));
}

void CassetteEditor::renderShell (int type, float scale)
{
    using namespace theme;

    // Drawn at the display's physical resolution, so the label text and edges stay as
    // crisp as the vector knobs at 125-200 % scaling
    shellImage = juce::Image (juce::Image::ARGB, std::max (1, juce::roundToInt ((float) graphic.getWidth() * scale)),
                              std::max (1, juce::roundToInt ((float) graphic.getHeight() * scale)), true);
    juce::Graphics g (shellImage);
    g.addTransform (juce::AffineTransform::scale (scale));
    const auto accent = TypeSelector::accentFor (type);

    // The cassette well: a dark recess in the faceplate
    auto well = juce::Rectangle<float> (3.0f, 3.0f, graphic.getWidth() - 6.0f, graphic.getHeight() - 6.0f);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff11141b), well.getCentreX(), well.getY(),
                                             juce::Colour (0xff090b0f), well.getCentreX(), well.getBottom(), false));
    g.fillRoundedRectangle (well, 12.0f);
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.drawRoundedRectangle (well.reduced (2.0f), 11.0f, 3.0f);
    g.setColour (outline);
    g.drawRoundedRectangle (well, 12.0f, 1.5f);

    // Shell: shadow, smoked body, rim light, screws
    g.setColour (juce::Colours::black.withAlpha (0.55f));
    g.fillRoundedRectangle (shell.translated (0.0f, 4.0f).expanded (1.0f), 10.0f);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff2d323d), shell.getCentreX(), shell.getY(),
                                             juce::Colour (0xff1b1e25), shell.getCentreX(), shell.getBottom(), false));
    g.fillRoundedRectangle (shell, 9.0f);
    g.setColour (juce::Colours::white.withAlpha (0.08f));
    g.drawRoundedRectangle (shell.reduced (1.5f), 8.0f, 1.0f);
    g.setColour (juce::Colours::black.withAlpha (0.85f));
    g.drawRoundedRectangle (shell, 9.0f, 1.2f);

    for (auto corner : { shell.getTopLeft(), shell.getTopRight(), shell.getBottomLeft(), shell.getBottomRight() })
        drawScrew (g, { corner.x + (corner.x == shell.getX() ? 8.0f : -8.0f), corner.y + (corner.y == shell.getY() ? 8.0f : -8.0f) }, 3.0f);

    // Label: a paper panel lighter than the plastic, with an edge, a muted type stripe,
    // the product name, two write-on lines, the window cut-out and the type strip
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillRoundedRectangle (label.translated (0.0f, 1.0f), 4.0f);
    g.setGradientFill (juce::ColourGradient (panelLight.brighter (0.16f), label.getX(), label.getY(),
                                             panelLight.brighter (0.08f), label.getX(), label.getBottom(), false));
    g.fillRoundedRectangle (label, 4.0f);

    const auto stripe = accent.withSaturation (accent.getSaturation() * 0.6f);
    g.setGradientFill (juce::ColourGradient (stripe, label.getX(), label.getY(), stripe.darker (0.35f), label.getRight(), label.getY(), false));
    g.fillRoundedRectangle (label.withHeight (16.0f), 4.0f);
    g.fillRect (label.getX(), label.getY() + 10.0f, label.getWidth(), 6.0f);
    g.setColour (vuCream.withAlpha (0.85f));
    g.fillRect (label.getX(), label.getY() + 16.0f, label.getWidth(), 1.0f);
    g.setColour (juce::Colours::white.withAlpha (0.08f));
    g.drawRoundedRectangle (label.reduced (0.5f), 4.0f, 1.0f);

    g.setColour (text);
    g.setFont (font (12.5f, true).withExtraKerningFactor (0.12f));
    g.drawText ("NIGHTSHIFT", label.withTrimmedTop (19.0f).withHeight (16.0f).withTrimmedLeft (10.0f), juce::Justification::centredLeft, true);
    g.setColour (textDim.brighter (0.2f));
    g.setFont (monoFont (12.0f, true));
    g.drawText ("C-90", label.withTrimmedTop (20.0f).withHeight (16.0f).withTrimmedRight (10.0f), juce::Justification::centredRight, true);

    g.setColour (juce::Colours::white.withAlpha (0.07f));

    for (float y : { label.getY() + 40.5f, label.getY() + 47.5f })
        g.fillRect (label.getX() + 10.0f, y, label.getWidth() - 20.0f, 1.0f);

    // The window is cut out of the label; the packs and hubs are drawn into it every frame
    g.setColour (juce::Colour (0xff07080b));
    g.fillRoundedRectangle (window.expanded (2.0f), 5.0f);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff111318), window.getCentreX(), window.getY(),
                                             juce::Colour (0xff060708), window.getCentreX(), window.getBottom(), false));
    g.fillRoundedRectangle (window, 4.0f);

    const auto dot = juce::String::fromUTF8 ("   \xc2\xb7   ");   // middle dot
    const juce::String typeText[] = { "TYPE I" + dot + "FERRIC", "TYPE II" + dot + "CHROME", "TYPE IV" + dot + "METAL" };
    auto strip = juce::Rectangle<float> (label.getX() + 8.0f, window.getBottom() + 6.0f, label.getWidth() - 16.0f, 16.0f);
    g.setGradientFill (juce::ColourGradient (vuCream, strip.getX(), strip.getY(), vuCream.darker (0.18f), strip.getX(), strip.getBottom(), false));
    g.fillRoundedRectangle (strip, 3.0f);
    g.setColour (accent.darker (0.2f));
    g.fillRoundedRectangle (strip.getX() + 3.0f, strip.getY() + 3.0f, 11.0f, 11.0f, 2.0f);
    g.setColour (juce::Colours::white);
    g.setFont (font (12.0f, true));
    g.drawText ("A", juce::Rectangle<float> (strip.getX() + 3.0f, strip.getY() + 3.0f, 11.0f, 11.0f), juce::Justification::centred, true);
    g.setColour (juce::Colour (0xff2a241b));
    g.setFont (font (11.5f, true).withExtraKerningFactor (0.04f));
    g.drawText (typeText[juce::jlimit (0, 2, type)], strip.withTrimmedLeft (18.0f), juce::Justification::centred, true);

    // Lower shell: the raised trapezoid over the tape path, with the capstan holes, the
    // pinch-roller recesses and the head opening where the felt pressure pad sits
    const float cx = shell.getCentreX(), bottom = shell.getBottom();
    const float trapTop = bottom - 36.0f, trapBase = bottom - 9.0f;
    juce::Path trapezoid;
    trapezoid.startNewSubPath (cx - 84.0f, trapBase);
    trapezoid.lineTo (cx - 70.0f, trapTop);
    trapezoid.lineTo (cx + 70.0f, trapTop);
    trapezoid.lineTo (cx + 84.0f, trapBase);
    trapezoid.closeSubPath();
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff272b34), cx, trapTop, juce::Colour (0xff1d2028), cx, trapBase, false));
    g.fillPath (trapezoid);
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.strokePath (trapezoid, juce::PathStrokeType (1.0f));
    g.setColour (juce::Colours::white.withAlpha (0.09f));
    g.drawLine (cx - 69.0f, trapTop + 1.0f, cx + 69.0f, trapTop + 1.0f, 1.0f);

    for (float side : { -1.0f, 1.0f })
    {
        // Capstan hole with its metal ring
        const float hx = cx + side * 48.0f;
        auto hole = juce::Rectangle<float> (12.0f, 12.0f).withCentre ({ hx, trapTop + 12.0f });
        g.setColour (juce::Colour (0xff07080b));
        g.fillEllipse (hole);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff8d94a3), hx, hole.getY(), juce::Colour (0xff3a3f4b), hx, hole.getBottom(), false));
        g.fillEllipse (hole.reduced (3.5f));

        // Pinch-roller recess under it, and a small guide hole nearer the centre
        g.setColour (juce::Colour (0xff07080b));
        g.fillRoundedRectangle (hx - 7.0f, trapBase - 8.0f, 14.0f, 9.0f, 1.5f);
        g.fillRoundedRectangle (cx + side * 26.0f - 3.0f, trapTop + 9.0f, 6.0f, 6.0f, 1.0f);
    }

    // Head opening: the felt pad on its bronze leaf spring, behind the tape
    g.setColour (juce::Colour (0xff07080b));
    g.fillRoundedRectangle (cx - 16.0f, trapBase - 12.0f, 32.0f, 13.0f, 2.0f);
    g.setColour (juce::Colour (0xffa07a45));
    g.fillRect (cx - 12.0f, trapBase - 9.0f, 24.0f, 1.2f);
    g.setGradientFill (juce::ColourGradient (vuCream, cx, trapBase - 6.0f, vuCream.darker (0.3f), cx, trapBase, false));
    g.fillRoundedRectangle (cx - 7.0f, trapBase - 6.0f, 14.0f, 5.0f, 1.0f);

    // Deck strip: lamp housings and the counter window
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1b1f28), deckStrip.getCentreX(), deckStrip.getY(),
                                             juce::Colour (0xff141720), deckStrip.getCentreX(), deckStrip.getBottom(), false));
    g.fillRoundedRectangle (deckStrip, 6.0f);
    g.setColour (outline);
    g.drawRoundedRectangle (deckStrip, 6.0f, 1.0f);
    g.setColour (juce::Colours::white.withAlpha (0.05f));
    g.drawRoundedRectangle (deckStrip.reduced (1.0f), 5.0f, 1.0f);

    g.setFont (font (11.5f, true).withExtraKerningFactor (0.04f));
    const float lampY = deckStrip.getY() + 22.0f;

    for (auto [lx, name] : { std::pair<float, const char*> { deckStrip.getX() + 34.0f, "PLAY" },
                             std::pair<float, const char*> { deckStrip.getX() + 86.0f, "DROPOUT" } })
    {
        auto housing = juce::Rectangle<float> (14.0f, 14.0f).withCentre ({ lx, lampY });
        g.setColour (juce::Colour (0xff07080b));
        g.fillEllipse (housing);
        g.setColour (textDim);
        g.drawText (name, juce::Rectangle<float> (lx - 28.0f, lampY + 11.0f, 56.0f, 12.0f), juce::Justification::centred, true);
    }

    g.setColour (juce::Colour (0xff07080b));
    g.fillRoundedRectangle (counterBox.expanded (2.0f), 4.0f);
    g.setColour (juce::Colour (0xff101216));
    g.fillRoundedRectangle (counterBox, 3.0f);
    g.setColour (textDim);
    g.drawText ("TAPE COUNTER", counterBox.withY (counterBox.getBottom() + 3.0f).withHeight (12.0f), juce::Justification::centred, true);

    shellImageType = type;
    shellImageScale = scale;
}

namespace
{
    /** Where a strand of tape from `from` touches the pack (centre c, radius r) on its
        outer side (side -1: the left edge, +1: the right edge). */
    juce::Point<float> tangentPoint (juce::Point<float> c, float r, juce::Point<float> from, float side)
    {
        const auto d = from - c;
        const float dist = std::max (r + 0.01f, d.getDistanceFromOrigin());
        const float base = std::atan2 (d.y, d.x), off = std::acos (r / dist);
        const juce::Point<float> t1 = c + juce::Point<float> (std::cos (base + off), std::sin (base + off)) * r;
        const juce::Point<float> t2 = c + juce::Point<float> (std::cos (base - off), std::sin (base - off)) * r;
        return (side < 0.0f ? t1.x < t2.x : t1.x > t2.x) ? t1 : t2;
    }
}

void CassetteEditor::drawMechanism (juce::Graphics& g) const
{
    using namespace theme;
    const float rL = packRadius (tapePosition), rR = packRadius (1.0f - tapePosition);
    const auto tape = juce::Colour (0xff6b4e33);

    // The tape path: from each pack's outer edge down to the corner rollers (showing
    // through the smoked plastic below the label), then along the bottom edge past the
    // pressure pad
    {
        g.saveState();
        g.excludeClipRegion (label.toNearestInt());
        g.setColour (tape.withAlpha (0.85f));

        for (auto [centre, radius, roller, side] : { std::tuple<juce::Point<float>, float, juce::Point<float>, float> { reelL, rL, rollerL, -1.0f },
                                                     std::tuple<juce::Point<float>, float, juce::Point<float>, float> { reelR, rR, rollerR, 1.0f } })
        {
            const auto start = roller + juce::Point<float> (side * 4.5f, 0.0f);
            g.drawLine ({ tangentPoint (centre, radius, start, side), start }, 1.8f);
        }

        g.restoreState();

        for (auto roller : { rollerL, rollerR })
        {
            auto r = juce::Rectangle<float> (9.0f, 9.0f).withCentre (roller);
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xffe9e3d6), roller.x, r.getY(), juce::Colour (0xffa9a293), roller.x, r.getBottom(), false));
            g.fillEllipse (r);
            g.setColour (juce::Colours::black.withAlpha (0.5f));
            g.drawEllipse (r, 0.8f);
            g.fillEllipse (juce::Rectangle<float> (2.5f, 2.5f).withCentre (roller));
        }

        g.setColour (tape);
        g.fillRect (rollerL.x, tapeY - 1.5f, rollerR.x - rollerL.x, 3.0f);
        g.setColour (juce::Colours::white.withAlpha (0.15f));
        g.fillRect (rollerL.x, tapeY - 1.5f, rollerR.x - rollerL.x, 1.0f);
    }

    // Through the window: the tape packs and the hubs
    g.saveState();
    g.reduceClipRegion (window.toNearestInt());

    auto pack = [&] (juce::Point<float> centre, float radius)
    {
        auto disc = juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff2b2018), centre.x, centre.y, juce::Colour (0xff43321f), centre.x + radius, centre.y, true));
        g.fillEllipse (disc);
        g.setColour (juce::Colours::white.withAlpha (0.10f));
        g.drawEllipse (disc, 1.2f);

        for (float r = radius - 5.0f; r > 24.0f; r -= 5.0f)
        {
            g.setColour (juce::Colours::black.withAlpha (0.12f));
            g.drawEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (centre), 0.8f);
        }
    };

    pack (reelL, rL);
    pack (reelR, rR);

    auto hub = [&] (juce::Point<float> centre, float angle)
    {
        // A cream hub ring with six small drive teeth pointing into the spindle hole
        auto ring = juce::Rectangle<float> (40.0f, 40.0f).withCentre (centre);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xfff0ebe0), centre.x, ring.getY(), juce::Colour (0xffc9c2b4), centre.x, ring.getBottom(), false));
        g.fillEllipse (ring);
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.drawEllipse (ring, 1.0f);
        g.setColour (juce::Colours::black.withAlpha (0.15f));
        g.drawEllipse (ring.reduced (5.0f), 1.0f);
        g.setColour (juce::Colour (0xff15171c));
        g.fillEllipse (juce::Rectangle<float> (22.0f, 22.0f).withCentre (centre));

        for (int i = 0; i < 6; ++i)
        {
            const float a = angle + (float) i * juce::MathConstants<float>::twoPi / 6.0f;
            juce::Path tooth;
            tooth.addRoundedRectangle (-1.8f, -11.5f, 3.6f, 5.5f, 1.2f);
            g.setColour (juce::Colour (0xffe6e0d3));
            g.fillPath (tooth, juce::AffineTransform::rotation (a).translated (centre));
        }

        g.setColour (juce::Colour (0xff0a0b0e));
        g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (centre));
    };

    hub (reelL, reelAngleL);
    hub (reelR, reelAngleR);

    // Glass over the window: a diagonal fade, so it reads as a reflection, not a stripe
    g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.10f), window.getX(), window.getY(),
                                             juce::Colours::white.withAlpha (0.0f), window.getX() + window.getWidth() * 0.4f, window.getBottom(), false));
    g.fillRoundedRectangle (window, 4.0f);
    g.restoreState();

    g.setColour (juce::Colours::white.withAlpha (0.10f));
    g.drawRoundedRectangle (window, 4.0f, 1.0f);

    // Lamps: PLAY while the transport runs, DROPOUT in the Noise section's rose
    auto lamp = [&] (juce::Point<float> centre, juce::Colour colour, float intensity)
    {
        auto dot = juce::Rectangle<float> (9.0f, 9.0f).withCentre (centre);
        g.setColour (colour.withAlpha (0.12f + 0.88f * intensity));
        g.fillEllipse (dot);

        if (intensity > 0.05f)
        {
            g.setColour (colour.withAlpha (0.35f * intensity));
            g.fillEllipse (dot.expanded (3.5f));
        }
    };

    const float lampY = deckStrip.getY() + 22.0f;
    lamp ({ deckStrip.getX() + 34.0f, lampY }, ledOn, juce::jlimit (0.0f, 1.0f, running));
    lamp ({ deckStrip.getX() + 86.0f, lampY }, rose, juce::jlimit (0.0f, 1.0f, dropoutLamp * 2.5f));

    // Counter: three mechanical wheels
    const int value = ((int) counter) % 1000;
    g.setFont (monoFont (21.0f, true));

    for (int d = 0; d < 3; ++d)
    {
        auto cell = juce::Rectangle<float> (counterBox.getX() + 7.0f + d * 31.0f, counterBox.getY() + 3.0f, 29.0f, 26.0f);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1b1e25), cell.getCentreX(), cell.getY(), juce::Colour (0xff0c0d11), cell.getCentreX(), cell.getBottom(), false));
        g.fillRoundedRectangle (cell, 2.0f);
        g.setColour (vuCream);
        const int digit = (value / (d == 0 ? 100 : d == 1 ? 10 : 1)) % 10;
        g.drawText (juce::String (digit), cell, juce::Justification::centred, true);
    }
}

void CassetteEditor::drawSpeedMeter (juce::Graphics& g) const
{
    using namespace theme;
    auto r = meterArea;     // a copy: this is called on every paint

    g.setColour (textDim.brighter (0.15f));
    g.setFont (font (11.5f, true).withExtraKerningFactor (0.06f));
    g.drawText ("W & F", r.removeFromTop (16), juce::Justification::centred, true);

    const auto readout = r.removeFromBottom (16);
    auto bezel = r.toFloat().reduced (2.0f, 1.0f);
    g.setColour (juce::Colour (0xff0b0d11));
    g.fillRoundedRectangle (bezel, 6.0f);
    auto face = bezel.reduced (4.0f);
    g.setGradientFill (juce::ColourGradient (vuCream, face.getX(), face.getY(), vuCream.darker (0.25f), face.getX(), face.getBottom(), false));
    g.fillRoundedRectangle (face, 4.0f);

    // A panel meter with its pivot showing at the foot of the face. The scale is
    // square-root, like a VU's, so it reads small wobble finely and still reaches the
    // +-30 cents that wow 100 plus flutter can produce
    const auto pivot = juce::Point<float> (face.getCentreX(), face.getBottom() - 7.0f);
    const float radius = face.getHeight() - 19.0f, span = 0.62f;
    const auto ink = juce::Colour (0xff3a3328);
    auto position = [] (float cents) { return juce::jlimit (-1.0f, 1.0f, std::copysign (std::sqrt (std::abs (cents) / 30.0f), cents)); };
    auto dirAt = [&] (float cents) { const float a = position (cents) * span; return juce::Point<float> (std::sin (a), -std::cos (a)); };

    g.saveState();
    g.reduceClipRegion (face.toNearestInt());

    juce::Path scale;
    scale.addCentredArc (pivot.x, pivot.y, radius, radius, 0.0f, -span, span, true);
    g.setColour (ink);
    g.strokePath (scale, juce::PathStrokeType (1.2f));

    for (float cents : { -25.0f, -20.0f, -15.0f, -10.0f, -5.0f, -2.0f, 0.0f, 2.0f, 5.0f, 10.0f, 15.0f, 20.0f, 25.0f })
    {
        const bool major = cents == 0.0f || std::abs (cents) == 5.0f || std::abs (cents) == 25.0f;
        const auto dir = dirAt (cents);
        g.drawLine ({ pivot + dir * (radius - (major ? 5.0f : 3.0f)), pivot + dir * radius }, major ? 1.3f : 1.0f);
    }

    g.setFont (font (12.0f, true));

    for (float cents : { -25.0f, 0.0f, 25.0f })
        g.drawText (juce::String (std::abs ((int) cents)), juce::Rectangle<float> (24.0f, 15.0f).withCentre (pivot + dirAt (cents) * (radius - 13.0f)),
                    juce::Justification::centred, true);

    g.setColour (ink);
    g.setFont (font (11.5f, true).withExtraKerningFactor (0.06f));
    g.drawText ("CENTS", juce::Rectangle<float> (face.getWidth(), 14.0f).withCentre ({ pivot.x, pivot.y - 18.0f }), juce::Justification::centred, true);

    const auto needleDir = dirAt (centsNeedle);
    g.setColour (juce::Colour (0xff1b1712));
    g.drawLine ({ pivot, pivot + needleDir * (radius + 2.0f) }, 1.4f);
    g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (pivot));
    g.restoreState();

    // Glass glint and bezel edge
    g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.10f), face.getX(), face.getY(),
                                             juce::Colours::white.withAlpha (0.0f), face.getX(), face.getCentreY(), false));
    g.fillRoundedRectangle (face, 4.0f);
    g.setColour (outline);
    g.drawRoundedRectangle (bezel.reduced (0.5f), 6.0f, 1.0f);

    g.setColour (text);
    g.setFont (monoFont (12.0f));
    g.drawText (juce::String::fromUTF8 ("\xc2\xb1") + juce::String (centsPeak, 1) + juce::String::fromUTF8 (" \xc2\xa2"),
                readout, juce::Justification::centred, true);
}

void CassetteEditor::paintBody (juce::Graphics& g, juce::Rectangle<int>)
{
    const int type = juce::jlimit (0, 2, juce::roundToInt (cassette.param ("type")));
    const float scale = juce::jlimit (1.0f, 4.0f, g.getInternalContext().getPhysicalPixelScaleFactor());

    if (shellImage.isNull() || shellImageType != type || shellImageScale != scale)
        renderShell (type, scale);

    g.drawImage (shellImage, graphic.toFloat());

    g.saveState();
    g.addTransform (juce::AffineTransform::translation ((float) graphic.getX(), (float) graphic.getY()));
    drawMechanism (g);
    g.restoreState();

    drawSectionTitle (g, tapeArea.withHeight (14), "Tape", theme::amber);
    drawSectionTitle (g, noiseArea.withHeight (14), "Noise", theme::rose);
    drawSectionTitle (g, motionArea.withHeight (14), "Motion", theme::teal);
    drawSectionTitle (g, outArea.withHeight (14), "Out", theme::violet);

    // A hairline between the left and right columns of sections
    g.setColour (juce::Colours::white.withAlpha (0.06f));
    g.fillRect (noiseArea.getX() - 9, tapeArea.getY() + 22, 1, motionArea.getBottom() - tapeArea.getY() - 30);

    drawSpeedMeter (g);
}

void CassetteEditor::animate (float seconds)
{
    // The motor runs while the tape is engaged and something is playing: the host's
    // transport if it reports one, otherwise signal at the input (held 1.5 s so the
    // reels don't stop in every gap)
    const bool engaged = cassette.param ("bypass") < 0.5f;
    const bool signal = inputLevel > 1.0e-4f || cassette.getHostPlaying() == 1;
    signalHold = signal ? 1.5f : std::max (0.0f, signalHold - seconds);
    const float runTarget = engaged && signalHold > 0.0f ? 1.0f : 0.0f;
    running += (runTarget - running) * std::min (1.0f, seconds * 3.0f);

    wobbleSmoothed += (cassette.getTransportWobble() - wobbleSmoothed) * 0.5f;
    spoolSpeed += ((float) transportKeys->getSpool() - spoolSpeed) * std::min (1.0f, seconds * 4.0f);

    // Tape speed past the heads in px/s (about real cassette speed at this scale);
    // positive runs left to right, the left pack paying out and both reels turning
    // anticlockwise. FF spools onward in the play direction, REW back.
    constexpr float tapeLength = 18000.0f;     // one side at play speed: about 200 s
    float velocity = playDirection * (running * 90.0f * wobbleSmoothed + spoolSpeed * 600.0f);
    float next = tapePosition - velocity * seconds / tapeLength;

    if (next < 0.03f || next > 0.97f)
    {
        next = juce::jlimit (0.03f, 0.97f, next);
        velocity = seconds > 0.0f ? (tapePosition - next) * tapeLength / seconds : 0.0f;

        // Auto-reverse at the end of a side while playing
        if (std::abs (spoolSpeed) < 0.05f && running > 0.5f)
            playDirection = -playDirection;
    }

    reelAngleL -= seconds * velocity / packRadius (tapePosition);
    reelAngleR -= seconds * velocity / packRadius (1.0f - tapePosition);
    reelAngleL = std::fmod (reelAngleL, juce::MathConstants<float>::twoPi);
    reelAngleR = std::fmod (reelAngleR, juce::MathConstants<float>::twoPi);
    tapePosition = next;

    counter = std::fmod (counter + seconds * velocity * 0.03f + 1000.0f, 1000.0f);

    const float drop = engaged ? cassette.getDropoutActivity() : 0.0f;
    dropoutLamp = std::max (drop, dropoutLamp * 0.82f);

    const float cents = engaged ? cassette.getSpeedCents() : 0.0f;
    centsNeedle += (cents - centsNeedle) * 0.45f;
    centsPeak = std::max (std::abs (cents), centsPeak * 0.985f);

    // The keys below repaint themselves when they change
    repaint (graphic.withHeight ((int) keys.getY()));
    repaint (meterArea);
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CassetteProcessor();
}
