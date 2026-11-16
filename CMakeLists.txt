cmake_minimum_required(VERSION 3.22)

project(NightshiftLofi VERSION 0.1.0 LANGUAGES C CXX)

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

set(LOFI_COMMON_SOURCES
    common/LofiTheme.cpp
    common/TextAudit.cpp
    common/LofiProcessor.cpp
    common/LofiEditor.cpp)

set(LOFI_DEFINITIONS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0   # built under the GPLv3 licence
    JUCE_STRICT_REFCOUNTEDPOINTER=1)

# One call per plugin: target name, 4-char plugin code, product name, source folder
function(lofi_add_plugin TARGET CODE PRODUCT FOLDER)
    juce_add_plugin(${TARGET}
        PRODUCT_NAME "${PRODUCT}"
        COMPANY_NAME "Nightshift"
        BUNDLE_ID "com.nightshift.lofi.${FOLDER}"
        PLUGIN_MANUFACTURER_CODE Nsft
        PLUGIN_CODE ${CODE}
        FORMATS ${PLUGIN_FORMATS}
        VST3_CATEGORIES Fx
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

    target_sources(${TARGET} PRIVATE ${PLUGIN_SOURCES} ${LOFI_COMMON_SOURCES})
    target_include_directories(${TARGET} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${TARGET} PUBLIC ${LOFI_DEFINITIONS} JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1)
    target_link_libraries(${TARGET} PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

    # The standalone is our own app: a plain window with audio, or --snapshot for an offscreen GUI capture
    target_sources(${TARGET}_Standalone PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools/standalone/LofiStandalone.cpp)

    if (MSVC)
        target_compile_options(${TARGET} PRIVATE /bigobj /MP)
        target_compile_options(${TARGET}_Standalone PRIVATE /bigobj /MP)
    endif()
endfunction()

lofi_add_plugin(NightshiftCassette Ncas "Nightshift Cassette" cassette)
lofi_add_plugin(NightshiftVinyl    Nvin "Nightshift Vinyl"    vinyl)
lofi_add_plugin(NightshiftCrush    Ncru "Nightshift Crush"    crush)
lofi_add_plugin(NightshiftHaze     Nhaz "Nightshift Haze"     haze)

#==============================================================================
# lofitool: hosts the built VST3s to render, snapshot, validate and measure them
juce_add_gui_app(lofitool
    PRODUCT_NAME "lofitool"
    COMPANY_NAME "Nightshift"
    NEEDS_CURL FALSE
    NEEDS_WEB_BROWSER FALSE)

juce_generate_juce_header(lofitool)
target_sources(lofitool PRIVATE tools/lofitool/Main.cpp tools/lofitool/Measure.h tools/lofitool/Measure.cpp)
target_include_directories(lofitool PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(lofitool PRIVATE
    ${LOFI_DEFINITIONS}
    JUCE_PLUGINHOST_VST3=1
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_APPLICATION_NAME_STRING="lofitool"
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}")
target_link_libraries(lofitool PRIVATE
    juce::juce_audio_devices
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

if (MSVC)
    target_compile_options(lofitool PRIVATE /bigobj /MP)
endif()
