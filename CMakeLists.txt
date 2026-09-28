cmake_minimum_required(VERSION 3.22)
project(ForkHost VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(JUCE CONFIG REQUIRED)

# VST2 hosting is opt-in because JUCE requires the separately-supplied VST2 SDK.
# Do not vendor or distribute Steinberg's VST2 SDK as part of this repository.
if(NOT FORKHOST_LEGACY_OPTIONS_MIGRATED)
    # Preserve existing local builds when upgrading from the previous option names.
    foreach(_option IN ITEMS ENABLE_VST2_HOST VST2_SDK_PATH BUILD_CLI CLI_KEEP_SYMBOLS)
        set(_legacy_option "LIGHTHOST_${_option}")
        set(_forkhost_option "FORKHOST_${_option}")
        if(DEFINED ${_legacy_option})
            if(_option STREQUAL "VST2_SDK_PATH")
                set(${_forkhost_option} "${${_legacy_option}}" CACHE PATH
                    "Migrated from the previous LightHost option name" FORCE)
            else()
                set(${_forkhost_option} "${${_legacy_option}}" CACHE BOOL
                    "Migrated from the previous LightHost option name" FORCE)
            endif()
        endif()
    endforeach()
    set(FORKHOST_LEGACY_OPTIONS_MIGRATED ON CACHE INTERNAL "Legacy options migrated")
endif()

option(FORKHOST_ENABLE_VST2_HOST "Enable local VST2 plug-in hosting" OFF)
set(FORKHOST_VST2_SDK_PATH "" CACHE PATH
    "Path to a locally available VST2 SDK (must contain pluginterfaces/vst2.x headers)")

if(FORKHOST_ENABLE_VST2_HOST)
    if(NOT FORKHOST_VST2_SDK_PATH)
        message(FATAL_ERROR
            "FORKHOST_ENABLE_VST2_HOST=ON requires -DFORKHOST_VST2_SDK_PATH=<path to VST2 SDK>")
    endif()

    if(NOT EXISTS "${FORKHOST_VST2_SDK_PATH}/pluginterfaces/vst2.x/aeffect.h")
        message(FATAL_ERROR
            "VST2 SDK headers not found under '${FORKHOST_VST2_SDK_PATH}'. "
            "Expected pluginterfaces/vst2.x/aeffect.h")
    endif()

    if(NOT COMMAND juce_set_vst2_sdk_path)
        message(FATAL_ERROR
            "This JUCE package does not expose juce_set_vst2_sdk_path(); "
            "use a JUCE CMake package with VST2 host support")
    endif()

    juce_set_vst2_sdk_path("${FORKHOST_VST2_SDK_PATH}")
    set(FORKHOST_PLUGINHOST_VST 1)
else()
    set(FORKHOST_PLUGINHOST_VST 0)
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
    set(_FORKHOST_JUCE_DEVICE_SELECTOR
        "${JUCE_MODULES_DIR}/juce_audio_utils/gui/juce_AudioDeviceSelectorComponent.cpp")
    file(READ "${_FORKHOST_JUCE_DEVICE_SELECTOR}" _FORKHOST_JUCE_DEVICE_SELECTOR_CONTENT)
    string(REPLACE
        "g.setColour (Colours::grey);"
        "g.setColour (findColour (ListBox::textColourId));"
        _FORKHOST_JUCE_DEVICE_SELECTOR_PATCHED
        "${_FORKHOST_JUCE_DEVICE_SELECTOR_CONTENT}")
    if(NOT _FORKHOST_JUCE_DEVICE_SELECTOR_CONTENT STREQUAL _FORKHOST_JUCE_DEVICE_SELECTOR_PATCHED)
        message(STATUS "Patching JUCE audio-device empty-list text to use the host theme")
        file(WRITE "${_FORKHOST_JUCE_DEVICE_SELECTOR}" "${_FORKHOST_JUCE_DEVICE_SELECTOR_PATCHED}")
    endif()
endif()

#==============================================================================
# Application targets
#==============================================================================

option(FORKHOST_BUILD_CLI "Build the console-subsystem ForkHost CLI executable" ON)
option(FORKHOST_CLI_KEEP_SYMBOLS
       "Build ForkHostCLI without optimisation/LTO and retain full debugger symbols" ON)

set(FORKHOST_SOURCES
    Source/HostStartup.cpp
    Source/IsolatedPluginScanner.cpp
    Source/PluginRackWorker.cpp
    Source/IconMenu.cpp
    Source/AudioSettingsComponent.cpp
    Source/PluginChain.cpp
    Source/PluginWindow.cpp
    Source/PresetManager.cpp
)

# Binary resources are shared by the GUI and CLI-subsystem executables.
juce_add_binary_data(ForkHost_Resources
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

function(configure_fork_host_target target)
    target_sources(${target} PRIVATE ${FORKHOST_SOURCES})

    target_compile_definitions(${target} PRIVATE
        JUCE_PLUGINHOST_VST3=1
        JUCE_PLUGINHOST_VST=${FORKHOST_PLUGINHOST_VST}
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
        ForkHost_Resources
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

juce_add_gui_app(ForkHost
    PRODUCT_NAME "ForkHost"
    COMPANY_NAME "Rolando Islas"
    COMPANY_WEBSITE "https://www.rolandoislas.com"
    COMPANY_EMAIL "admin@rolandoislas.com"
    BUNDLE_ID "com.rolandoislas.ForkHost"
    VERSION "1.2.0"
    ICON_BIG "Resources/icon.png"
    ICON_SMALL "Resources/icon.png"
)
configure_fork_host_target(ForkHost)

# Keep debugger symbols in the normal host binary, but allow the linker to drop
# unused code and fold identical functions. LTO stays off.
set_property(TARGET ForkHost PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
if(MSVC)
    target_compile_options(ForkHost PRIVATE /MP /EHsc /Od /Ob0 /Zi /Oy- /Gy /Gw)
    target_link_options(ForkHost PRIVATE /DEBUG:FULL /OPT:REF /OPT:ICF)
elseif(APPLE)
    target_compile_options(ForkHost PRIVATE -O0 -g3 -fno-omit-frame-pointer -fno-inline -ffunction-sections -fdata-sections)
    target_link_options(ForkHost PRIVATE -Wl,-dead_strip)
else()
    target_compile_options(ForkHost PRIVATE -O0 -g3 -fno-omit-frame-pointer -fno-inline -ffunction-sections -fdata-sections)
    target_link_options(ForkHost PRIVATE -Wl,--gc-sections)
endif()

if(FORKHOST_BUILD_CLI)
    # This is still a full JUCE GUI application and can display the real plugin
    # editor. The console subsystem only adds usable stdin/stdout/stderr when
    # launched from a terminal or debugger.
    juce_add_console_app(ForkHostCLI
        PRODUCT_NAME "ForkHost CLI"
        COMPANY_NAME "Rolando Islas"
        BUNDLE_ID "com.forkhost.cli"
        VERSION "1.2.0"
    )
    configure_fork_host_target(ForkHostCLI)
    set_target_properties(ForkHostCLI PROPERTIES OUTPUT_NAME "lhc")
    target_compile_definitions(ForkHostCLI PRIVATE FORKHOST_CLI_BUILD=1)

    if(FORKHOST_CLI_KEEP_SYMBOLS)
        make_reverse_engineering_friendly(ForkHostCLI)

        if(APPLE)
            add_custom_command(TARGET ForkHostCLI POST_BUILD
                COMMAND dsymutil "$<TARGET_FILE:ForkHostCLI>"
                        -o "$<TARGET_FILE:ForkHostCLI>.dSYM"
                COMMENT "Generating full dSYM for ForkHostCLI"
                VERBATIM
            )
        endif()
    else()
        target_link_libraries(ForkHostCLI PUBLIC
            juce::juce_recommended_config_flags
        )
    endif()
endif()
