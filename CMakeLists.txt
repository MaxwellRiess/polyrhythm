cmake_minimum_required(VERSION 3.22)
project(PolyrhythmModule VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Download JUCE automatically — first build will take a few minutes
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        master      # JUCE 8+ — required for macOS 15 / Xcode 17 compatibility
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
    NEEDS_MIDI_INPUT            FALSE
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
