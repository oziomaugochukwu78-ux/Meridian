#pragma once

#include "model/Session.h"

#include <map>

namespace meridian
{

/** Applies mix groups (model/Groups.h): when a track's fader, mute, solo,
    record arm or automation mode changes, the other members of its active
    mix groups follow. Faders follow relatively (the same change in dB, held
    at the fader's ends); the switches and the automation mode take the same
    value. The followers' changes go into the same undo step as the move that
    caused them; undo and redo restore every member on their own, so nothing
    is applied twice.

    Message thread only. Watches whichever session it is attached to. */
class GroupLinker final : private juce::ValueTree::Listener
{
public:
    GroupLinker() = default;
    ~GroupLinker() override;

    void attach (Session*);

private:
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void remember (const juce::ValueTree& track);

    Session* session = nullptr;
    std::map<int, double> faderDb;  ///< each track's last fader value, to turn a move into a change in dB
    bool following = false;         ///< applying followers' changes: they do not lead in turn
};

} // namespace meridian
