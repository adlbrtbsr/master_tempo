cmake_minimum_required(VERSION 3.20)

project(master_tempo VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(juce
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 8.0.3
)
FetchContent_MakeAvailable(juce)

juce_add_gui_app(master_tempo
    PRODUCT_NAME "MasterTempo"
    VERSION "0.1.0"
    COMPANY_NAME "MasterTempo"
)

target_sources(master_tempo PRIVATE
    src/Main.cpp
    src/MainComponent.h
    src/MainComponent.cpp
    src/ui_setup.cpp
    src/ui_layout.cpp
    src/dsp_processing.cpp
    src/loopback_glue.cpp
    src/win/WASAPILoopback.h
)

target_compile_definitions(master_tempo PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(master_tempo PRIVATE
    juce::juce_gui_extra
    juce::juce_osc
    juce::juce_dsp
    juce::juce_audio_utils
    juce::juce_audio_devices
    juce::juce_audio_basics
    juce::juce_gui_basics
    juce::juce_core
    $<$<PLATFORM_ID:Windows>:Ole32>
)

juce_generate_juce_header(master_tempo)


