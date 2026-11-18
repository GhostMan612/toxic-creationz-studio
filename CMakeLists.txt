cmake_minimum_required(VERSION 3.22)
project(AIDAW VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable multi-processor compilation on MSVC
if(MSVC)
    add_compile_options(/MP /bigobj)
endif()

# 1. Add JUCE
add_subdirectory(third_party/JUCE)

# 2. Register Tracktion Engine modules
juce_add_modules(
    third_party/tracktion_engine/modules/tracktion_core
    third_party/tracktion_engine/modules/tracktion_graph
    third_party/tracktion_engine/modules/tracktion_engine
)

# 3. Define the Desktop DAW Executable
juce_add_gui_app(AIDAW
    PRODUCT_NAME "AI DAW"
    COMPANY_NAME "Toxic Creationz Studio"
    BUNDLE_ID "com.toxiccreationz.aidaw"
)

target_sources(AIDAW PRIVATE
    src/Main.cpp
    src/MainComponent.h
    src/MainComponent.cpp
    src/ArrangementView.h
    src/ArrangementView.cpp
    src/PianoRollComponent.h
    src/PianoRollComponent.cpp
    src/MixerComponent.h
    src/MixerComponent.cpp
    src/CvRackPlugins.h
    src/ModularRackComponent.h
    src/ModularRackComponent.cpp
    src/RiffLibrary.h
    src/RiffLibrary.cpp
    src/BgmGeneratorComponent.h
    src/BgmGeneratorComponent.cpp
)

# 4. Preprocessor flags: enable VST3 hosting, disable unused bloat
target_compile_definitions(AIDAW PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:AIDAW,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:AIDAW,JUCE_VERSION>"
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_VST=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    TRACKTION_ENABLE_TIMESTRETCH_SOUNDTOUCH=1
)

# 5. Link JUCE & Tracktion modules
target_link_libraries(AIDAW PRIVATE
    tracktion_core
    tracktion_graph
    tracktion_engine

    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)
