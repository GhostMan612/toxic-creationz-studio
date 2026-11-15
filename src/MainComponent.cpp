#include "MainComponent.h"

MainComponent::MainComponent()
{
    // Register VST3 format with Tracktion's PluginManager
    auto& pluginManager = engine.getPluginManager();
    pluginManager.knownPluginList.clear();

    setupDefaultSession();

    addAndMakeVisible (playButton);
    addAndMakeVisible (stopButton);
    addAndMakeVisible (scanPluginsButton);
    addAndMakeVisible (statusLabel);

    playButton.onClick = [this]
    {
        if (edit != nullptr)
            edit->getTransport().play (false);
    };

    stopButton.onClick = [this]
    {
        if (edit != nullptr)
            edit->getTransport().stop (false, false);
    };

    scanPluginsButton.onClick = [this]
    {
        scanVst3PluginsAsync();
    };

    setSize (960, 600);
    startTimerHz (30);
}

MainComponent::~MainComponent()
{
    stopTimer();

    if (edit != nullptr)
    {
        edit->getTransport().stop (false, false);
        edit.reset();
    }
}

void MainComponent::setupDefaultSession()
{
    auto tempEditFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("default_session.tracktionedit");
    tempEditFile.deleteFile();

    edit = te::createEmptyEdit (engine, tempEditFile);

    edit->ensureNumberOfAudioTracks (4);
    edit->tempoSequence.getTempo (0)->setBpm (124.0);

    // Seed Track 1 with a 1-bar MIDI clip and a built-in Tracktion 4OSC synth
    auto tracks = te::getAudioTracks (*edit);

    if (! tracks.isEmpty())
    {
        auto* track1 = tracks[0];
        track1->setName ("Lead Synth");

        if (auto synth = edit->getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, {}))
            track1->pluginList.insertPlugin (synth, 0, nullptr);

        const te::TimeRange barOne { te::TimePosition::fromSeconds (0.0),
                                    edit->tempoSequence.toTime (te::BeatPosition::fromBeats (4.0)) };

        if (auto clip = dynamic_cast<te::MidiClip*> (
                track1->insertNewClip (te::TrackItem::Type::midi, "Intro Riff", barOne, nullptr).get()))
        {
            auto& seq = clip->getSequence();
            auto& um = edit->getUndoManager();

            seq.addNote (60, te::BeatPosition::fromBeats (0.0), te::BeatDuration::fromBeats (1.0), 110, 0, &um);
            seq.addNote (63, te::BeatPosition::fromBeats (1.0), te::BeatDuration::fromBeats (1.0), 105, 0, &um);
            seq.addNote (67, te::BeatPosition::fromBeats (2.0), te::BeatDuration::fromBeats (1.0), 115, 0, &um);
            seq.addNote (70, te::BeatPosition::fromBeats (3.0), te::BeatDuration::fromBeats (1.0), 120, 0, &um);
        }
    }
}

void MainComponent::scanVst3PluginsAsync()
{
    statusLabel.setText ("Scanning C:\\Program Files\\Common Files\\VST3 ...", juce::dontSendNotification);

    juce::VST3PluginFormat vst3Format;
    juce::FileSearchPath defaultPaths = vst3Format.getDefaultLocationsToSearch();
    auto& knownList = engine.getPluginManager().knownPluginList;

    // Run directory scan off the UI thread, instantiate plugins only on message thread
    juce::Thread::launch ([this, defaultPaths, &knownList]
    {
        juce::VST3PluginFormat format;
        juce::PluginDirectoryScanner scanner (knownList, format, defaultPaths, true, juce::File());
        juce::String pluginBeingScanned;

        while (scanner.scanNextFile (true, pluginBeingScanned))
        {
            juce::MessageManager::callAsync ([this, pluginBeingScanned]
            {
                statusLabel.setText ("Scanning: " + pluginBeingScanned, juce::dontSendNotification);
            });
        }

        const int count = knownList.getNumTypes();
        juce::MessageManager::callAsync ([this, count]
        {
            statusLabel.setText ("VST3 Scan Complete. Found " + juce::String (count) + " plugins.",
                                juce::dontSendNotification);
        });
    });
}

void MainComponent::timerCallback()
{
    if (edit != nullptr)
    {
        const double posSec = edit->getTransport().getPosition().inSeconds();
        const bool playing = edit->getTransport().isPlaying();

        if (playing)
            statusLabel.setText ("Playing: " + juce::String (posSec, 2) + "s", juce::dontSendNotification);
    }
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff181a1f));
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced (12);
    auto topBar = area.removeFromTop (36);

    playButton.setBounds (topBar.removeFromLeft (80));
    topBar.removeFromLeft (8);
    stopButton.setBounds (topBar.removeFromLeft (80));
    topBar.removeFromLeft (8);
    scanPluginsButton.setBounds (topBar.removeFromLeft (110));
    topBar.removeFromLeft (16);
    statusLabel.setBounds (topBar);
}
