#include "MainComponent.h"

// ============================================================================
// Owned, Interruptible Background VST3 Scanner (Zero Dangling `this`)
// ============================================================================
class MainComponent::Vst3ScanThread : public juce::Thread
{
public:
    explicit Vst3ScanThread (MainComponent& ownerComponent)
        : juce::Thread ("Vst3ScanThread"),
          safeOwner (&ownerComponent)
    {
    }

    void run() override
    {
        juce::VST3PluginFormat vst3Format;
        const juce::FileSearchPath searchPaths = vst3Format.getDefaultLocationsToSearch();

        juce::KnownPluginList threadLocalList;

        juce::PluginDirectoryScanner scanner (
            threadLocalList,
            vst3Format,
            searchPaths,
            true,
            juce::File());

        juce::String currentPlugin;

        while (! threadShouldExit() && scanner.scanNextFile (true, currentPlugin))
        {
            auto safeTarget = safeOwner;
            const juce::String pluginNameCopy = currentPlugin;

            juce::MessageManager::callAsync ([safeTarget, pluginNameCopy]
            {
                if (safeTarget != nullptr)
                    safeTarget->statusLabel.setText ("Scanning: " + pluginNameCopy,
                                                     juce::dontSendNotification);
            });
        }

        if (threadShouldExit())
            return;

        auto xmlSnapshot = threadLocalList.createXml();
        auto safeTarget = safeOwner;

        juce::MessageManager::callAsync ([safeTarget, xml = std::move (xmlSnapshot)]() mutable
        {
            if (safeTarget == nullptr)
                return;

            auto& knownList = safeTarget->engine.getPluginManager().knownPluginList;
            knownList.clear();

            if (xml != nullptr)
                knownList.recreateFromXml (*xml);

            safeTarget->saveCachedPlugins();

            safeTarget->scanPluginsButton.setEnabled (true);
            safeTarget->statusLabel.setText (
                "VST3 Scan Complete. Found " + juce::String (knownList.getNumTypes()) + " plugins.",
                juce::dontSendNotification);
        });
    }

private:
    juce::Component::SafePointer<MainComponent> safeOwner;
};

// ============================================================================
// MainComponent Implementation
// ============================================================================
MainComponent::MainComponent()
{
    loadCachedPlugins();

    const auto defaultFile = getDefaultProjectFile();

    if (defaultFile.existsAsFile())
    {
        edit = te::loadEditFromFile (engine, defaultFile);

        if (edit != nullptr)
        {
            currentEditFile = defaultFile;
            statusLabel.setText ("Loaded: " + currentEditFile.getFileName(), juce::dontSendNotification);
        }
    }

    if (edit == nullptr)
    {
        createNewSession (defaultFile, true);
        saveSession();
    }

    addAndMakeVisible (newButton);
    addAndMakeVisible (openButton);
    addAndMakeVisible (saveButton);
    addAndMakeVisible (saveAsButton);
    addAndMakeVisible (playButton);
    addAndMakeVisible (stopButton);
    addAndMakeVisible (scanPluginsButton);
    addAndMakeVisible (statusLabel);

    newButton.onClick = [this]
    {
        createNewSession (getDefaultProjectFile(), true);
        saveSession();

        statusLabel.setText ("Created new session: " + currentEditFile.getFileName(),
                             juce::dontSendNotification);
    };

    openButton.onClick   = [this] { openSessionDialog(); };
    saveButton.onClick   = [this] { saveSession(); };
    saveAsButton.onClick = [this] { saveSessionAsDialog(); };

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

    scanPluginsButton.onClick = [this] { startVst3Scan(); };

    setSize (1040, 620);
    startTimerHz (30);
    updateWindowTitle();
}

MainComponent::~MainComponent()
{
    stopTimer();
    stopVst3Scan();
    activeFileChooser.reset();

    if (edit != nullptr)
    {
        edit->getTransport().stop (false, false);
        edit.reset();
    }
}

