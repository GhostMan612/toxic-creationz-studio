#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <tracktion_engine/tracktion_engine.h>

#include "ArrangementView.h"
#include "PianoRollComponent.h"
#include "MixerComponent.h"
#include "ModularRackComponent.h"
#include "BgmGeneratorComponent.h"
#include "DawIpcServer.h"
#include "WaveformEditorComponent.h"
#include "MixdownExporter.h"
#include "MidiKeyboardDock.h"
#include "RecordingManager.h"
#include "GameForgeComponent.h"
#include "RpgSoundboardComponent.h"
#include "GhostTutorComponent.h"
#include "UiSoundService.h"
#include "ScriptPrompterComponent.h"
#include "StudioLookAndFeel.h"
#include "StudioPluginWindow.h"

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
    bool keyPressed (const juce::KeyPress& key) override;

    bool hasUnsavedChanges() const noexcept { return sessionDirty; }
    bool isVst3ScanRunning() const;
    bool isScanAbortRequested() const noexcept { return scanAbortRequested.load(); }

    // Headless regression harness, invoked by AI DAW.exe --smoke-test.
    static int runHeadlessSmokeTest();

    void guardUnsavedChanges (std::function<void (bool)> onProceed);

    // WO-022B workspace suite switcher.
    enum class WorkspaceMode { studio, modularRack, voBooth };
    void setWorkspaceMode (WorkspaceMode mode);
    WorkspaceMode getWorkspaceMode() const noexcept { return workspaceMode; }

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
    void loadWorkspaceMode();
    void saveWorkspaceMode() const;

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

    void installEditFilePathResolver (te::Edit& targetEdit);
    void attachEdit (te::Edit& editToAttach);
    void syncTransportControlsFromEdit();
    void updateTransportPositionReadout();
    static bool isTransientEditSubtree (juce::ValueTree tree);

    void createNewSession (const juce::File& targetFile, bool seedDefaultContent, bool untitled);
    bool loadSessionFromFileSafely (const juce::File& fileToLoad, juce::String* errorOut = nullptr);
    void openSessionDialog();
    void saveSession (std::function<void (bool)> completion = nullptr);
    void saveSessionAsDialog (std::function<void (bool)> completion = nullptr);
    void exportMixdownDialog();
    void updateWindowTitle();

    StudioLookAndFeel studioLookAndFeel;
    te::Engine engine { "AI DAW", std::make_unique<StudioUIBehaviour>(), nullptr };
    std::unique_ptr<te::Edit> edit;
    RecordingManager recordingManager { engine };
    MixerComponent mixerComponent { engine };
    ModularRackComponent modularRackComponent { engine };
    RiffLibrary riffLibrary;
    BgmGeneratorComponent bgmGeneratorComponent { riffLibrary };
    DawIpcServer ipcServer { riffLibrary, recordingManager };

    WaveformEditorComponent waveformEditorComponent;
    MidiKeyboardDock midiKeyboardDock;
    ScriptPrompterComponent scriptPrompter;
    tcs::game::GameForgeComponent gameForgeComponent;
    RpgSoundboardComponent rpgSoundboardComponent;
    GhostTutorComponent ghostTutorComponent;
    tcs::ui::UiSoundService uiSoundService;

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
    juce::TextButton exportWavButton   { "Export WAV" };
    juce::TextButton addTrackButton    { "+ Track" };
    juce::TextButton undoButton        { "Undo" };
    juce::TextButton redoButton        { "Redo" };
    juce::TextButton playButton        { "Play" };
    juce::TextButton stopButton        { "Stop" };
    juce::TextButton recordButton      { "Rec" };
    juce::TextButton loopToggleButton  { "Loop" };
    juce::TextButton clickToggleButton { "Click" };
    juce::TextButton countInButton     { "Count-In" };
    juce::TextButton autoToggleButton  { "Auto" };
    juce::TextButton scanPluginsButton { "Scan VST3" };
    juce::TextButton mixerToggleButton   { "Mixer" };
    juce::TextButton rackToggleButton     { "Rack" };
    juce::TextButton keysToggleButton     { "Keys" };
    juce::TextButton bgmToggleButton       { "BGM Gen" };
    juce::TextButton waveEditorToggleButton { "Wave Edit" };
    juce::TextButton voBoothToggleButton   { "VO Booth" };
    juce::TextButton studioWorkspaceButton { "Studio" };
    juce::TextButton rackWorkspaceButton   { "Modular Rack" };
    juce::TextButton voWorkspaceButton     { "VO Booth Suite" };
    juce::TextButton gameForgePlaceholder  { "Game Forge" };
    juce::TextButton rpgBoardPlaceholder   { "RPG Board" };
    juce::TextButton ghostToggleButton     { "GHOST" };
    WorkspaceMode workspaceMode = WorkspaceMode::studio;
    juce::TextButton audioSettingsButton { "Audio Settings" };
    juce::Slider bpmSlider;
    juce::Label transportPosLabel      { {}, "Bar 1.1" };
    juce::Label statusLabel            { {}, "Ready" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
