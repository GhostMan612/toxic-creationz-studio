#include "MainComponent.h"

#include <iostream>

namespace
{
int runHeadlessSmokeTest()
{
    std::cout << "[SMOKE_TEST] Initializing Tracktion Engine..." << std::endl;
    te::Engine engine { "AI DAW SmokeTest" };

    const auto tempFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getChildFile ("aidaw_ci_smoke.tracktionedit");
    tempFile.deleteFile();

    {
        auto edit = te::createEmptyEdit (engine, tempFile);

        if (edit == nullptr)
        {
            std::cerr << "[SMOKE_TEST] FAIL: createEmptyEdit returned nullptr" << std::endl;
            return 1;
        }

        edit->ensureNumberOfAudioTracks (4);
        edit->tempoSequence.getTempo (0)->setBpm (128.0);

        auto tracks = te::getAudioTracks (*edit);

        if (tracks.size() != 4)
        {
            std::cerr << "[SMOKE_TEST] FAIL: expected 4 tracks, got " << tracks.size() << std::endl;
            return 1;
        }

        auto* track1 = tracks[0];
        track1->setName ("Smoke Lead");

        auto synth = edit->getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, {});

        if (synth == nullptr)
        {
            std::cerr << "[SMOKE_TEST] FAIL: failed to instantiate FourOscPlugin" << std::endl;
            return 1;
        }

        track1->pluginList.insertPlugin (synth, 0, nullptr);

        const tracktion::TimeRange barOne {
            tracktion::TimePosition::fromSeconds (0.0),
            edit->tempoSequence.toTime (tracktion::BeatPosition::fromBeats (4.0))
        };

        auto* clip = dynamic_cast<te::MidiClip*> (
            track1->insertNewClip (te::TrackItem::Type::midi, "Smoke Riff", barOne, nullptr));

        if (clip == nullptr)
        {
            std::cerr << "[SMOKE_TEST] FAIL: insertNewClip did not return MidiClip*" << std::endl;
            return 1;
        }

        auto& seq = clip->getSequence();
        auto& um = edit->getUndoManager();

        seq.addNote (60, tracktion::BeatPosition::fromBeats (0.0), tracktion::BeatDuration::fromBeats (1.0), 100, 0, &um);
        seq.addNote (64, tracktion::BeatPosition::fromBeats (1.0), tracktion::BeatDuration::fromBeats (1.0), 105, 0, &um);
        seq.addNote (67, tracktion::BeatPosition::fromBeats (2.0), tracktion::BeatDuration::fromBeats (1.0), 110, 0, &um);
        seq.addNote (72, tracktion::BeatPosition::fromBeats (3.0), tracktion::BeatDuration::fromBeats (1.0), 115, 0, &um);

        te::EditFileOperations fileOps (*edit);

        if (! fileOps.writeToFile (tempFile, false))
        {
            std::cerr << "[SMOKE_TEST] FAIL: EditFileOperations::writeToFile failed" << std::endl;
            return 1;
        }
    }

    // Reload from disk and verify round-trip persistence
    {
        auto reloaded = te::loadEditFromFile (engine, tempFile);

        if (reloaded == nullptr)
        {
            std::cerr << "[SMOKE_TEST] FAIL: loadEditFromFile returned nullptr" << std::endl;
            tempFile.deleteFile();
            return 1;
        }

        auto tracks = te::getAudioTracks (*reloaded);

        if (tracks.size() != 4)
        {
            std::cerr << "[SMOKE_TEST] FAIL: reloaded track count mismatch" << std::endl;
            tempFile.deleteFile();
            return 1;
        }

        int totalNotes = 0;
        for (auto* c : tracks[0]->getClips())
            if (auto* mc = dynamic_cast<te::MidiClip*> (c))
                totalNotes += mc->getSequence().getNumNotes();

        if (totalNotes != 4)
        {
            std::cerr << "[SMOKE_TEST] FAIL: expected 4 reloaded MIDI notes, got " << totalNotes << std::endl;
            tempFile.deleteFile();
            return 1;
        }
    }

    tempFile.deleteFile();

    std::cout << "[SMOKE_TEST] PASS: Engine, 4OSC, MIDI sequence, and .tracktionedit round-trip verified." << std::endl;
    return 0;
}
} // namespace

class AIDAWApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String& commandLine) override
    {
        if (commandLine.contains ("--smoke-test"))
        {
            setApplicationReturnValue (runHeadlessSmokeTest());
            quit();
            return;
        }

        mainWindow = std::make_unique<MainWindow> (getApplicationName());
    }

    void shutdown() override
    {
        mainWindow.reset();
    }

    void systemRequestedQuit() override
    {
        quit();
    }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (juce::String name)
            : DocumentWindow (name, juce::Colour (0xff181a1f), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (true, true);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override
        {
            JUCEApplication::getInstance()->systemRequestedQuit();
        }
    };

private:
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (AIDAWApplication)
