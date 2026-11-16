#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion::engine;

class MainComponent : public juce::Component,
                      private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    class Vst3ScanThread;

    void timerCallback() override;

    juce::File getStudioRootDir() const;
    juce::File getDefaultProjectFile() const;
    juce::File getPluginCacheFile() const;

    void loadCachedPlugins();
    void saveCachedPlugins();

    void startVst3Scan();
    void stopVst3Scan();

    void createNewSession (const juce::File& targetFile, bool seedDefaultContent);
    void openSessionDialog();
    void saveSession();
    void saveSessionAsDialog();
    void updateWindowTitle();

    te::Engine engine { "AI DAW" };
    std::unique_ptr<te::Edit> edit;

    juce::File currentEditFile;

    std::unique_ptr<Vst3ScanThread> scanThread;
    std::unique_ptr<juce::FileChooser> activeFileChooser;

    juce::TextButton newButton         { "New" };
    juce::TextButton openButton        { "Open..." };
    juce::TextButton saveButton        { "Save" };
    juce::TextButton saveAsButton      { "Save As..." };
    juce::TextButton playButton        { "Play" };
    juce::TextButton stopButton        { "Stop" };
    juce::TextButton scanPluginsButton { "Scan VST3" };
    juce::Label statusLabel            { {}, "Ready" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
