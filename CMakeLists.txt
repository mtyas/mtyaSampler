cmake_minimum_required(VERSION 3.22)
project(mtyaSampler VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Point the plugin at a running dev server (python -m http.server 8765 in ui/) instead of the
# embedded files, so UI edits show up with a reload instead of a rebuild. Debug builds only.
option(MTYA_UI_DEV_SERVER "Load the UI from http://localhost:8765 instead of embedded files" OFF)

# ---------------------------------------------------------------------------
# Dependencies: JUCE 8 (WebView UI support) + clap-juce-extensions (CLAP format)
# ---------------------------------------------------------------------------
include(FetchContent)
set(MTYA_JUCE_TAG "8.0.15" CACHE STRING "JUCE git tag to build against (use the newest 8.x)")

FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        ${MTYA_JUCE_TAG}
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

FetchContent_Declare(clap_juce_extensions
    GIT_REPOSITORY https://github.com/free-audio/clap-juce-extensions.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(clap_juce_extensions)

# ---------------------------------------------------------------------------
# Plugin target
# ---------------------------------------------------------------------------
juce_add_plugin(mtyaSampler
    COMPANY_NAME                 "mtya"
    BUNDLE_ID                    "com.mtya.mtyasampler"
    PLUGIN_MANUFACTURER_CODE     Mtya
    PLUGIN_CODE                  MtSm
    PRODUCT_NAME                 "mtyaSampler"
    FORMATS                      VST3 Standalone
    IS_SYNTH                     TRUE
    NEEDS_MIDI_INPUT             TRUE
    NEEDS_MIDI_OUTPUT            FALSE
    IS_MIDI_EFFECT               FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS  TRUE
    NEEDS_WEBVIEW2               TRUE
    VST3_CATEGORIES              Instrument Sampler
    COPY_PLUGIN_AFTER_BUILD      FALSE)

clap_juce_extensions_plugin(TARGET mtyaSampler
    CLAP_ID       "com.mtya.mtyasampler"
    CLAP_FEATURES instrument sampler synthesizer stereo)

target_sources(mtyaSampler PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/ParameterSpec.cpp
    Source/UiResources.cpp
    Source/SampleRecorder.cpp
    Source/InstrumentMap.cpp
    Source/SamplerEngine.cpp
    Source/SampleLoader.cpp)

# ---------------------------------------------------------------------------
# Web UI → BinaryData
# JUCE's frontend library is copied into ui/js/juce/ (git-ignored) so the same folder
# works embedded and from the dev server. Files are looked up by FILE NAME at runtime,
# so every file under ui/ must have a unique name — checked below.
# ---------------------------------------------------------------------------
set(JUCE_JS_DIR "${juce_SOURCE_DIR}/modules/juce_gui_extra/native/javascript")
file(GLOB JUCE_JS_FILES "${JUCE_JS_DIR}/*.js")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/ui/js/juce")
foreach(f ${JUCE_JS_FILES})
    get_filename_component(n "${f}" NAME)
    configure_file("${f}" "${CMAKE_CURRENT_SOURCE_DIR}/ui/js/juce/${n}" COPYONLY)
endforeach()

file(GLOB_RECURSE MTYA_UI_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/*.html"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/*.css"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/*.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/*.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/*.svg"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/*.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/*.woff2")

set(_seen "")
foreach(f ${MTYA_UI_FILES})
    get_filename_component(n "${f}" NAME)
    if(n IN_LIST _seen)
        message(FATAL_ERROR "Two UI files are both named '${n}'. Files in ui/ are embedded by name, so rename one.")
    endif()
    list(APPEND _seen "${n}")
endforeach()

juce_add_binary_data(mtyaSampler_UI
    HEADER_NAME MtyaUI.h
    NAMESPACE   MtyaUI
    SOURCES     ${MTYA_UI_FILES})

target_compile_definitions(mtyaSampler PUBLIC
    JUCE_WEB_BROWSER=1
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1
    $<$<AND:$<CONFIG:Debug>,$<BOOL:${MTYA_UI_DEV_SERVER}>>:MTYA_UI_DEV_SERVER=1>)

target_link_libraries(mtyaSampler
    PRIVATE
        mtyaSampler_UI
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_dsp
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Engine tests: plays generated samples through SamplerEngine offline and checks the output.
# cmake --build <build dir> --target mtyaEngineTests, then run it (exit code 1 = a check failed).
# ---------------------------------------------------------------------------
option(MTYA_BUILD_TESTS "Build the offline engine tests" ON)
if(MTYA_BUILD_TESTS)
    juce_add_console_app(mtyaEngineTests PRODUCT_NAME "mtyaEngineTests")
    target_sources(mtyaEngineTests PRIVATE
        tests/EngineTests.cpp
        Source/InstrumentMap.cpp
        Source/SamplerEngine.cpp)
    target_compile_definitions(mtyaEngineTests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1)
    target_link_libraries(mtyaEngineTests
        PRIVATE
            juce::juce_audio_formats
            juce::juce_audio_processors
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
endif()
