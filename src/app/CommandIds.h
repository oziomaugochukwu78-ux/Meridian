#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace meridian::commands
{

// JUCE reserves 0x1000-0x1fff for StandardApplicationCommandIDs (quit = 0x1001,
// del, copy, paste, ...). Meridian's ids start at 0x10000 so none can collide:
// an overlap made "Open Session" share an id with Quit.
enum : juce::CommandID
{
    // File
    dashboard = 0x11000,
    openSession,
    saveSession,
    importAudio,
    bounceMix,
    saveSessionAs,

    // Edit
    undo = 0x12000,
    redo,
    separateClip,
    deleteSelection,
    elasticAudio,
    zoomIn,
    zoomOut,

    // Tools and modes
    slipMode = 0x12100,
    gridMode,
    trimTool,
    selectorTool,
    grabberTool,
    smartTool,
    pencilTool,

    // Track
    newTracks = 0x13000,
    deleteTrack,

    // Transport
    playStop = 0x14000,
    record,
    returnToZero,
    loopPlayback,
    clickToggle,
    countInOff,
    countIn1Bar,
    countIn2Bars,
    addMarker,

    // Setup
    playbackEngine = 0x15000,
    pluginManager,
    toggleRemoteControl,
    remoteControlInfo,

    // Window
    toggleWindow = 0x16000,
    showEditWindow,
    showMixWindow,
};

} // namespace meridian::commands
