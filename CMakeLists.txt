cmake_minimum_required(VERSION 3.24)
project(RelayHost VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Relay Host currently targets 64-bit Windows.")
endif()

include(FetchContent)
set(JUCE_BUILD_EXTRAS OFF)
set(JUCE_BUILD_EXAMPLES OFF)
FetchContent_Declare(juce
    URL https://codeload.github.com/juce-framework/JUCE/zip/dabd5ad13cd869cba41046947fffc8e5ce5cd662
    URL_HASH SHA256=2ea3ba7d512a47f3b7af7313804a4c3ffe0f541fbd6cd6d6112da7acac010438)
FetchContent_Declare(vst2_headers
    URL https://codeload.github.com/Xaymar/vst2sdk/zip/refs/tags/v0.4.0
    URL_HASH SHA256=de2bad3db73f5e062f3b9a258207a7eacec396ecf069f2f029f7c6424f85896b)
FetchContent_Declare(vst2_juce_shim
    URL https://codeload.github.com/heide-oficial/Light-Host-Modern/zip/12cef28abb48a6df98a37de8254f3efd7e919db7
    URL_HASH SHA256=8caa704393b0ff52a14df7da4ab494afbcb6e1a830ea01edb579073fffc440cc
    SOURCE_SUBDIR ThirdParty/XaymarVST2JuceShim)
FetchContent_MakeAvailable(vst2_headers vst2_juce_shim juce)

juce_add_gui_app(RelayHost PRODUCT_NAME "Relay Host" COMPANY_NAME "Relay Host"
    BUNDLE_ID com.relayhost.desktop)
target_sources(RelayHost PRIVATE Source/Main.cpp Source/AudioEngine.cpp Source/PluginScan.cpp
    Source/VoicemeeterInsert.cpp)
target_compile_features(RelayHost PRIVATE cxx_std_20)
target_include_directories(RelayHost PRIVATE
    "${vst2_headers_SOURCE_DIR}/include"
    "${vst2_juce_shim_SOURCE_DIR}/ThirdParty/XaymarVST2JuceShim/include")
target_compile_definitions(RelayHost PRIVATE
    JUCE_ASIO=1
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=1
    JUCE_PLUGINHOST_VST=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0)
target_link_libraries(RelayHost PRIVATE
    juce::juce_audio_utils
    juce::juce_gui_extra
    juce::juce_recommended_config_flags)
if(MSVC)
    target_compile_options(RelayHost PRIVATE /utf-8 /MP2)
endif()

enable_testing()
add_test(NAME audio-realtime-smoke
    COMMAND RelayHost --self-test "${CMAKE_CURRENT_BINARY_DIR}/audio-self-test.txt")
