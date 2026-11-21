#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <tracktion_engine/tracktion_engine.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace te = tracktion::engine;

//==============================================================================
// Scrollable dark-themed parameter strip editor for built-in Tracktion and
// custom CV plugins that do not supply their own native VST3 UI.
//==============================================================================
class GenericTracktionPluginEditor : public te::Plugin::EditorComponent,
                                     private juce::Timer
{
public:
    explicit GenericTracktionPluginEditor (te::Plugin& targetPlugin)
        : plugin (targetPlugin)
    {
        setOpaque (true);

        const int numParams = plugin.getNumAutomatableParameters();

        addAndMakeVisible (headerLabel);
        headerLabel.setText (plugin.getName() + "  (" + juce::String (numParams) + " params)",
                             juce::dontSendNotification);
        headerLabel.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
        headerLabel.setColour (juce::Label::textColourId, juce::Colour (0xff38bdf8));

        addAndMakeVisible (viewport);
        viewport.setViewedComponent (&paramsContainer, false);
        viewport.setScrollBarsShown (true, false);

        for (int i = 0; i < numParams; ++i)
        {
            auto p = plugin.getAutomatableParameter (i);
            if (p == nullptr)
                continue;

            ParamRow row;
            row.paramIndex = i;

            row.nameLabel = std::make_unique<juce::Label>();
            row.nameLabel->setText (p->getParameterName(), juce::dontSendNotification);
            row.nameLabel->setFont (juce::Font (juce::FontOptions (11.5f)));
            row.nameLabel->setColour (juce::Label::textColourId, juce::Colour (0xffe2e8f0));
            row.nameLabel->setJustificationType (juce::Justification::centredLeft);

            row.slider = std::make_unique<juce::Slider> (juce::Slider::LinearHorizontal,
                                                         juce::Slider::NoTextBox);
            const auto range = p->getValueRange();
            const double minVal = range.getLength() > 0.0f ? static_cast<double> (range.getStart()) : 0.0;
            const double maxVal = range.getLength() > 0.0f ? static_cast<double> (range.getEnd()) : 1.0;
            const double step   = (p->isDiscrete() && range.getLength() >= 1.0f) ? 1.0 : 0.0;

            row.slider->setRange (minVal, maxVal, step);
            row.slider->setValue (static_cast<double> (p->getCurrentValue()), juce::dontSendNotification);
            row.slider->setColour (juce::Slider::trackColourId, juce::Colour (0xff38bdf8));
            row.slider->setColour (juce::Slider::backgroundColourId, juce::Colour (0xff2a3242));
            row.slider->setColour (juce::Slider::thumbColourId, juce::Colour (0xfff1f5f9));

            row.valueLabel = std::make_unique<juce::Label>();
            row.valueLabel->setText (p->getCurrentValueAsStringWithLabel(), juce::dontSendNotification);
            row.valueLabel->setFont (juce::Font (juce::FontOptions (11.0f)));
            row.valueLabel->setColour (juce::Label::textColourId, juce::Colour (0xff94a3b8));
            row.valueLabel->setJustificationType (juce::Justification::centredRight);

            const size_t rowIdx = rows.size();
            row.slider->onValueChange = [this, rowIdx]
            {
                if (rowIdx >= rows.size())
                    return;

                auto& r = rows[rowIdx];
                if (auto param = plugin.getAutomatableParameter (r.paramIndex))
                {
                    param->setParameter (static_cast<float> (r.slider->getValue()), juce::sendNotification);
                    r.valueLabel->setText (param->getCurrentValueAsStringWithLabel(),
                                           juce::dontSendNotification);
                }
            };

            paramsContainer.addAndMakeVisible (*row.nameLabel);
            paramsContainer.addAndMakeVisible (*row.slider);
            paramsContainer.addAndMakeVisible (*row.valueLabel);
            rows.push_back (std::move (row));
        }

        const int initialH = juce::jlimit (140, 420, 46 + static_cast<int> (rows.size()) * 28);
        setSize (440, initialH);
        startTimerHz (10);
    }

    ~GenericTracktionPluginEditor() override
    {
        stopTimer();
    }

    bool allowWindowResizing() override { return true; }
    juce::ComponentBoundsConstrainer* getBoundsConstrainer() override { return nullptr; }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff141821));

        g.setColour (juce::Colour (0xff1b212d));
        g.fillRect (0, 0, getWidth(), 32);
        g.setColour (juce::Colour (0xff2b3344));
        g.drawHorizontalLine (31, 0.0f, static_cast<float> (getWidth()));
    }

    void resized() override
    {
        auto area = getLocalBounds();
        headerLabel.setBounds (area.removeFromTop (32).reduced (10, 4));
        viewport.setBounds (area);

        const int rowH = 28;
        const int containerW = std::max (220, viewport.getMaximumVisibleWidth());
        const int containerH = std::max (area.getHeight(), 8 + static_cast<int> (rows.size()) * rowH);
        paramsContainer.setSize (containerW, containerH);

        int y = 4;
        for (auto& r : rows)
        {
            juce::Rectangle<int> rowBounds (8, y, containerW - 16, rowH - 2);
            const int nameW  = std::min (155, rowBounds.getWidth() / 3);
            const int valW   = 78;

            r.nameLabel->setBounds (rowBounds.removeFromLeft (nameW));
            r.valueLabel->setBounds (rowBounds.removeFromRight (valW));
            rowBounds.removeFromLeft (4);
            rowBounds.removeFromRight (4);
            r.slider->setBounds (rowBounds);
            y += rowH;
        }
    }

