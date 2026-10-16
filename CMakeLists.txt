cmake_minimum_required(VERSION 3.22)
project(PolyrhythmModule VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Download JUCE automatically — first build will take a few minutes
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        9.0.4       # Pinned so every build uses the same JUCE. Bump deliberately.
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(PolyrhythmModule
    VERSION                     1.0.0
    COMPANY_NAME                "Polyrhythm"
    PLUGIN_MANUFACTURER_CODE    PLRM          # must be 4 chars, uppercase
    PLUGIN_CODE                 PLRH          # must be 4 chars, unique to your plugin
    FORMATS                     AU VST3
    PRODUCT_NAME                "Polyrhythm_Module"
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            TRUE          # required for the VST3 to expose an event INPUT bus
    NEEDS_MIDI_OUTPUT           TRUE
    IS_MIDI_EFFECT              TRUE          # MIDI Effect: generated MIDI flows directly to the instrument
    VST3_CATEGORIES             "Fx|MIDI"
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD     TRUE          # auto-installs to ~/Library/Audio/Plug-Ins/
    DESCRIPTION                 "Polyrhythm MIDI Generator"
)

juce_generate_juce_header(PolyrhythmModule)

target_sources(PolyrhythmModule
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
)

target_compile_definitions(PolyrhythmModule
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
)

target_link_libraries(PolyrhythmModule
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

#===============================================================================
# Instrument variant of the same plugin.
#
# Logic only offers "Record MIDI to Track Here" in the MIDI FX slot, so the MIDI-effect
# build above stays. This second target loads in the Instrument slot instead, and shows up
# as a routable MIDI source in hosts that support that (Ableton, Bitwig, Reaper). It emits
# MIDI only and leaves its audio output silent.
juce_add_plugin(PolyrhythmModuleInst
    VERSION                     1.0.0
    COMPANY_NAME                "Polyrhythm"
    PLUGIN_MANUFACTURER_CODE    PLRM
    PLUGIN_CODE                 PLRI          # must differ from the MIDI-effect build
    FORMATS                     AU VST3
    PRODUCT_NAME                "Polyrhythm_Module_Inst"
    IS_SYNTH                    TRUE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           TRUE
    IS_MIDI_EFFECT              FALSE
    VST3_CATEGORIES             "Instrument"
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD     TRUE
    DESCRIPTION                 "Polyrhythm MIDI Generator (Instrument)"
)

juce_generate_juce_header(PolyrhythmModuleInst)

target_sources(PolyrhythmModuleInst
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
)

target_compile_definitions(PolyrhythmModuleInst
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
)

target_link_libraries(PolyrhythmModuleInst
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# Scheduler regression tests — not part of the default build.
#   cmake --build build --target SchedulerTest
#   ./build/SchedulerTest_artefacts/SchedulerTest
juce_add_console_app(SchedulerTest PRODUCT_NAME "SchedulerTest")

juce_generate_juce_header(SchedulerTest)

target_sources(SchedulerTest PRIVATE Tests/SchedulerTest.cpp Source/PluginProcessor.cpp)

# The processor pulls in PluginEditor.h for createEditor(); the tests never open a window
target_sources(SchedulerTest PRIVATE Source/PluginEditor.cpp)

target_compile_definitions(SchedulerTest
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1
        JucePlugin_Name="Polyrhythm_Module"
        JucePlugin_IsMidiEffect=1
)

target_link_libraries(SchedulerTest
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
)

set_target_properties(SchedulerTest PROPERTIES EXCLUDE_FROM_ALL TRUE)

# Renders the editor offscreen at several sizes so the layout can be eyeballed without a DAW.
#   cmake --build build --target EditorSnapshot
#   ./build/EditorSnapshot_artefacts/Release/EditorSnapshot <output-dir>
juce_add_console_app(EditorSnapshot PRODUCT_NAME "EditorSnapshot")

juce_generate_juce_header(EditorSnapshot)

target_sources(EditorSnapshot
    PRIVATE
        Tests/EditorSnapshot.cpp
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
)

target_compile_definitions(EditorSnapshot
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1
        JucePlugin_Name="Polyrhythm_Module"
        JucePlugin_IsMidiEffect=1
)

target_link_libraries(EditorSnapshot
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
)

set_target_properties(EditorSnapshot PROPERTIES EXCLUDE_FROM_ALL TRUE)
