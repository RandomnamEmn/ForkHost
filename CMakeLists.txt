cmake_minimum_required(VERSION 3.22)
project(LightHostReforge VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(JUCE CONFIG REQUIRED)

# VST2 hosting is opt-in because JUCE requires the separately-supplied VST2 SDK.
# Do not vendor or distribute Steinberg's VST2 SDK as part of this repository.
option(LIGHTHOST_ENABLE_VST2_HOST "Enable local VST2 plug-in hosting" OFF)
set(LIGHTHOST_VST2_SDK_PATH "" CACHE PATH
    "Path to a locally available VST2 SDK (must contain pluginterfaces/vst2.x headers)")

if(LIGHTHOST_ENABLE_VST2_HOST)
    if(NOT LIGHTHOST_VST2_SDK_PATH)
        message(FATAL_ERROR
            "LIGHTHOST_ENABLE_VST2_HOST=ON requires -DLIGHTHOST_VST2_SDK_PATH=<path to VST2 SDK>")
    endif()

    if(NOT EXISTS "${LIGHTHOST_VST2_SDK_PATH}/pluginterfaces/vst2.x/aeffect.h")
        message(FATAL_ERROR
            "VST2 SDK headers not found under '${LIGHTHOST_VST2_SDK_PATH}'. "
            "Expected pluginterfaces/vst2.x/aeffect.h")
    endif()

    if(NOT COMMAND juce_set_vst2_sdk_path)
        message(FATAL_ERROR
            "This JUCE package does not expose juce_set_vst2_sdk_path(); "
            "use a JUCE CMake package with VST2 host support")
    endif()

    juce_set_vst2_sdk_path("${LIGHTHOST_VST2_SDK_PATH}")
    set(LIGHTHOST_PLUGINHOST_VST 1)
else()
    set(LIGHTHOST_PLUGINHOST_VST 0)
endif()

#==============================================================================
# JUCE VST3PluginFormat patch (optional): accept "AudioEffect" category (WaveShell)
#==============================================================================
# WaveShell VST3 uses "AudioEffect" as its class category instead of the
# standard kVstAudioEffectClass ("Audio Module Class"). JUCE 8 only accepts
# the standard category, so WaveShell sub-plugins are invisible.
#
# This patch adds "AudioEffect" as an accepted category at all three filter
# locations in VST3PluginFormat.cpp. The REPLACE is idempotent — if the file
# is already patched, no changes.
#
# The path is auto-detected from JUCE_MODULES_DIR when available (set by
# JUCEConfig.cmake). Override with -DJUCE_VST3_FORMAT_SRC=<path> or set
# to empty to skip patching.
#==============================================================================

# Auto-detect from JUCE's module directory
if(JUCE_MODULES_DIR AND EXISTS "${JUCE_MODULES_DIR}/juce_audio_processors/format_types/juce_VST3PluginFormat.cpp")
    set(_JUCE_VST3_DEFAULT "${JUCE_MODULES_DIR}/juce_audio_processors/format_types/juce_VST3PluginFormat.cpp")
else()
    set(_JUCE_VST3_DEFAULT "")
endif()
set(JUCE_VST3_FORMAT_SRC "${_JUCE_VST3_DEFAULT}"
    CACHE FILEPATH "Full path to VST3PluginFormat.cpp for WaveShell patch (empty = skip)")

if(JUCE_VST3_FORMAT_SRC)
    if(NOT EXISTS "${JUCE_VST3_FORMAT_SRC}")
        message(WARNING "JUCE_VST3_FORMAT_SRC set to non-existent path: ${JUCE_VST3_FORMAT_SRC}. Skipping WaveShell patch.")
    else()
        file(READ "${JUCE_VST3_FORMAT_SRC}" JUCE_VST3_CONTENT)

        # Patch 1: moduleinfo.json fast path (c.category string comparison)
        string(REPLACE
            "if (c.category != kVstAudioEffectClass)"
            "if (c.category != kVstAudioEffectClass && c.category != \"AudioEffect\")"
            JUCE_VST3_PATCHED "${JUCE_VST3_CONTENT}")

        # Patch 2: DLL slow path + instance creation (std::strcmp — matches 2 locations)
        string(REPLACE
            "if (std::strcmp (info.category, kVstAudioEffectClass) != 0)"
            "if (std::strcmp (info.category, kVstAudioEffectClass) != 0 && std::strcmp (info.category, \"AudioEffect\") != 0)"
            JUCE_VST3_PATCHED "${JUCE_VST3_PATCHED}")

        if(NOT JUCE_VST3_CONTENT STREQUAL JUCE_VST3_PATCHED)
            message(STATUS "Patching VST3PluginFormat.cpp to accept \"AudioEffect\" category (WaveShell)")
            file(WRITE "${JUCE_VST3_FORMAT_SRC}" "${JUCE_VST3_PATCHED}")
        endif()
    endif()
else()
    message(STATUS "JUCE_VST3_FORMAT_SRC not found — skipping WaveShell VST3 patch")
endif()

# JUCE's audio device selector hard-codes grey for empty MIDI/device lists,
# which bypasses the host's selected text color. Make those labels follow the
# ListBox text color, while keeping the patch local to the JUCE source used to
# build this host.
if(JUCE_MODULES_DIR AND EXISTS "${JUCE_MODULES_DIR}/juce_audio_utils/gui/juce_AudioDeviceSelectorComponent.cpp")
    set(_LIGHTHOST_JUCE_DEVICE_SELECTOR
        "${JUCE_MODULES_DIR}/juce_audio_utils/gui/juce_AudioDeviceSelectorComponent.cpp")
    file(READ "${_LIGHTHOST_JUCE_DEVICE_SELECTOR}" _LIGHTHOST_JUCE_DEVICE_SELECTOR_CONTENT)
    string(REPLACE
        "g.setColour (Colours::grey);"
        "g.setColour (findColour (ListBox::textColourId));"
        _LIGHTHOST_JUCE_DEVICE_SELECTOR_PATCHED
        "${_LIGHTHOST_JUCE_DEVICE_SELECTOR_CONTENT}")
    if(NOT _LIGHTHOST_JUCE_DEVICE_SELECTOR_CONTENT STREQUAL _LIGHTHOST_JUCE_DEVICE_SELECTOR_PATCHED)
        message(STATUS "Patching JUCE audio-device empty-list text to use the host theme")
        file(WRITE "${_LIGHTHOST_JUCE_DEVICE_SELECTOR}" "${_LIGHTHOST_JUCE_DEVICE_SELECTOR_PATCHED}")
    endif()
endif()

#==============================================================================
# Application targets
#==============================================================================

option(LIGHTHOST_BUILD_CLI "Build the console-subsystem Light Host CLI executable" ON)
option(LIGHTHOST_CLI_KEEP_SYMBOLS
       "Build LightHostCLI without optimisation/LTO and retain full debugger symbols" ON)

set(LIGHTHOST_SOURCES
    Source/HostStartup.cpp
    Source/IsolatedPluginScanner.cpp
    Source/IconMenu.cpp
    Source/AudioSettingsComponent.cpp
    Source/PluginChain.cpp
    Source/PluginWindow.cpp
    Source/PresetManager.cpp
)

# Binary resources are shared by the GUI and CLI-subsystem executables.
juce_add_binary_data(LightHost_Resources
    NAMESPACE BinaryData
    SOURCES
        Resources/icon.png
        Resources/menu_icon.png
        Resources/menu_icon_white.png
)

if(WIN32)
    find_path(ASIOSDK_INCLUDE_DIR iasiodrv.h PATH_SUFFIXES asiosdk/common)
    if(ASIOSDK_INCLUDE_DIR)
        message(STATUS "ASIO SDK found: ${ASIOSDK_INCLUDE_DIR}")
    else()
        message(FATAL_ERROR "ASIO SDK not found. "
            "Install via: vcpkg install asiosdk  "
            "Or set CMAKE_PREFIX_PATH to the SDK location")
    endif()
endif()

function(configure_light_host_target target)
    target_sources(${target} PRIVATE ${LIGHTHOST_SOURCES})

    target_compile_definitions(${target} PRIVATE
        JUCE_PLUGINHOST_VST3=1
        JUCE_PLUGINHOST_VST=${LIGHTHOST_PLUGINHOST_VST}
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:${target},JUCE_PRODUCT_NAME>"
        JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:${target},JUCE_VERSION>"
    )

    if(WIN32)
        target_compile_definitions(${target} PRIVATE
            JUCE_ASIO=1
            JUCE_USE_WINRT_MIDI=1
        )
        target_include_directories(${target} PRIVATE ${ASIOSDK_INCLUDE_DIR})
        target_link_libraries(${target} PRIVATE dbghelp)
    else()
        target_compile_definitions(${target} PRIVATE JUCE_ASIO=0)
    endif()

    if(APPLE)
        target_compile_definitions(${target} PRIVATE JUCE_PLUGINHOST_AU=1)
    else()
        target_compile_definitions(${target} PRIVATE JUCE_PLUGINHOST_AU=0)
    endif()

    target_link_libraries(${target} PRIVATE
        juce::juce_audio_utils
        juce::juce_gui_extra
        juce::juce_data_structures
        LightHost_Resources
    )

    target_link_libraries(${target} PUBLIC
        juce::juce_recommended_warning_flags
    )
endfunction()

function(make_reverse_engineering_friendly target)
    # Keep functions visible and stable for Binary Ninja/x64dbg:
    # no LTO, no inlining/optimisation, frame pointers retained, and no
    # linker dead-stripping or identical-code folding.
    set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)

    if(MSVC)
        target_compile_options(${target} PRIVATE /MP /EHsc /Od /Ob0 /Zi /Oy-)
        target_link_options(${target} PRIVATE /DEBUG:FULL /OPT:NOREF /OPT:NOICF)
    elseif(APPLE)
        target_compile_options(${target} PRIVATE -O0 -g3 -fno-omit-frame-pointer -fno-inline)
        # Apple's linker does not dead-strip by default. Do not pass -dead_strip,
        # and avoid the unsupported inverse flag -no_dead_strip.
    else()
        target_compile_options(${target} PRIVATE -O0 -g3 -fno-omit-frame-pointer -fno-inline)
        target_link_options(${target} PRIVATE -Wl,--no-gc-sections -Wl,--export-dynamic)
    endif()
