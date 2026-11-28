#pragma once

// Desktop-only boutique hardware faceplates for the five signature native
// modules. NEVER include this header from tcs_core / tcs_core_isolation.

#include <juce_gui_basics/juce_gui_basics.h>
#include <tracktion_engine/tracktion_engine.h>

#include <functional>
#include <memory>
#include <vector>

namespace te = tracktion::engine;

/** Returns a bespoke editor for the five native modules, or nullptr. */
std::unique_ptr<te::Plugin::EditorComponent> createCustomEditorForPlugin (te::Plugin& plugin);

class HardwareEditorBase : public te::Plugin::EditorComponent,
                           private juce::Timer
{
public:
    HardwareEditorBase (te::Plugin& targetPlugin, juce::String faceplateTitle,
                        juce::Colour accent, int width, int height);
    ~HardwareEditorBase() override;

    void paint (juce::Graphics& g) override;
    void resized() override;
    bool allowWindowResizing() override { return true; }
    juce::ComponentBoundsConstrainer* getBoundsConstrainer() override { return nullptr; }

    int countUnboundSliders() const noexcept;

protected:
    struct KnobControl
    {
        std::unique_ptr<juce::Slider> slider;
        std::unique_ptr<juce::Label> label;
        te::AutomatableParameter::Ptr parameter;
    };

    void addKnob (const juce::String& paramId, const juce::String& caption,
                  int x, int y, int size);
    juce::TextButton& addPad (const juce::String& name, int x, int y, int w, int h,
                              juce::Colour colour, std::function<void()> action);
    void addMeterLabel (const juce::String& prefix, int x, int y, int w);
    void setSectionDividers (const std::vector<juce::Colour>& colours);

    void drawSilkscreen (juce::Graphics& g, const juce::String& text,
                         juce::Rectangle<float> area, juce::Colour colour) const;
    void drawSectionBlock (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour) const;
    void drawRackEars (juce::Graphics& g) const;
    void drawDummyCap (juce::Graphics& g, juce::Rectangle<float> area, const juce::String& label) const;
    void drawStepLed (juce::Graphics& g, juce::Rectangle<float> area, bool lit) const;

    virtual void updateMeters() {}
    virtual void syncPlayhead (double beats, bool isPlaying) { juce::ignoreUnused (beats, isPlaying); }

    te::Plugin& plugin;
    juce::String title;
    juce::Colour accentColour;
    std::vector<KnobControl> knobs;
    std::vector<std::unique_ptr<juce::TextButton>> pads;
    std::vector<std::unique_ptr<juce::Label>> meterLabels;
    std::vector<juce::Colour> dividerColours;
    juce::Label titleLabel;
    int panelWidth = 640, panelHeight = 320;

private:
    void timerCallback() override;
};

class Tr808Editor : public HardwareEditorBase
{
public:
    explicit Tr808Editor (te::Plugin& p);
    void syncFromPlayheadBeats (double beats, bool isPlaying);
    int getActiveStep() const noexcept { return activeStep; }
private:
    void updateMeters() override;
    void syncPlayhead (double beats, bool isPlaying) override;
    void paint (juce::Graphics& g) override;
    int activeStep = -1;
};

class VintageSamplerEditor : public HardwareEditorBase
{
public:
    explicit VintageSamplerEditor (te::Plugin& p);
    ~VintageSamplerEditor() override;
    void paint (juce::Graphics& g) override;
private:
    void updateMeters() override;
    juce::TextButton loadButton;
    juce::TextButton resetButton;
    juce::Label sampleLabel;
    std::unique_ptr<juce::FileChooser> chooser;
};

class AnalogTapeEditor : public HardwareEditorBase
{
public:
    explicit AnalogTapeEditor (te::Plugin& p);
    void paint (juce::Graphics& g) override;
    void syncFromPlayheadBeats (double beats, bool isPlaying);
    float getReelAngle() const noexcept { return reelAngle; }
private:
    void updateMeters() override;
    void syncPlayhead (double beats, bool isPlaying) override;
    float leftNeedle = -60.0f, rightNeedle = -60.0f;
    float reelAngle = 0.0f;
};

class SidechainCompEditor : public HardwareEditorBase
{
public:
    explicit SidechainCompEditor (te::Plugin& p);
    void paint (juce::Graphics& g) override;
private:
    void updateMeters() override;
    float reductionDb = 0.0f;
};

class VocalChannelEditor : public HardwareEditorBase
{
public:
    explicit VocalChannelEditor (te::Plugin& p);
    ~VocalChannelEditor() override;
    void paint (juce::Graphics& g) override;
private:
    void updateMeters() override;
    float limiterGrDb = 0.0f;
    juce::ComboBox presetCombo;
};
