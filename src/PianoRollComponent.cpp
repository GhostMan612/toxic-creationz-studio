#include "PianoRollComponent.h"

#include <algorithm>
#include <cmath>

PianoRollComponent::PianoRollComponent()
{
    setOpaque (true);

    addAndMakeVisible (headerLabel);
    headerLabel.setText ("PIANO ROLL  |  Left-Click: Paint  |  Drag Edge: Resize  |  Right-Click: Delete",
                         juce::dontSendNotification);
    headerLabel.setColour (juce::Label::textColourId, juce::Colour (0xffcbd5e1));
    headerLabel.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));

    addAndMakeVisible (snapSelector);
    snapSelector.addItem ("Snap: 1/4",  1);
    snapSelector.addItem ("Snap: 1/8",  2);
    snapSelector.addItem ("Snap: 1/16", 3);
    snapSelector.addItem ("Snap: 1/32", 4);
    snapSelector.setSelectedId (3, juce::dontSendNotification);

    snapSelector.onChange = [this]
    {
        switch (snapSelector.getSelectedId())
        {
            case 1: setSnapDivision (1.0);    break;
            case 2: setSnapDivision (0.5);    break;
            case 3: setSnapDivision (0.25);   break;
            case 4: setSnapDivision (0.125);  break;
            default: break;
        }
    };
}

void PianoRollComponent::setTarget (te::Edit* targetEdit, tracktion::EditItemID clipID)
{
    edit = targetEdit;
    targetClipID = clipID;
    horizontalScrollBeats = 0.0;
    dragMode = DragMode::none;
    activeNoteState = {};
    repaint();
}

void PianoRollComponent::setSnapDivision (double beatsPerStep)
{
    snapBeats = std::max (0.0625, beatsPerStep);
    repaint();
}

void PianoRollComponent::setPixelsPerBeat (float newPixelsPerBeat)
{
    pixelsPerBeat = juce::jlimit (36.0f, 160.0f, newPixelsPerBeat);
    repaint();
}

void PianoRollComponent::zoomIn()
{
    setPixelsPerBeat (pixelsPerBeat * 1.25f);
}

void PianoRollComponent::zoomOut()
{
    setPixelsPerBeat (pixelsPerBeat / 1.25f);
}

te::MidiClip* PianoRollComponent::resolveClip() const
{
    if (edit == nullptr || ! targetClipID.isValid())
        return nullptr;

    for (auto* track : te::getAudioTracks (*edit))
        for (auto* clip : track->getClips())
            if (clip->itemID == targetClipID)
                return dynamic_cast<te::MidiClip*> (clip);

    return nullptr;
}

te::MidiNote* PianoRollComponent::resolveActiveNote (te::MidiList& seq) const
{
    if (! activeNoteState.isValid())
        return nullptr;

    for (auto* note : seq.getNotes())
        if (note->state == activeNoteState)
            return note;

    return nullptr;
}

double PianoRollComponent::snapBeat (double rawBeat) const
{
    return std::max (0.0, std::floor (rawBeat / snapBeats) * snapBeats);
}

bool PianoRollComponent::isBlackKey (int midiPitch)
{
    const int mod = midiPitch % 12;
    return mod == 1 || mod == 3 || mod == 6 || mod == 8 || mod == 10;
}

juce::String PianoRollComponent::getNoteLabel (int midiPitch)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const int octave = (midiPitch / 12) - 1;
    return juce::String (names[midiPitch % 12]) + juce::String (octave);
}

int PianoRollComponent::getPitchAtY (int y) const
{
    const int numRows = highestPitch - lowestPitch + 1;
    const float gridHeight = static_cast<float> (std::max (1, getHeight() - topBarHeight));
    const float rowHeight = gridHeight / static_cast<float> (numRows);

    const int rowFromTop = static_cast<int> ((y - topBarHeight) / rowHeight);
    return juce::jlimit (lowestPitch, highestPitch, highestPitch - rowFromTop);
}

double PianoRollComponent::getBeatAtX (int x) const
{
    return std::max (0.0, horizontalScrollBeats + static_cast<double> (x - pianoKeyWidth) / pixelsPerBeat);
}

juce::Rectangle<int> PianoRollComponent::getNoteRect (const te::MidiNote& note) const
{
    const int numRows = highestPitch - lowestPitch + 1;
    const float gridHeight = static_cast<float> (std::max (1, getHeight() - topBarHeight));
    const float rowHeight = gridHeight / static_cast<float> (numRows);

    const int pitch = note.getNoteNumber();
    const int rowFromTop = highestPitch - pitch;

    const int x = pianoKeyWidth + static_cast<int> (std::round ((note.getStartBeat().inBeats() - horizontalScrollBeats) * pixelsPerBeat));
    const int w = std::max (8, static_cast<int> (std::round (note.getLengthBeats().inBeats() * pixelsPerBeat)));
    const int y = topBarHeight + static_cast<int> (rowFromTop * rowHeight);
    const int h = std::max (4, static_cast<int> (std::ceil (rowHeight)) - 1);

    return { x, y, w, h };
}