endfunction()

juce_add_gui_app(LightHost
    PRODUCT_NAME "Light Host"
    COMPANY_NAME "Rolando Islas"
    COMPANY_WEBSITE "https://www.rolandoislas.com"
    COMPANY_EMAIL "admin@rolandoislas.com"
    BUNDLE_ID "com.rolandoislas.LightHost"
    VERSION "1.2.0"
    ICON_BIG "Resources/icon.png"
    ICON_SMALL "Resources/icon.png"
)
configure_light_host_target(LightHost)

# Keep the normal tray-host build optimised.
target_link_libraries(LightHost PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
)

if(LIGHTHOST_BUILD_CLI)
    # This is still a full JUCE GUI application and can display the real plugin
    # editor. The console subsystem only adds usable stdin/stdout/stderr when
    # launched from a terminal or debugger.
    juce_add_console_app(LightHostCLI
        PRODUCT_NAME "Light Host CLI"
        COMPANY_NAME "Rolando Islas"
        VERSION "1.2.0"
    )
    configure_light_host_target(LightHostCLI)
    set_target_properties(LightHostCLI PROPERTIES OUTPUT_NAME "lhc")
    target_compile_definitions(LightHostCLI PRIVATE LIGHTHOST_CLI_BUILD=1)

    if(LIGHTHOST_CLI_KEEP_SYMBOLS)
        make_reverse_engineering_friendly(LightHostCLI)

        if(APPLE)
            add_custom_command(TARGET LightHostCLI POST_BUILD
                COMMAND dsymutil "$<TARGET_FILE:LightHostCLI>"
                        -o "$<TARGET_FILE:LightHostCLI>.dSYM"
                COMMENT "Generating full dSYM for LightHostCLI"
                VERBATIM
            )
        endif()
    else()
        target_link_libraries(LightHostCLI PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
        )
    endif()
endif()