private:
    struct ParamRow
    {
        int paramIndex = 0;
        std::unique_ptr<juce::Label> nameLabel;
        std::unique_ptr<juce::Slider> slider;
        std::unique_ptr<juce::Label> valueLabel;
    };

    void timerCallback() override
    {
        for (auto& r : rows)
        {
            if (r.slider == nullptr || r.slider->isMouseButtonDown())
                continue;

            if (auto p = plugin.getAutomatableParameter (r.paramIndex))
            {
                const double current = static_cast<double> (p->getCurrentValue());
                if (std::abs (r.slider->getValue() - current) > 1e-5)
                    r.slider->setValue (current, juce::dontSendNotification);

                r.valueLabel->setText (p->getCurrentValueAsStringWithLabel(),
                                       juce::dontSendNotification);
            }
        }
    }

    te::Plugin& plugin;
    juce::Label headerLabel;
    juce::Viewport viewport;
    juce::Component paramsContainer;
    std::vector<ParamRow> rows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GenericTracktionPluginEditor)
};

//==============================================================================
// Floating host window for both native VST3 UIs and GenericTracktionPluginEditor.
//==============================================================================
class StudioPluginWindow : public juce::DocumentWindow
{
public:
    explicit StudioPluginWindow (te::Plugin& plug)
        : DocumentWindow (plug.getName(),
                          juce::Colour (0xff141821),
                          juce::DocumentWindow::closeButton,
                          true),
          plugin (plug)
    {
        setUsingNativeTitleBar (true);
        getConstrainer()->setMinimumOnscreenAmounts (0x10000, 50, 30, 50);
        setResizeLimits (240, 120, 4000, 4000);

        recreateEditor();

        if (plugin.windowState != nullptr)
            setBoundsConstrained (getLocalBounds() + plugin.windowState->choosePositionForPluginWindow());

        updateStoredBounds = true;
    }

    ~StudioPluginWindow() override
    {
        updateStoredBounds = false;
        plugin.edit.flushPluginStateIfNeeded (plugin);
        setEditor (nullptr);
    }

    static std::unique_ptr<juce::Component> create (te::Plugin& plugin)
    {
        if (auto* externalPlugin = dynamic_cast<te::ExternalPlugin*> (&plugin))
            if (externalPlugin->getAudioPluginInstance() == nullptr)
                return nullptr;

        auto w = std::make_unique<StudioPluginWindow> (plugin);

        if (w->getEditor() == nullptr)
            return {};

        w->show();
        return w;
    }

    void show()
    {
        setVisible (true);
        toFront (false);
        setBoundsConstrained (getBounds());
    }

    void setEditor (std::unique_ptr<te::Plugin::EditorComponent> newEditor)
    {
        setConstrainer (nullptr);
        setContentNonOwned (nullptr, false);
        editor.reset();

        if (newEditor != nullptr)
        {
            editor = std::move (newEditor);
            setContentNonOwned (editor.get(), true);
        }

        setResizable (editor == nullptr || editor->allowWindowResizing(), false);

        if (editor != nullptr && editor->allowWindowResizing())
            setConstrainer (editor->getBoundsConstrainer());
    }

    te::Plugin::EditorComponent* getEditor() const noexcept { return editor.get(); }

    void recreateEditor()
    {
        setEditor (nullptr);
        auto ed = plugin.createEditor();

        if (ed == nullptr)
            ed = std::make_unique<GenericTracktionPluginEditor> (plugin);

        setEditor (std::move (ed));
    }

    void recreateEditorAsync()
    {
        setEditor (nullptr);

        juce::Timer::callAfterDelay (50, [safeThis = juce::Component::SafePointer<StudioPluginWindow> (this)]
        {
            if (safeThis != nullptr)
                safeThis->recreateEditor();
        });
    }

private:
    void moved() override
    {
        if (updateStoredBounds && plugin.windowState != nullptr)
        {
            plugin.windowState->lastWindowBounds = getBounds();
            plugin.edit.pluginChanged (plugin);
        }
    }

    void userTriedToCloseWindow() override
    {
        if (plugin.windowState != nullptr)
            plugin.windowState->closeWindowExplicitly();
    }

    void closeButtonPressed() override
    {
        userTriedToCloseWindow();
    }

    float getDesktopScaleFactor() const override { return 1.0f; }

    std::unique_ptr<te::Plugin::EditorComponent> editor;
    te::Plugin& plugin;
    bool updateStoredBounds = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StudioPluginWindow)
};

//==============================================================================
// Studio Tracktion UIBehaviour enabling native VST3 & built-in plugin windows.
//==============================================================================
class StudioUIBehaviour : public te::UIBehaviour
{
public:
    StudioUIBehaviour() = default;

    std::unique_ptr<juce::Component> createPluginWindow (te::PluginWindowState& pws) override
    {
        if (auto* ws = dynamic_cast<te::Plugin::WindowState*> (&pws))
            return StudioPluginWindow::create (ws->plugin);

        return {};
    }

    void recreatePluginWindowContentAsync (te::Plugin& p) override
    {
        if (p.windowState != nullptr)
        {
            if (auto* w = dynamic_cast<StudioPluginWindow*> (p.windowState->pluginWindow.get()))
            {
                w->recreateEditorAsync();
                return;
            }
        }

        UIBehaviour::recreatePluginWindowContentAsync (p);
    }
};