void PianoRollComponent::resized()
{
    auto topBar = getLocalBounds().removeFromTop (topBarHeight).reduced (6, 3);
    snapSelector.setBounds (topBar.removeFromRight (115));
    headerLabel.setBounds (topBar);
}

void PianoRollComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff121419));

    const auto bounds = getLocalBounds();
    g.setColour (juce::Colour (0xff1d2028));
    g.fillRect (0, 0, bounds.getWidth(), topBarHeight);

    auto* clip = resolveClip();

    if (clip == nullptr || edit == nullptr)
    {
        g.setColour (juce::Colour (0xff94a3b8));
        g.setFont (juce::Font (juce::FontOptions (14.0f)));
        g.drawText ("Select a MIDI clip above (or double-click an empty track lane to create one).",
                    bounds.withTrimmedTop (topBarHeight), juce::Justification::centred);
        return;
    }

    const int numRows = highestPitch - lowestPitch + 1;
    const float gridHeight = static_cast<float> (std::max (1, bounds.getHeight() - topBarHeight));
    const float rowHeight = gridHeight / static_cast<float> (numRows);

    // Pitch rows + left piano keyboard
    for (int pitch = highestPitch; pitch >= lowestPitch; --pitch)
    {
        const int rowFromTop = highestPitch - pitch;
        const int y = topBarHeight + static_cast<int> (rowFromTop * rowHeight);
        const int h = std::max (2, static_cast<int> (std::ceil (rowHeight)));
        const bool black = isBlackKey (pitch);

        g.setColour (black ? juce::Colour (0xff151820) : juce::Colour (0xff1a1e27));
        g.fillRect (pianoKeyWidth, y, bounds.getWidth() - pianoKeyWidth, h);

        g.setColour (pitch % 12 == 0 ? juce::Colour (0xff334155) : juce::Colour (0xff222632));
        g.drawHorizontalLine (y + h - 1, static_cast<float> (pianoKeyWidth),
                              static_cast<float> (bounds.getWidth()));

        g.setColour (black ? juce::Colour (0xff1e222b) : juce::Colour (0xffe2e8f0));
        g.fillRect (0, y, pianoKeyWidth - 1, h - 1);

        if (pitch % 12 == 0 || rowHeight >= 11.0f)
        {
            g.setColour (black ? juce::Colours::lightgrey : juce::Colours::black);
            g.setFont (juce::Font (juce::FontOptions (10.0f)));
            g.drawText (getNoteLabel (pitch), 4, y, pianoKeyWidth - 8, h, juce::Justification::centredLeft);
        }
    }

    // Vertical snap / beat grid & MIDI notes clipped to note grid area
    juce::Graphics::ScopedSaveState gridClip (g);
    g.reduceClipRegion (pianoKeyWidth, topBarHeight,
                        bounds.getWidth() - pianoKeyWidth, bounds.getHeight() - topBarHeight);

    const double clipStartBeat = edit->tempoSequence.toBeats (clip->getPosition().getStart()).inBeats();
    const double clipEndBeat   = edit->tempoSequence.toBeats (clip->getPosition().getEnd()).inBeats();
    const double clipBeats     = std::max (4.0, clipEndBeat - clipStartBeat);
    const int totalSteps       = static_cast<int> (std::ceil (clipBeats / snapBeats));

    for (int step = 0; step <= totalSteps; ++step)
    {
        const double beat = step * snapBeats;
        const int x = pianoKeyWidth + static_cast<int> (std::round ((beat - horizontalScrollBeats) * pixelsPerBeat));
        if (x > bounds.getWidth())
            break;
        if (x < pianoKeyWidth)
            continue;

        const bool isBar  = std::fmod (beat, 4.0) < 1e-4;
        const bool isBeat = std::fmod (beat, 1.0) < 1e-4;

        g.setColour (isBar ? juce::Colour (0xff475569)
                           : (isBeat ? juce::Colour (0xff2e3442) : juce::Colour (0xff20242f)));
        g.drawVerticalLine (x, static_cast<float> (topBarHeight), static_cast<float> (bounds.getHeight()));
    }

    // MIDI notes
    const auto& notes = clip->getSequence().getNotes();

    for (int i = 0; i < notes.size(); ++i)
    {
        const auto* note = notes[i];

        if (note->getNoteNumber() < lowestPitch || note->getNoteNumber() > highestPitch)
            continue;

        const auto rect = getNoteRect (*note);
        const bool isActive = (note->state == activeNoteState);

        g.setColour (isActive ? juce::Colour (0xff60a5fa) : juce::Colour (0xff38bdf8));
        g.fillRoundedRectangle (rect.toFloat(), 3.0f);

        g.setColour (juce::Colour (0xff0f172a));
        g.drawRoundedRectangle (rect.toFloat(), 3.0f, 1.0f);

        g.setColour (juce::Colours::white.withAlpha (0.8f));
        g.fillRect (rect.getRight() - 3, rect.getY() + 2, 2, std::max (2, rect.getHeight() - 4));
    }
}