juce::File MainComponent::getStudioRootDir() const
{
    auto dir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                   .getChildFile ("Toxic Creationz Studio");

    dir.createDirectory();
    return dir;
}

juce::File MainComponent::getDefaultProjectFile() const
{
    auto projectsDir = getStudioRootDir().getChildFile ("Projects");
    projectsDir.createDirectory();

    return projectsDir.getChildFile ("autosave_session.tracktionedit");
}

juce::File MainComponent::getPluginCacheFile() const
{
    return getStudioRootDir().getChildFile ("known_vst3_plugins.xml");
}

void MainComponent::loadCachedPlugins()
{
    const auto cacheFile = getPluginCacheFile();

    if (! cacheFile.existsAsFile())
        return;

    if (auto xml = juce::XmlDocument::parse (cacheFile))
    {
        auto& knownList = engine.getPluginManager().knownPluginList;
        knownList.clear();
        knownList.recreateFromXml (*xml);

        statusLabel.setText ("Loaded " + juce::String (knownList.getNumTypes()) + " cached VST3 plugins.",
                             juce::dontSendNotification);
    }
}

void MainComponent::saveCachedPlugins()
{
    if (auto xml = engine.getPluginManager().knownPluginList.createXml())
        xml->writeTo (getPluginCacheFile());
}

void MainComponent::startVst3Scan()
{
    stopVst3Scan();

    scanPluginsButton.setEnabled (false);
    statusLabel.setText ("Scanning VST3 directories...", juce::dontSendNotification);

    scanThread = std::make_unique<Vst3ScanThread> (*this);
    scanThread->startThread();
}

void MainComponent::stopVst3Scan()
{
    if (scanThread != nullptr)
    {
        scanThread->signalThreadShouldExit();
        scanThread->stopThread (3000);
        scanThread.reset();
    }
}

void MainComponent::createNewSession (const juce::File& targetFile, bool seedDefaultContent)
{
    if (edit != nullptr)
    {
        edit->getTransport().stop (false, false);
        edit.reset();
    }

    targetFile.getParentDirectory().createDirectory();

    if (targetFile.existsAsFile())
        targetFile.deleteFile();

    currentEditFile = targetFile;

    edit = te::createEmptyEdit (engine, currentEditFile);
    edit->ensureNumberOfAudioTracks (4);
    edit->tempoSequence.getTempo (0)->setBpm (124.0);

    if (seedDefaultContent)
    {
        auto tracks = te::getAudioTracks (*edit);

        if (! tracks.isEmpty())
        {
            auto* track1 = tracks[0];
            track1->setName ("Lead Synth");

            if (auto synth = edit->getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, {}))
                track1->pluginList.insertPlugin (synth, 0, nullptr);

            const tracktion::TimeRange barOne {
                tracktion::TimePosition::fromSeconds (0.0),
                edit->tempoSequence.toTime (tracktion::BeatPosition::fromBeats (4.0))
            };

            if (auto* clip = dynamic_cast<te::MidiClip*> (
                    track1->insertNewClip (te::TrackItem::Type::midi, "Intro Riff", barOne, nullptr)))
            {
                auto& seq = clip->getSequence();
                auto& um = edit->getUndoManager();

                seq.addNote (60, tracktion::BeatPosition::fromBeats (0.0), tracktion::BeatDuration::fromBeats (1.0), 110, 0, &um);
                seq.addNote (63, tracktion::BeatPosition::fromBeats (1.0), tracktion::BeatDuration::fromBeats (1.0), 105, 0, &um);
                seq.addNote (67, tracktion::BeatPosition::fromBeats (2.0), tracktion::BeatDuration::fromBeats (1.0), 115, 0, &um);
                seq.addNote (70, tracktion::BeatPosition::fromBeats (3.0), tracktion::BeatDuration::fromBeats (1.0), 120, 0, &um);
            }
        }
    }

    updateWindowTitle();
}

