#pragma once

// WO-020: single source of truth for platform capability macros. CMake injects
// TCS_ENABLE_DESKTOP_VST3 on the desktop app; the fallback keeps header-only
// consumers (and the isolated core build) valid when it is not defined.

#ifndef TCS_ENABLE_DESKTOP_VST3
#  define TCS_ENABLE_DESKTOP_VST3 0
#endif

#if defined(__ANDROID__) || defined(ANDROID)
#  define TCS_PLATFORM_IS_ANDROID 1
#else
#  define TCS_PLATFORM_IS_ANDROID 0
#endif

#if defined(_WIN32) || defined(_WIN64)
#  define TCS_AUDIO_BACKEND_NAME "WASAPI"
#elif defined(__APPLE__) && !defined(__IOS__)
#  define TCS_AUDIO_BACKEND_NAME "CoreAudio"
#elif TCS_PLATFORM_IS_ANDROID
#  define TCS_AUDIO_BACKEND_NAME "Oboe"
#else
#  define TCS_AUDIO_BACKEND_NAME "ALSA/JACK"
#endif

// Desktop plug-in hosting never exists on Android or in the isolated core.
#if (TCS_ENABLE_DESKTOP_VST3 == 1) && (TCS_PLATFORM_IS_ANDROID == 0)
#  define TCS_HAS_DESKTOP_VST3 1
#else
#  define TCS_HAS_DESKTOP_VST3 0
#endif

// The core-isolation target defines TCS_CORE_ISOLATION; it must not see UI.
#if (TCS_PLATFORM_IS_ANDROID == 0) && !defined(TCS_CORE_ISOLATION)
#  define TCS_HAS_DESKTOP_UI 1
#else
#  define TCS_HAS_DESKTOP_UI 0
#endif
