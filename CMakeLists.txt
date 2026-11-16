cmake_minimum_required(VERSION 3.22)

project(NightshiftBass VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static runtime so the plugins run without the VC++ redistributable
if (MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# JUCE comes from the Nightshift checkout next door; override with -DJUCE_DIR=...
set(JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../nightshift/external/tracktion_engine/modules/juce" CACHE PATH "JUCE checkout")
add_subdirectory(${JUCE_DIR} juce)

# Per-user plugin folders: no admin rights needed. Windows: VST3 in %LOCALAPPDATA% (FL Studio /
# Nightshift can be pointed at it); a Mac: ~/Library/Audio/Plug-Ins, VST3 and Audio Units, where
# every Mac host looks
if (APPLE)
    set(VST3_USER_DIR "$ENV{HOME}/Library/Audio/Plug-Ins/VST3")
    set(AU_USER_DIR "$ENV{HOME}/Library/Audio/Plug-Ins/Components")
    set(PLUGIN_FORMATS VST3 AU Standalone)
else()
    file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}/Programs/Common/VST3" VST3_USER_DIR)
    set(AU_USER_DIR "")
    set(PLUGIN_FORMATS VST3 Standalone)
endif()

set(BASS_COMMON_SOURCES
    common/BassTheme.cpp
    common/TextAudit.cpp
    common/BassProcessor.cpp
    common/BassEditor.cpp
    common/BassTone.cpp
    common/BassWidgets.cpp)

set(BASS_DEFINITIONS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0   # built under JUCE's open-source licence
    JUCE_STRICT_REFCOUNTEDPOINTER=1)

# One call per plugin: target name, 4-char plugin code, product name, source folder
function(bass_add_plugin TARGET CODE PRODUCT FOLDER)
    juce_add_plugin(${TARGET}
        PRODUCT_NAME "${PRODUCT}"
        COMPANY_NAME "Nightshift"
        BUNDLE_ID "com.nightshift.bass.${FOLDER}"
        PLUGIN_MANUFACTURER_CODE Nsft
        PLUGIN_CODE ${CODE}
        FORMATS ${PLUGIN_FORMATS}
        VST3_CATEGORIES Fx Distortion
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT FALSE
        COPY_PLUGIN_AFTER_BUILD TRUE
        VST3_COPY_DIR "${VST3_USER_DIR}"
        AU_COPY_DIR "${AU_USER_DIR}"
        MICROPHONE_PERMISSION_ENABLED TRUE          # macOS kills a standalone that opens an input without this
        MICROPHONE_PERMISSION_TEXT "The standalone takes your instrument from your audio interface.")

    juce_generate_juce_header(${TARGET})

    file(GLOB PLUGIN_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/plugins/${FOLDER}/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/plugins/${FOLDER}/*.h)

    target_sources(${TARGET} PRIVATE ${PLUGIN_SOURCES} ${BASS_COMMON_SOURCES})
    target_include_directories(${TARGET} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    # JUCE_ASIO: the standalone can open the MiniFuse's ASIO driver (JUCE bundles the SDK)
    target_compile_definitions(${TARGET} PUBLIC ${BASS_DEFINITIONS} JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1 $<$<PLATFORM_ID:Windows>:JUCE_ASIO=1>)
    target_link_libraries(${TARGET} PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

    # The standalone is our own app: a plain window with audio, --snapshot for an offscreen
    # GUI capture, or --render to run a file through the processor directly (IR loading too)
    target_sources(${TARGET}_Standalone PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools/standalone/BassStandalone.cpp)

    if (MSVC)
        target_compile_options(${TARGET} PRIVATE /bigobj /MP)
        target_compile_options(${TARGET}_Standalone PRIVATE /bigobj /MP)
    endif()
endfunction()

bass_add_plugin(NightshiftBassDrive Nbdr "Nightshift Bass Drive" drive)
bass_add_plugin(NightshiftBassSplit Nbsp "Nightshift Bass Split" split)

#==============================================================================
# basstool: hosts the built VST3s to render, validate, measure and time them
juce_add_gui_app(basstool
    PRODUCT_NAME "basstool"
    COMPANY_NAME "Nightshift"
    NEEDS_CURL FALSE
    NEEDS_WEB_BROWSER FALSE)

juce_generate_juce_header(basstool)
target_sources(basstool PRIVATE tools/basstool/Main.cpp tools/basstool/Measure.h tools/basstool/Measure.cpp)
target_include_directories(basstool PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(basstool PRIVATE
    ${BASS_DEFINITIONS}
    JUCE_PLUGINHOST_VST3=1
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_APPLICATION_NAME_STRING="basstool"
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}")
target_link_libraries(basstool PRIVATE
    juce::juce_audio_devices
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

if (MSVC)
    target_compile_options(basstool PRIVATE /bigobj /MP)
endif()
