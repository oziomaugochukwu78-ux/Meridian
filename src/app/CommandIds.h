#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace meridian::commands
{

enum : juce::CommandID
{
    // File
    dashboard = 0x1000,
    openSession,
    saveSession,
    importAudio,
    bounceMix,

    // Edit
    undo = 0x2000,
    redo,
    separateClip,
    deleteSelection,
    elasticAudio,
    zoomIn,
    zoomOut,

    // Tools and modes
    slipMode = 0x2100,
    gridMode,
    trimTool,
    selectorTool,
    grabberTool,
    smartTool,

    // Track
    newTracks = 0x3000,
    deleteTrack,

    // Transport
    playStop = 0x4000,
    record,
    returnToZero,

    // Setup
    playbackEngine = 0x5000,
    pluginManager,
    toggleRemoteControl,
    remoteControlInfo,

    // Window
    toggleWindow = 0x6000,
    showEditWindow,
    showMixWindow,
};

} // namespace meridian::commands
