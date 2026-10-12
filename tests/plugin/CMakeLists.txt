# A tiny VST3 used by `Meridian --self-test` to verify plug-in hosting.
# Not installed anywhere: the self test loads it straight from the build tree.

juce_add_plugin(MeridianTestGain
    FORMATS VST3
    PRODUCT_NAME "Meridian Test Gain"
    COMPANY_NAME "Meridian Project"
    PLUGIN_MANUFACTURER_CODE Mrdn
    PLUGIN_CODE Mtgn
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(MeridianTestGain PRIVATE TestGainPlugin.cpp)
target_compile_definitions(MeridianTestGain PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(MeridianTestGain
    PRIVATE juce::juce_audio_utils
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
set_target_properties(MeridianTestGain PROPERTIES FOLDER "Meridian/Tests")
