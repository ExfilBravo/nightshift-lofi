#include <JuceHeader.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#include "common/LofiTheme.h"
#include "common/TextAudit.h"

/** The standalone build of each Nightshift Lofi plugin. With no arguments it is the usual
    JUCE standalone: a window with the editor and an audio device. With

        <Plugin>.exe --snapshot <out.png> [--set Name=value]...

    it builds the editor offscreen, runs it for a moment so meters and animations settle,
    writes a PNG and quits, so the GUI can be checked without a window opening.
*/
extern juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

namespace
{

/** Runs the processor and editor from the message-thread timer for 45 frames, then captures. */
class SnapshotRunner : private juce::Timer
{
public:
    explicit SnapshotRunner (const juce::StringArray& tokens)
        : outFile (after (tokens, "--snapshot")), textAudit (tokens.contains ("--textaudit")), allText (tokens.contains ("--alltext"))
    {
        processor.reset (createPluginFilter());
        processor->setPlayConfigDetails (2, 2, sampleRate, blockSize);
        processor->prepareToPlay (sampleRate, blockSize);

        for (int i = 0; i + 1 < tokens.size(); ++i)
        {
            if (tokens[i] != "--set")
                continue;

            const auto assignment = tokens[i + 1].unquoted();
            const auto name = assignment.upToFirstOccurrenceOf ("=", false, false).trim();
            const auto text = assignment.fromFirstOccurrenceOf ("=", false, false).trim();
            bool found = false;

            for (auto* p : processor->getParameters())
            {
                auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (p);

                if (p->getName (64).equalsIgnoreCase (name) || (withId != nullptr && withId->paramID.equalsIgnoreCase (name)))
                {
                    p->setValueNotifyingHost (p->getValueForText (text));
                    found = true;
                }
            }

            if (! found)
            {
                std::cout << "no parameter called '" << name << "'" << std::endl;
                finish (1);
                return;
            }
        }

        editor.reset (processor->createEditorIfNeeded());

        if (editor == nullptr)
        {
            std::cout << "no editor" << std::endl;
            finish (1);
            return;
        }

        block.setSize (2, blockSize);
        startTimerHz (30);
    }

    ~SnapshotRunner() override
    {
        stopTimer();
        editor = nullptr;

        if (processor != nullptr)
            processor->releaseResources();
    }

private:
    static juce::String after (const juce::StringArray& tokens, const juce::String& flag)
    {
        const int i = tokens.indexOf (flag);
        return i >= 0 && i + 1 < tokens.size() ? tokens[i + 1].unquoted() : juce::String();
    }

    void timerCallback() override
    {
        // Keep a -12 dBFS tone flowing so the meters show a working unit
        for (int rep = 0; rep < 3; ++rep)
        {
            for (int i = 0; i < blockSize; ++i)
            {
                const float v = 0.25f * (float) std::sin (phase);
                phase += juce::MathConstants<double>::twoPi * 220.0 / sampleRate;
                block.setSample (0, i, v);
                block.setSample (1, i, v);
            }

            processor->processBlock (block, midi);
        }

        if (++frames < 45)
            return;

        stopTimer();
        // --textaudit: the same picture, painted by the text check, which lists every piece of text that's under
        // 15 px (the small capitals: 14), squeezed, cut short, clipped or faint (--alltext: every piece)
        juce::Image image;

        if (textAudit)
            std::cout << nightshift::textaudit::report (nightshift::textaudit::collect (*editor, &image), allText);
        else
            image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);

        outFile.deleteFile();
        juce::FileOutputStream stream (outFile);
        const bool ok = stream.openedOk() && juce::PNGImageFormat().writeImageToStream (image, stream);
        std::cout << (ok ? "snapshot " : "snapshot failed ") << outFile.getFullPathName()
                  << "  (" << image.getWidth() << "x" << image.getHeight() << ")" << std::endl;
        finish (ok ? 0 : 1);
    }

    static void finish (int code)
    {
        if (auto* app = juce::JUCEApplication::getInstance())
        {
            app->setApplicationReturnValue (code);
            app->quit();
        }
    }

    static constexpr double sampleRate = 48000.0;
    static constexpr int blockSize = 512;

    juce::File outFile;
    bool textAudit = false, allText = false;
    std::unique_ptr<juce::AudioProcessor> processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    juce::AudioBuffer<float> block;
    juce::MidiBuffer midi;
    double phase = 0.0;
    int frames = 0;
};

} // namespace

//==============================================================================
class LofiStandaloneApp : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override       { return JucePlugin_Name; }
    const juce::String getApplicationVersion() override    { return JucePlugin_VersionString; }
    bool moreThanOneInstanceAllowed() override             { return true; }

    void initialise (const juce::String& commandLine) override
    {
        const auto tokens = juce::StringArray::fromTokens (commandLine, true);

        if (tokens.contains ("--snapshot"))
        {
            snapshot = std::make_unique<SnapshotRunner> (tokens);
            return;
        }

        juce::PropertiesFile::Options options;
        options.applicationName = getApplicationName();
        options.filenameSuffix = ".settings";
        options.osxLibrarySubFolder = "Application Support";
        options.folderName = "Nightshift Lofi";
        appProperties.setStorageParameters (options);

        window = std::make_unique<juce::StandaloneFilterWindow> (getApplicationName(), lofi::theme::background,
                                                                 appProperties.getUserSettings(), false);
        window->setVisible (true);
    }

    void shutdown() override
    {
        snapshot = nullptr;
        window = nullptr;
        appProperties.saveIfNeeded();
    }

    void systemRequestedQuit() override
    {
        if (window != nullptr)
            window->pluginHolder->savePluginState();

        quit();
    }

private:
    juce::ApplicationProperties appProperties;
    std::unique_ptr<juce::StandaloneFilterWindow> window;
    std::unique_ptr<SnapshotRunner> snapshot;
};

START_JUCE_APPLICATION (LofiStandaloneApp)
