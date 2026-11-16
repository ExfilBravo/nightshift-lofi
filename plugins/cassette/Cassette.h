#pragma once

#include <JuceHeader.h>
#include "common/LofiDsp.h"
#include "common/LofiEditor.h"
#include "common/LofiParams.h"
#include "common/LofiProcessor.h"
#include "CassetteDsp.h"

/** Nightshift Cassette: tape saturation, head bump, age, wow, flutter, hiss, dropouts.
    See SPEC.md for the design and the measurements it must meet.

    Chain: drive -> pre-emphasis (type) -> 4x oversampled tape curve with bias ->
    loudness compensation -> de-emphasis + type tilt + head bump + age (transient-aware
    lowpass) -> wow & flutter (speed error integrated into a wandering delay, L/R
    decorrelated by width) -> dropouts (dip + HF loss) -> + hiss.
*/
class CassetteProcessor : public lofi::LofiProcessor
{
public:
    CassetteProcessor();
    juce::AudioProcessorEditor* createEditor() override;

    /** Reel speed modulation for the editor's animation, around 1.0 (exaggerated so
        it can be seen: a 5 cent wobble reads as about 10 % on the reels). */
    float getTransportWobble() const    { return transportWobble.load(); }

    /** The transport's current speed error in cents (left channel, block average). */
    float getSpeedCents() const         { return speedCents.load(); }

    /** 0..1: how deep the dropout in progress is, for the editor's lamp. */
    float getDropoutActivity() const    { return dropoutActivity.load(); }

    /** 1 while the host transport plays, 0 while it is stopped, -1 if the host
        doesn't say (no play head): the editor's reels run on it or on input. */
    int getHostPlaying() const          { return hostPlaying.load(); }

protected:
    void prepareDsp (const juce::dsp::ProcessSpec&) override;
    void processDsp (juce::AudioBuffer<float>&) override;
    void resetDsp() override;
    int getDspLatency() const override  { return latency; }

private:
    static Layout createLayout();

    struct TypeProfile
    {
        float preEmphDb;    // HF boost into the saturator (and cut after), so highs compress like tape
        float tiltDb;       // fixed high-shelf character at 7 kHz
        float blend;        // 0 = soft knee (ferric) .. 1 = hard knee (metal)
        float biasScale;    // even-harmonic bias relative to type II
        float hissLpHz;     // where the hiss rolls off
        float hissDb;       // hiss level relative to the knob
    };

    static const TypeProfile profiles[3];

    struct Channel
    {
        cassette::Svf preEmph, deEmph, tilt, bumpShelf, bumpBell, age;
        lofi::dsp::DcBlocker dc;
        lofi::dsp::ModDelay delay;
        cassette::OnePoleLp dropLp;
        cassette::Hiss hiss;
        cassette::Wobble wobble;
        double pos = 0.0;
        float envFast = 0.0f, envSlow = 0.0f, transient = 0.0f, ageFc = -1.0f;
    };

    void updateTargets();
    void snapToTargets();
    void updateCoefficients (int numCh);
    void processChunk (float* const* data, int numCh, int len, double& wobbleSum, double& centsSum, float& dropPeak);

    static constexpr int chunkSize = 256;
    static constexpr float saturatorScale = 1.25f;   // tape sensitivity: -12 dBFS at drive 0 sits well inside the knee
    static constexpr float referenceAmp = 0.251f;    // -12 dBFS, the level the drive compensation holds
    static constexpr double baseDelaySeconds = 0.0075; // the centre the wow & flutter delay wanders around

    int latency = 0;
    std::atomic<float> transportWobble { 1.0f }, speedCents { 0.0f }, dropoutActivity { 0.0f };
    std::atomic<int> hostPlaying { -1 };
    int chunkStart = 0;     // where the chunk being processed starts in the block

    std::atomic<float>* pType = nullptr; std::atomic<float>* pDrive = nullptr; std::atomic<float>* pAge = nullptr;
    std::atomic<float>* pBump = nullptr; std::atomic<float>* pWow = nullptr; std::atomic<float>* pFlutter = nullptr;
    std::atomic<float>* pWidth = nullptr; std::atomic<float>* pHiss = nullptr; std::atomic<float>* pDropouts = nullptr;

    double sampleRate = 48000.0, leak = 0.0, posSoft = 0.0, posLimit = 0.0, posRestore = 0.002;
    int numChannels = 2, baseDelay = 0, coeffPhase = 0;
    float envAttack = 0.1f, envRelease = 0.01f, envSlowCoef = 0.001f, transientCoef = 0.01f;
    float hissCalib[3] { 1.0f, 1.0f, 1.0f };
    float widthCos = 1.0f, widthSin = 0.0f;

