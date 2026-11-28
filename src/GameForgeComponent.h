#pragma once

#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace tcs
{
namespace game
{
/** Desktop-only Game Forge dock. All work is dispatched through onAction as
 *  JSON-RPC method name + params JSON; no IPC or Edit pointers are retained.
 */
class GameForgeComponent : public juce::Component
{
public:
    GameForgeComponent();
    ~GameForgeComponent() override;

    void resized() override;

    void setStatus (const juce::String& text);
    void setExportDirectory (const juce::File& dir);

    std::function<void (const juce::String&, const juce::String&)> onAction;
    std::function<void (const juce::String&)> onUiSound;

private:
    void forgeVariations();
    void exportLoop();
    void exportStems();
    void exportPack();

    juce::Label titleLabel       { {}, "GAME FORGE" };
    juce::Label forgeLabel       { {}, "SFX Forge" };
    juce::Label categoryLabel    { {}, "Category" };
    juce::Label countLabel       { {}, "Variations" };
    juce::Label seedLabel        { {}, "Seed" };
    juce::Label exportLabel      { {}, "Export Bundle" };
    juce::Label dirLabel         { {}, "Directory" };

    juce::ComboBox categoryCombo;
    juce::Slider countSlider { juce::Slider::LinearHorizontal, juce::Slider::TextEntryBoxPosition::TextBoxRight };
    juce::TextEditor seedEditor;
    juce::TextButton randomizeButton { "Randomize" };
    juce::TextButton forgeButton     { "Forge Variations" };
    juce::Label forgeStatus          { {}, "Ready" };

    juce::TextEditor dirEditor;
    juce::TextButton loopButton { "Export Seamless Loop" };
    juce::TextButton stemsButton { "Export Adaptive Stems" };
    juce::TextButton packButton  { "Export Godot Audio Pack" };
    juce::Label exportStatus      { {}, "Ready" };

    juce::Label uiSoundLabel      { {}, "UI Sounds" };
    juce::TextButton uiSoundButtons[7];
    juce::TextButton uiMuteButton { "Mute UI Sounds" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GameForgeComponent)
};
} // namespace game
} // namespace tcs
