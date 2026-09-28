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
# This patch adds "AudioEffect" as an accepted category at the VST3 filter
# locations. The patched source is staged in the build directory so configuring
# ForkHost never changes the installed JUCE package.
#
# The path is auto-detected from JUCE_MODULES_DIR when available (set by
# JUCEConfig.cmake). Override with -DJUCE_VST3_FORMAT_SRC=<path> or set
# to empty to skip patching.
#==============================================================================

# Stage a patched JUCE module source file in the build tree and redirect that
# module's wrapper source to the staged copy. JUCE's wrapper includes the
# patched file using a module-relative path, so mirroring the module directory
# layout makes the local copy take precedence while other includes continue to
# resolve from JUCE's original module directory.
function(forkhost_stage_juce_patch module_name wrapper_name patch_relative source_file description)
    if(NOT TARGET "${module_name}")
        message(WARNING "JUCE target ${module_name} is unavailable; skipping ${description}")
        return()
    endif()

    if(NOT EXISTS "${source_file}")
        message(WARNING "JUCE source not found at '${source_file}'; skipping ${description}")
        return()
    endif()

    get_target_property(_juce_module_base "${module_name}" INTERFACE_JUCE_MODULE_PATH)
    if(NOT _juce_module_base)
        message(WARNING "JUCE module path for ${module_name} is unavailable; skipping ${description}")
        return()
    endif()

    set(_juce_content "")
    file(READ "${source_file}" _juce_content)
    set(_juce_patched "${_juce_content}")
    set(_patch_rules ${ARGN})
    list(LENGTH _patch_rules _patch_rule_count)
    math(EXPR _patch_rule_remainder "${_patch_rule_count} % 2")
    if(_patch_rule_remainder)
        message(FATAL_ERROR "Invalid patch rules supplied for ${description}")
    endif()

    set(_patch_rule_index 0)
    while(_patch_rule_index LESS _patch_rule_count)
        list(GET _patch_rules ${_patch_rule_index} _patch_search)
        math(EXPR _replacement_index "${_patch_rule_index} + 1")
        list(GET _patch_rules ${_replacement_index} _patch_replacement)

        string(FIND "${_juce_patched}" "${_patch_search}" _search_position)
        if(NOT _search_position EQUAL -1)
            string(REPLACE "${_patch_search}" "${_patch_replacement}" _juce_patched "${_juce_patched}")
        else()
            string(FIND "${_juce_patched}" "${_patch_replacement}" _replacement_position)
            if(_replacement_position EQUAL -1)
                message(WARNING "Expected JUCE code for ${description} was not found; skipping the patch")
                return()
            endif()
        endif()

        math(EXPR _patch_rule_index "${_patch_rule_index} + 2")
    endwhile()

    set(_juce_module_dir "${_juce_module_base}/${module_name}")
    set(_juce_wrapper_source "${_juce_module_dir}/${wrapper_name}")
    get_target_property(_juce_module_sources "${module_name}" INTERFACE_SOURCES)
    set(_found_wrapper FALSE)
    foreach(_module_source IN LISTS _juce_module_sources)
        get_filename_component(_module_source_name "${_module_source}" NAME)
        if(_module_source_name STREQUAL "${wrapper_name}")
            set(_juce_wrapper_source "${_module_source}")
            set(_found_wrapper TRUE)
            break()
        endif()
    endforeach()

    if(NOT _found_wrapper OR NOT EXISTS "${_juce_wrapper_source}")
        message(WARNING "JUCE wrapper ${wrapper_name} was not found for ${module_name}; skipping ${description}")
        return()
    endif()

    set(_staged_module_root "${CMAKE_CURRENT_BINARY_DIR}/ForkHostJucePatches/modules")
    set(_staged_module_dir "${_staged_module_root}/${module_name}")
    set(_staged_wrapper_source "${_staged_module_dir}/${wrapper_name}")
    set(_staged_patch_source "${_staged_module_dir}/${patch_relative}")
    get_filename_component(_staged_wrapper_dir "${_staged_wrapper_source}" DIRECTORY)
    get_filename_component(_staged_patch_dir "${_staged_patch_source}" DIRECTORY)
    get_filename_component(_juce_patch_relative_dir "${patch_relative}" DIRECTORY)
    file(MAKE_DIRECTORY "${_staged_wrapper_dir}" "${_staged_patch_dir}")
    configure_file("${_juce_wrapper_source}" "${_staged_wrapper_source}" COPYONLY)
    file(WRITE "${_staged_patch_source}" "${_juce_patched}")

    list(REMOVE_ITEM _juce_module_sources "${_juce_wrapper_source}")
    list(APPEND _juce_module_sources "${_staged_wrapper_source}")
    set_property(TARGET "${module_name}" PROPERTY INTERFACE_SOURCES "${_juce_module_sources}")
    target_include_directories("${module_name}" BEFORE INTERFACE
        "${_staged_module_root}"
        "${_juce_module_dir}/${_juce_patch_relative_dir}"
        "${_juce_module_dir}"
        "${_juce_module_base}"
    )

    message(STATUS "Using build-local JUCE patch for ${description}")
endfunction()

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

        forkhost_stage_juce_patch(
            juce_audio_processors
            juce_audio_processors.cpp
            format_types/juce_VST3PluginFormat.cpp
            "${JUCE_VST3_FORMAT_SRC}"
            "VST3 AudioEffect category (WaveShell)"
            "if (c.category != kVstAudioEffectClass)"
            "if (c.category != kVstAudioEffectClass && c.category != \"AudioEffect\")"
            "if (std::strcmp (info.category, kVstAudioEffectClass) != 0)"
            "if (std::strcmp (info.category, kVstAudioEffectClass) != 0 && std::strcmp (info.category, \"AudioEffect\") != 0)"
        )
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
    forkhost_stage_juce_patch(
        juce_audio_utils
        juce_audio_utils.cpp
        gui/juce_AudioDeviceSelectorComponent.cpp
        "${_FORKHOST_JUCE_DEVICE_SELECTOR}"
        "audio-device empty-list theme text"
        "g.setColour (Colours::grey);"
        "g.setColour (findColour (ListBox::textColourId));"
    )
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

# Keep the normal tray-host build optimised.
target_link_libraries(ForkHost PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
)

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
            juce::juce_recommended_lto_flags
        )
    endif()
endif()
