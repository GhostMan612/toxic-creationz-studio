#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <tracktion_engine/tracktion_engine.h>

#include "ArrangementView.h"
#include "PianoRollComponent.h"
#include "MixerComponent.h"
#include "ModularRackComponent.h"

#include <atomic>
#include <functional>
#include <vector>

namespace te = tracktion::engine;

class MainComponent : public juce::Component,
                      private juce::Timer,
                      private juce::ValueTree::Listener
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

    bool hasUnsavedChanges() const noexcept { return sessionDirty; }
    bool isVst3ScanRunning() const;
    bool isScanAbortRequested() const noexcept { return scanAbortRequested.load(); }

    // Headless regression harness, invoked by AI DAW.exe --smoke-test.
    static int runHeadlessSmokeTest();

private:
    class Vst3ScanThread;

    // juce::ValueTree::Listener
    void valueTreePropertyChanged (juce::ValueTree& treeWhosePropertyHasChanged,
                                   const juce::Identifier& property) override;
    void valueTreeChildAdded (juce::ValueTree& parentTree,
                              juce::ValueTree& childWhichHasBeenAdded) override;
    void valueTreeChildRemoved (juce::ValueTree& parentTree,
                                juce::ValueTree& childWhichHasBeenRemoved,
                                int indexFromWhichChildWasRemoved) override;
    void valueTreeChildOrderChanged (juce::ValueTree& parentTreeWhoseChildrenHaveMoved,
                                     int oldIndex, int newIndex) override;
    void valueTreeParentChanged (juce::ValueTree& treeWhoseParentHasChanged) override;
    void valueTreeRedirected (juce::ValueTree& treeWhichHasBeenChanged) override;

    void timerCallback() override;

    juce::File getStudioRootDir() const;
    juce::File getProjectsDir() const;
    juce::File getDefaultProjectFile() const;
    juce::File getPluginCacheFile() const;

    void loadCachedPlugins();
    void saveCachedPlugins();

    void startVst3Scan();
    void stopVst3Scan (int timeoutMs = 3000);
    void reapRetiredScanThreads();

    void attachStateListener (te::Edit& edit);
    void detachStateListener (te::Edit& edit);
    void markSessionDirty();
    void snapshotSavedState();
    void refreshDirtyFromSavedState();
    static juce::ValueTree getContentState (const juce::ValueTree& state);

    void attachEdit (te::Edit& editToAttach);
    static bool isTransientEditSubtree (juce::ValueTree tree);

    void guardUnsavedChanges (std::function<void (bool)> onProceed);

    void createNewSession (const juce::File& targetFile, bool seedDefaultContent, bool untitled);
    bool loadSessionFromFileSafely (const juce::File& fileToLoad, juce::String* errorOut = nullptr);
    void openSessionDialog();
    void saveSession (std::function<void (bool)> completion = nullptr);
    void saveSessionAsDialog (std::function<void (bool)> completion = nullptr);
    void updateWindowTitle();

    te::Engine engine { "AI DAW" };
    std::unique_ptr<te::Edit> edit;
    MixerComponent mixerComponent { engine };
    ModularRackComponent modularRackComponent { engine };

    juce::File currentEditFile;
    bool sessionUntitled = false;
    bool sessionDirty = false;
    bool suppressDirtyTracking = false;
    juce::ValueTree savedEditState;

    std::unique_ptr<Vst3ScanThread> scanThread;
    std::vector<std::unique_ptr<Vst3ScanThread>> retiredScanThreads;
    std::atomic<bool> scanAbortRequested { false };
    std::atomic<int> scanGeneration { 0 };
    std::unique_ptr<juce::FileChooser> activeFileChooser;
    juce::ScopedMessageBox alertBox;

    ArrangementView arrangementView;
    PianoRollComponent pianoRoll;

    juce::TextButton newButton         { "New" };
    juce::TextButton openButton        { "Open..." };
    juce::TextButton saveButton        { "Save" };
    juce::TextButton saveAsButton      { "Save As..." };
    juce::TextButton addTrackButton    { "+ Track" };
    juce::TextButton undoButton        { "Undo" };
    juce::TextButton redoButton        { "Redo" };
    juce::TextButton playButton        { "Play" };
    juce::TextButton stopButton        { "Stop" };
    juce::TextButton scanPluginsButton { "Scan VST3" };
    juce::TextButton mixerToggleButton   { "Mixer" };
    juce::TextButton rackToggleButton     { "Rack" };
    juce::TextButton audioSettingsButton { "Audio Settings" };
    juce::Slider bpmSlider;
    juce::Label statusLabel            { {}, "Ready" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