    /** After prepare or a reset there is no audio history, so the first block jumps the
        smoothers to the current settings (a host may set parameters after
        prepareToPlay) instead of ramping from stale values. */
    bool snapPending = true;

    /** After a reset the wet path holds silence for `latency` samples and then the live
        signal arrives all at once, while the base class is already part-way through
        fading the wet path in. This holds the wet output at zero until then and
        raises it over a short raised cosine, so un-bypass cannot step. */
    int resumeHold = 0, resumePos = 0, resumeLength = 1;
    float lastPre = -1000.0f, lastTilt = -1000.0f, lastBump = -1000.0f, lastHissHz = -1.0f;
    float lastDriveForPost = -1.0f, postTarget = 1.0f;
    int lastTypeForPost = -1;

    Channel ch[2];
    cassette::Dropouts dropouts;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampling;
    float biasBuf[chunkSize] {}, blendBuf[chunkSize] {}, offsetBuf[chunkSize] {};

    /** Gains ramp multiplicatively (linear in dB): the drive gain and its loudness
        compensation then move as a pair with no bump in their product, and the hiss
        fades evenly across its 60 dB range. */
    using GainSmoothed = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative>;

    GainSmoothed driveGain, postGain, hissGain;
    lofi::dsp::Smoothed bias, blend, preEmphDb, tiltDb, bumpDb, ageFc, ageAmount,
                        wowCents, flutterCents, widthAngle, hissTone;
};

//==============================================================================
/** A three-way hardware tape selector (I / II / IV) bound to the type parameter.
    Click a position or slide the cap. */
class TypeSelector : public juce::Component,
                     public juce::SettableTooltipClient
{
public:
    TypeSelector (juce::AudioProcessorValueTreeState&, const juce::String& paramId);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent& e) override    { mouseDown (e); }

    static juce::Colour accentFor (int type);

private:
    int positionAt (int y) const;

    juce::RangedAudioParameter& parameter;
    juce::ParameterAttachment attachment;
    int current = 1;
};

//==============================================================================
/** The deck's transport keys. PLAY latches: lit, the tape is engaged; click it again
    to bypass (it drives the bypass parameter, like the header switch). REW and FF
    spool the reels while held; they are part of the animation and leave the audio
    alone, as their tooltips say. */
class TransportKeys : public juce::Component,
                      public juce::SettableTooltipClient
{
public:
    explicit TransportKeys (juce::AudioProcessorValueTreeState&);

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    /** -1 while REW is held, +1 while FF is held, else 0. */
    int getSpool() const        { return spool; }

private:
    juce::Rectangle<float> keyBounds (int index) const;
    int keyAt (juce::Point<int>) const;

    juce::RangedAudioParameter& bypass;
    juce::ParameterAttachment attachment;
    bool bypassed = false;
    int spool = 0, hover = -1, pressed = -1;
};

//==============================================================================
class CassetteEditor : public lofi::LofiEditor
{
public:
    explicit CassetteEditor (CassetteProcessor&);

protected:
    void layoutBody (juce::Rectangle<int>) override;
    void paintBody (juce::Graphics&, juce::Rectangle<int>) override;
    void animate (float seconds) override;

private:
    void renderShell (int type, float scale);
    void drawMechanism (juce::Graphics&) const;
    void drawSpeedMeter (juce::Graphics&) const;
    static float packRadius (float position);

    CassetteProcessor& cassette;
    juce::Rectangle<int> graphic, tapeArea, noiseArea, motionArea, outArea, meterArea;
    std::vector<lofi::Knob*> tapeKnobs, motionKnobs, noiseKnobs, outKnobs;
    std::unique_ptr<TypeSelector> typeSelector;
    std::unique_ptr<TransportKeys> transportKeys;

    // Geometry of the cassette inside the graphic, in graphic-local pixels
    juce::Rectangle<float> shell, label, window, deckStrip, counterBox, keys;
    juce::Point<float> reelL, reelR, rollerL, rollerR;
    float tapeY = 0.0f;

    // The static art, rendered at the display's physical pixel scale and cached per type
    juce::Image shellImage;
    int shellImageType = -1;
    float shellImageScale = 0.0f;

    // Transport animation. tapePosition is the share of the tape on the left pack;
    // playDirection +1 runs the tape left to right past the heads (both reels turn
    // anticlockwise and the left pack pays out), -1 after an auto-reverse.
    float reelAngleL = 0.0f, reelAngleR = 1.1f, tapePosition = 0.62f, playDirection = 1.0f;
    float running = 0.0f, signalHold = 0.0f, spoolSpeed = 0.0f;
    float counter = 47.0f, wobbleSmoothed = 1.0f, dropoutLamp = 0.0f, centsNeedle = 0.0f, centsPeak = 0.0f;
};
