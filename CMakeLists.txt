cmake_minimum_required(VERSION 3.22)
project(AIDAW VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

#===============================================================================
# WO-020: Platform options. Desktop defaults stay ON so the Windows build path
# (AIDAW.exe, package-windows.ps1, windows-2022 CI) is byte-for-byte unchanged.
#===============================================================================
option(TCS_ENABLE_DESKTOP_VST3 "Enable desktop VST3 plugin hosting" ON)
option(TCS_BUILD_DESKTOP_APP "Build desktop GUI DAW executable" ON)
option(TCS_BUILD_CORE_ISOLATION_CHECK "Build headless/no-VST3 core boundary verification library" ON)
option(TCS_ENABLE_ASAN "Instrument MSVC builds with AddressSanitizer" OFF)

if(ANDROID)
    set(TCS_ENABLE_DESKTOP_VST3 OFF CACHE BOOL "Android uses Oboe, not desktop VST3" FORCE)
    set(TCS_BUILD_DESKTOP_APP OFF CACHE BOOL "Android builds tcs_core only" FORCE)
endif()

# Oboe is a property of the platform-neutral core, not of the desktop app.
# Because ANDROID forces TCS_BUILD_DESKTOP_APP OFF, an AIDAW-scoped define can
# never fire on Android; apply it to tcs_core so the Oboe backend is real.
if(ANDROID)
    set(TCS_ANDROID_OBOE_DEFINITIONS JUCE_USE_ANDROID_OBOE=1 JUCE_PLUGINHOST_VST3=0)
else()
    set(TCS_ANDROID_OBOE_DEFINITIONS "")
endif()

#-------------------------------------------------------------------------------
# Compiler flags: MSVC-only switches stay inside if(MSVC); GCC/Clang get the
# equivalent warnings + PIC in the non-MSVC branch. Flags are applied per target
# so the vendored JUCE/Tracktion trees keep their own warning policy.
#-------------------------------------------------------------------------------
if(MSVC)
    set(TCS_PLATFORM_COMPILE_OPTIONS /MP /bigobj /W4 /utf-8)
    if(TCS_ENABLE_ASAN)
        list(APPEND TCS_PLATFORM_COMPILE_OPTIONS /fsanitize=address)
    endif()
else()
    set(TCS_PLATFORM_COMPILE_OPTIONS -Wall -Wextra -Wno-unused-parameter)
    if(UNIX)
        list(APPEND TCS_PLATFORM_COMPILE_OPTIONS -fPIC)
    endif()
endif()

# 1. Add JUCE
add_subdirectory(third_party/JUCE)

# 2. Register Tracktion Engine modules
juce_add_modules(
    third_party/tracktion_engine/modules/tracktion_core
    third_party/tracktion_engine/modules/tracktion_graph
    third_party/tracktion_engine/modules/tracktion_engine
)

#-------------------------------------------------------------------------------
# 3. Platform-neutral core library. No desktop UI, no StudioPluginWindow, no
#    VST3 host: this is the layer an Android/Oboe client links against.
#-------------------------------------------------------------------------------
add_library(tcs_core STATIC
    src/RiffLibrary.cpp
    src/RecordingManager.cpp
    src/MixdownExporter.cpp
    src/PluginPresetManager.cpp
    src/AudioToMidiTranscriber.cpp
)
target_include_directories(tcs_core PUBLIC src)
target_compile_options(tcs_core PRIVATE ${TCS_PLATFORM_COMPILE_OPTIONS})
target_compile_definitions(tcs_core PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=0
    TRACKTION_ENABLE_TIMESTRETCH_SOUNDTOUCH=1
    TCS_ENABLE_DESKTOP_VST3=0
    ${TCS_ANDROID_OBOE_DEFINITIONS}
)
target_link_libraries(tcs_core PUBLIC
    tracktion_core
    tracktion_graph
    tracktion_engine
    juce::juce_audio_basics
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_core
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events
    juce::juce_recommended_config_flags
)
if(UNIX AND NOT APPLE AND NOT ANDROID)
    # GCC on Linux needs libatomic for 64-bit atomics emitted by tracktion/JUCE.
    target_link_libraries(tcs_core PUBLIC atomic)
endif()

#-------------------------------------------------------------------------------
# 4. Compile-time core-boundary proof. Built with VST3 off and no desktop UI.
#-------------------------------------------------------------------------------
if(TCS_BUILD_CORE_ISOLATION_CHECK)
    add_library(tcs_core_isolation STATIC src/CoreIsolationCheck.cpp)
    target_include_directories(tcs_core_isolation PUBLIC src)
    target_compile_options(tcs_core_isolation PRIVATE ${TCS_PLATFORM_COMPILE_OPTIONS})
    target_compile_definitions(tcs_core_isolation PRIVATE
        TCS_ENABLE_DESKTOP_VST3=0
        TCS_CORE_ISOLATION=1
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_PLUGINHOST_VST3=0
    )
    target_link_libraries(tcs_core_isolation PUBLIC tcs_core)
endif()

#-------------------------------------------------------------------------------
# 5. Desktop GUI app. Skipped entirely on Android / when TCS_BUILD_DESKTOP_APP=OFF.
#-------------------------------------------------------------------------------
if(TCS_BUILD_DESKTOP_APP)
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
        src/HardwareRackPlugins.h
        src/SidechainCompressorPlugin.h
        src/VocalChannelPlugin.h
        src/ModularRackComponent.h
        src/ModularRackComponent.cpp
        src/RiffLibrary.h
        src/RiffLibrary.cpp
        src/BgmGeneratorComponent.h
        src/BgmGeneratorComponent.cpp
        src/DawIpcServer.h
        src/DawIpcServer.cpp
        src/WaveformEditorComponent.h
        src/WaveformEditorComponent.cpp
        src/MixdownExporter.h
        src/MixdownExporter.cpp
        src/MidiKeyboardDock.h
        src/MidiKeyboardDock.cpp
        src/ScriptPrompterComponent.h
        src/ScriptPrompterComponent.cpp
        src/RecordingManager.h
        src/RecordingManager.cpp
        src/StudioLookAndFeel.h
        src/StudioPluginWindow.h
        src/HardwarePluginEditors.h
        src/HardwarePluginEditors.cpp
        src/PluginPresetManager.h
        src/PluginPresetManager.cpp
        src/AutomationManager.h
        src/AutomationManager.cpp
        src/AudioToMidiTranscriber.h
        src/AudioToMidiTranscriber.cpp
        src/PlatformConfig.h
        src/CoreIsolationCheck.h
        src/CoreIsolationCheck.cpp
    )

    target_compile_options(AIDAW PRIVATE ${TCS_PLATFORM_COMPILE_OPTIONS})

    target_compile_definitions(AIDAW PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:AIDAW,JUCE_PRODUCT_NAME>"
        JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:AIDAW,JUCE_VERSION>"
        JUCE_PLUGINHOST_VST3=$<BOOL:${TCS_ENABLE_DESKTOP_VST3}>
        JUCE_PLUGINHOST_VST=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        TRACKTION_ENABLE_TIMESTRETCH_SOUNDTOUCH=1
        TCS_ENABLE_DESKTOP_VST3=$<BOOL:${TCS_ENABLE_DESKTOP_VST3}>
    )

    #--- Platform audio backends -------------------------------------------------
    if(WIN32)
        target_compile_definitions(AIDAW PRIVATE JUCE_WASAPI=1 JUCE_DIRECTSOUND=1)
    elseif(APPLE AND NOT IOS)
        if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
            set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "macOS deployment target")
        endif()
        target_compile_definitions(AIDAW PRIVATE JUCE_COREAUDIO=1)
    elseif(UNIX AND NOT APPLE AND NOT ANDROID)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(TCS_LINUX_DEPS QUIET IMPORTED_TARGET
                alsa freetype2 x11 xrandr xinerama xcursor gl)
        endif()
        target_compile_definitions(AIDAW PRIVATE JUCE_ALSA=1 JUCE_JACK=1)
        target_link_libraries(AIDAW PRIVATE atomic)
    endif()

    target_link_libraries(AIDAW PRIVATE
        tcs_core

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

    if(TCS_LINUX_DEPS_FOUND)
        target_link_libraries(AIDAW PRIVATE PkgConfig::TCS_LINUX_DEPS)
    endif()
endif()
