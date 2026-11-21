#include "MidiKeyboardDock.h"

#include <algorithm>
#include <cmath>

MidiKeyboardDock::MidiKeyboardDock()
{
    setOpaque (true);

    keyboardState.addListener (this);
    keyboardComponent.setAvailableRange (36, 84); // C2..C6
    keyboardComponent.setKeyWidth (20.0f);
    addAndMakeVisible (keyboardComponent);

    addAndMakeVisible (titleLabel);
    titleLabel.setText ("MIDI KEYS", juce::dontSendNotification);
    titleLabel.setFont (juce::Font (juce::FontOptions (11.5f, juce::Font::bold)));
    titleLabel.setColour (juce::Label::textColourId, juce::Colour (0xfffbbf24));

    addAndMakeVisible (trackSelector);
    trackSelector.onChange = [this]
    {
        selectedTrackIndex = std::max (0, trackSelector.getSelectedItemIndex());
    };

    addAndMakeVisible (stepRecToggle);
    stepRecToggle.setTooltip ("When enabled, playing keys writes quantized MIDI notes at the playhead and advances");
}

MidiKeyboardDock::~MidiKeyboardDock()
{
    keyboardState.removeListener (this);
    setEdit (nullptr);
}

void MidiKeyboardDock::setEdit (te::Edit* newEdit)
{
    edit = newEdit;
    selectedTrackIndex = 0;
    syncFromEdit();
}

void MidiKeyboardDock::syncFromEdit()
{
    trackSelector.clear (juce::dontSendNotification);

    if (edit != nullptr)
    {
        auto tracks = te::getAudioTracks (*edit);

        for (int i = 0; i < tracks.size(); ++i)
            trackSelector.addItem (juce::String (i + 1) + ". " + tracks[i]->getName(), i + 1);

        if (! tracks.isEmpty())
        {
            selectedTrackIndex = juce::jlimit (0, tracks.size() - 1, selectedTrackIndex);
            trackSelector.setSelectedItemIndex (selectedTrackIndex, juce::dontSendNotification);
        }
    }

    repaint();
}

te::AudioTrack* MidiKeyboardDock::resolveTargetTrack() const
{
    if (edit == nullptr)
        return nullptr;

    auto tracks = te::getAudioTracks (*edit);

    if (juce::isPositiveAndBelow (selectedTrackIndex, tracks.size()))
        return tracks[selectedTrackIndex];

    return tracks.isEmpty() ? nullptr : tracks[0];
}

void MidiKeyboardDock::handleNoteOn (juce::MidiKeyboardState*, int midiChannel, int midiNoteNumber, float velocity)
{
    auto* track = resolveTargetTrack();

    if (track == nullptr || edit == nullptr)
        return;

    const auto msg = juce::MidiMessage::noteOn (midiChannel, midiNoteNumber, velocity);
    track->injectLiveMidiMessage (msg, te::MPESourceID{});

    if (stepRecToggle.getToggleState())
    {
        const int velInt = juce::jlimit (1, 127, static_cast<int> (std::round (velocity * 127.0f)));
        const double stepBeats = getStepBeatsCallback != nullptr
                                     ? std::max (0.125, getStepBeatsCallback())
                                     : 0.5;

        if (recordStepNote (midiNoteNumber, velInt, stepBeats) && onNotesRecorded != nullptr)
            onNotesRecorded();
    }
}

void MidiKeyboardDock::handleNoteOff (juce::MidiKeyboardState*, int midiChannel, int midiNoteNumber, float velocity)
{
    auto* track = resolveTargetTrack();

    if (track == nullptr || edit == nullptr)
        return;

    const auto msg = juce::MidiMessage::noteOff (midiChannel, midiNoteNumber, velocity);
    track->injectLiveMidiMessage (msg, te::MPESourceID{});
}

bool MidiKeyboardDock::recordStepNote (int midiNoteNumber, int velocity, double stepBeats)
{
    auto* track = resolveTargetTrack();

    if (track == nullptr || edit == nullptr)
        return false;

    const double step = std::max (0.125, stepBeats);
    const auto posTime = edit->getTransport().getPosition();
    const double currentBeat = std::max (0.0, edit->tempoSequence.toBeats (posTime).inBeats());
    const double noteEndBeat = currentBeat + step;

    auto& um = edit->getUndoManager();
    um.beginNewTransaction ("Step Record MIDI Note");

    // Target the clip under the playhead. A clip that merely starts before
    // the playhead must NOT be stretched across the timeline to hold the
    // note — step-recording late in the arrangement used to grow an early
    // 4-beat clip to cover the note instead of starting a fresh clip.
    te::MidiClip* targetClip = nullptr;

    for (auto* c : track->getClips())
    {
        if (auto* mc = dynamic_cast<te::MidiClip*> (c))
        {
            if (mc->getPosition().getStart() <= posTime && posTime < mc->getPosition().getEnd())
            {
                targetClip = mc;
                break;
            }
        }
    }

    // A fresh clip starts at the bar containing the playhead, not at 0.
    const double barStartBeat = std::floor (currentBeat / 4.0) * 4.0;
    const double minClipEndBeat = std::max (barStartBeat + 4.0, std::ceil (noteEndBeat / 4.0) * 4.0);
    const auto minClipEndTime = edit->tempoSequence.toTime (tracktion::BeatPosition::fromBeats (minClipEndBeat));

    if (targetClip == nullptr)
    {
        const tracktion::TimeRange clipRange {
            edit->tempoSequence.toTime (tracktion::BeatPosition::fromBeats (barStartBeat)),
            minClipEndTime
        };

        targetClip = dynamic_cast<te::MidiClip*> (
            track->insertNewClip (te::TrackItem::Type::midi, "Step Rec", clipRange, nullptr));
    }

    if (targetClip == nullptr)
        return false;

    if (targetClip->getPosition().getEnd() < minClipEndTime)
        targetClip->setEnd (minClipEndTime, true);

    const double clipStartBeat = edit->tempoSequence.toBeats (targetClip->getPosition().getStart()).inBeats();
    const double relStartBeat  = std::max (0.0, currentBeat - clipStartBeat);

    targetClip->getSequence().addNote (
        juce::jlimit (0, 127, midiNoteNumber),
        tracktion::BeatPosition::fromBeats (relStartBeat),
        tracktion::BeatDuration::fromBeats (step),
        juce::jlimit (1, 127, velocity),
        0,
        &um);

    edit->getTransport().setPosition (
        edit->tempoSequence.toTime (tracktion::BeatPosition::fromBeats (noteEndBeat)));

    return true;
}

void MidiKeyboardDock::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff14171f));
    g.setColour (juce::Colour (0xff2b313e));
    g.drawHorizontalLine (0, 0.0f, static_cast<float> (getWidth()));
}

void MidiKeyboardDock::resized()
{
    auto area = getLocalBounds().reduced (4, 3);
    auto left = area.removeFromLeft (controlStripWidth);

    titleLabel.setBounds (left.removeFromTop (16));
    left.removeFromTop (2);
    trackSelector.setBounds (left.removeFromTop (22));
    left.removeFromTop (2);
    stepRecToggle.setBounds (left.removeFromTop (22));

    area.removeFromLeft (6);
    keyboardComponent.setBounds (area);
}