void PianoRollComponent::mouseDown (const juce::MouseEvent& e)
{
    auto* clip = resolveClip();

    if (clip == nullptr || edit == nullptr || e.y < topBarHeight || e.x < pianoKeyWidth)
        return;

    auto& seq = clip->getSequence();
    auto& um  = edit->getUndoManager();
    const auto& notes = seq.getNotes();

    // Hit-test existing notes (top-to-bottom)
    for (int i = notes.size() - 1; i >= 0; --i)
    {
        auto* note = notes[i];
        const auto rect = getNoteRect (*note);

        if (rect.contains (e.getPosition()))
        {
            if (e.mods.isRightButtonDown())
            {
                um.beginNewTransaction ("Delete Note");
                seq.removeNote (*note, &um);
                activeNoteState = {};
                repaint();

                if (onSequenceChanged != nullptr)
                    onSequenceChanged();

                return;
            }

            activeNoteState = note->state;
            initialNoteStartBeat  = note->getStartBeat().inBeats();
            initialNoteLengthBeats = note->getLengthBeats().inBeats();
            lastNoteLengthBeats   = initialNoteLengthBeats;

            if (e.x >= rect.getRight() - 7)
            {
                dragMode = DragMode::resizeNote;
                um.beginNewTransaction ("Resize Note");
            }
            else
            {
                dragMode = DragMode::moveNote;
                dragStartBeatOffset = getBeatAtX (e.x) - initialNoteStartBeat;
                um.beginNewTransaction ("Move Note");
            }

            repaint();
            return;
        }
    }

    // Left-click on empty cell -> FL-style paint
    if (e.mods.isLeftButtonDown())
    {
        const double startBeat = snapBeat (getBeatAtX (e.x));
        const int pitch = getPitchAtY (e.y);
        const double lengthBeats = std::max (snapBeats, lastNoteLengthBeats);

        um.beginNewTransaction ("Paint Note");

        if (auto* added = seq.addNote (pitch,
                                       tracktion::BeatPosition::fromBeats (startBeat),
                                       tracktion::BeatDuration::fromBeats (lengthBeats),
                                       110, 0, &um))
        {
            // Track the new note by its stable ValueTree identity so an
            // immediate drag stays locked to it.
            activeNoteState        = added->state;
            initialNoteStartBeat   = startBeat;
            initialNoteLengthBeats = lengthBeats;
            dragStartBeatOffset    = 0.0;
            dragMode               = DragMode::moveNote;
        }

        repaint();

        if (onSequenceChanged != nullptr)
            onSequenceChanged();
    }
}

void PianoRollComponent::mouseDrag (const juce::MouseEvent& e)
{
    auto* clip = resolveClip();

    if (clip == nullptr || edit == nullptr || dragMode == DragMode::none)
        return;

    auto& seq = clip->getSequence();
    auto& um  = edit->getUndoManager();

    auto* note = resolveActiveNote (seq);

    if (note == nullptr)
        return;

    const double pointerBeat = getBeatAtX (e.x);

    if (dragMode == DragMode::resizeNote)
    {
        const double rawEndBeat = std::max (initialNoteStartBeat + snapBeats,
                                            std::round (pointerBeat / snapBeats) * snapBeats);
        const double newLen = std::max (snapBeats, rawEndBeat - initialNoteStartBeat);
        lastNoteLengthBeats = newLen;

        note->setStartAndLength (tracktion::BeatPosition::fromBeats (initialNoteStartBeat),
                                 tracktion::BeatDuration::fromBeats (newLen),
                                 &um);
    }
    else if (dragMode == DragMode::moveNote)
    {
        const double newStart = snapBeat (pointerBeat - dragStartBeatOffset);
        const int newPitch = getPitchAtY (e.y);

        note->setStartAndLength (tracktion::BeatPosition::fromBeats (newStart),
                                 tracktion::BeatDuration::fromBeats (initialNoteLengthBeats),
                                 &um);

        if (note->getNoteNumber() != newPitch)
            note->setNoteNumber (newPitch, &um);
    }

    repaint();

    if (onSequenceChanged != nullptr)
        onSequenceChanged();
}

void PianoRollComponent::mouseUp (const juce::MouseEvent&)
{
    dragMode = DragMode::none;
    activeNoteState = {};
}

void PianoRollComponent::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (e.mods.isCtrlDown())
    {
        if (wheel.deltaY > 0.0f) zoomIn();
        else if (wheel.deltaY < 0.0f) zoomOut();
        return;
    }

    if (e.mods.isShiftDown() || std::abs (wheel.deltaX) > 0.0f)
    {
        const double delta = (std::abs (wheel.deltaX) > 0.0f) ? -wheel.deltaX : -wheel.deltaY;
        horizontalScrollBeats = juce::jlimit (0.0, 64.0, horizontalScrollBeats + delta * 1.5);
        repaint();
    }
}