void MainComponent::saveSession()
{
    if (edit == nullptr)
        return;

    if (currentEditFile == juce::File())
        currentEditFile = getDefaultProjectFile();

    currentEditFile.getParentDirectory().createDirectory();

    // Write the edit's XML state directly to the target file. This is the
    // synchronous EditFileOperations path; the edit's own file binding is set
    // at create/load time, so Save As simply retargets currentEditFile.
    te::EditFileOperations fileOps (*edit);
    const bool saved = fileOps.writeToFile (currentEditFile, false);

    if (saved)
    {
        edit->resetChangedStatus();
        statusLabel.setText ("Saved: " + currentEditFile.getFileName(), juce::dontSendNotification);
        updateWindowTitle();
    }
    else
    {
        statusLabel.setText ("Save failed: " + currentEditFile.getFullPathName(), juce::dontSendNotification);
    }
}

void MainComponent::openSessionDialog()
{
    auto projectsDir = getStudioRootDir().getChildFile ("Projects");

    activeFileChooser = std::make_unique<juce::FileChooser> (
        "Open Tracktion Edit",
        projectsDir,
        "*.tracktionedit");

    juce::Component::SafePointer<MainComponent> safeThis (this);

    activeFileChooser->launchAsync (
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safeThis] (const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr)
                return;

            const auto chosen = chooser.getResult();
            if (! chosen.existsAsFile())
                return;

            if (safeThis->edit != nullptr)
            {
                safeThis->edit->getTransport().stop (false, false);
                safeThis->edit.reset();
            }

            safeThis->edit = te::loadEditFromFile (safeThis->engine, chosen);

            if (safeThis->edit != nullptr)
            {
                safeThis->currentEditFile = chosen;
                safeThis->statusLabel.setText ("Opened: " + chosen.getFileName(), juce::dontSendNotification);
                safeThis->updateWindowTitle();
            }
            else
            {
                safeThis->statusLabel.setText ("Failed to open: " + chosen.getFileName(), juce::dontSendNotification);
            }
        });
}

void MainComponent::saveSessionAsDialog()
{
    if (edit == nullptr)
        return;

    auto projectsDir = getStudioRootDir().getChildFile ("Projects");

    activeFileChooser = std::make_unique<juce::FileChooser> (
        "Save Tracktion Edit As",
        projectsDir.getChildFile ("untitled.tracktionedit"),
        "*.tracktionedit");

    juce::Component::SafePointer<MainComponent> safeThis (this);

    activeFileChooser->launchAsync (
        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
            | juce::FileBrowserComponent::warnAboutOverwriting,
        [safeThis] (const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr || safeThis->edit == nullptr)
                return;

            auto target = chooser.getResult();
            if (target == juce::File())
                return;

            if (! target.hasFileExtension (".tracktionedit"))
                target = target.withFileExtension (".tracktionedit");

            safeThis->currentEditFile = target;
            safeThis->saveSession();
        });
}

void MainComponent::updateWindowTitle()
{
    if (auto* topWindow = findParentComponentOfClass<juce::DocumentWindow>())
    {
        const auto suffix = currentEditFile.exists() ? (" — " + currentEditFile.getFileName()) : juce::String();
        topWindow->setName (JUCE_APPLICATION_NAME_STRING + suffix);
    }
}

void MainComponent::timerCallback()
{
    if (edit != nullptr && edit->getTransport().isPlaying())
    {
        const double posSec = edit->getTransport().getPosition().inSeconds();
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

    auto addToolbarBtn = [&topBar] (juce::TextButton& btn, int width)
    {
        btn.setBounds (topBar.removeFromLeft (width));
        topBar.removeFromLeft (6);
    };

    addToolbarBtn (newButton, 64);
    addToolbarBtn (openButton, 74);
    addToolbarBtn (saveButton, 64);
    addToolbarBtn (saveAsButton, 82);

    topBar.removeFromLeft (8);

    addToolbarBtn (playButton, 68);
    addToolbarBtn (stopButton, 68);

    topBar.removeFromLeft (8);

    addToolbarBtn (scanPluginsButton, 100);

    topBar.removeFromLeft (10);

    statusLabel.setBounds (topBar);
}
