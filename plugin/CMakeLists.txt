cmake_minimum_required(VERSION 3.22)

project(deepsvc VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(DEEPSVC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

# ---- JUCE 与 ARA SDK ----
add_subdirectory("${DEEPSVC_ROOT}/modules/JUCE" "${CMAKE_BINARY_DIR}/JUCE")
juce_set_ara_sdk_path("${DEEPSVC_ROOT}/modules/ARA_SDK")

# ---- 引擎静态库、soxr 静态库与模型权重（由 build.py 构建） ----
set(DEEPSVC_ENGINE_LIB "${DEEPSVC_ROOT}/native/target/release/libdeepsvc_engine.a"
    CACHE FILEPATH "deepsvc_engine 静态库路径")
set(DEEPSVC_SOXR_LIB "${DEEPSVC_ROOT}/build/soxr-install/lib/libsoxr.a"
    CACHE FILEPATH "libsoxr 静态库路径")
set(DEEPSVC_MODELS_DIR "${DEEPSVC_ROOT}/checkpoints"
    CACHE PATH "模型权重目录")

if(NOT EXISTS "${DEEPSVC_ENGINE_LIB}")
    message(FATAL_ERROR "引擎静态库不存在: ${DEEPSVC_ENGINE_LIB}（先运行 cargo build --release）")
endif()
if(NOT EXISTS "${DEEPSVC_SOXR_LIB}")
    message(FATAL_ERROR "libsoxr 静态库不存在: ${DEEPSVC_SOXR_LIB}（先运行 build.py 构建 soxr）")
endif()

# MLX 的 gguf C 辅助库：mlx-sys 构建产物，未打进 Rust 静态库，需要显式链接
file(GLOB DEEPSVC_GGUF_LIB "${DEEPSVC_ROOT}/native/target/release/build/mlx-sys-*/out/build/_deps/mlx-build/mlx/io/libgguflib.a")
if(NOT DEEPSVC_GGUF_LIB)
    message(FATAL_ERROR "libgguflib.a 不存在（先运行 cargo build --release）")
endif()

set(DEEPSVC_MODEL_FILES
    rmvpe.safetensors
    fcpe.safetensors
    whisper.safetensors
    campplus.safetensors
    yingmusic_step_000640.safetensors
    pupu-vocoder-large.safetensors
    pc-nsf-hifigan.safetensors
)
foreach(model IN LISTS DEEPSVC_MODEL_FILES)
    if(NOT EXISTS "${DEEPSVC_MODELS_DIR}/${model}")
        message(FATAL_ERROR "模型文件缺失: ${DEEPSVC_MODELS_DIR}/${model}")
    endif()
endforeach()

# ---- 插件目标 ----
juce_add_plugin(deepsvc
    COMPANY_NAME "deepsvc"
    PLUGIN_MANUFACTURER_CODE Dsvc
    PLUGIN_CODE Ds01
    PRODUCT_NAME "deepsvc"
    BUNDLE_ID "com.deepsvc.ara"
    VERSION "${PROJECT_VERSION}"

    FORMATS VST3 AU
    IS_ARA_EFFECT TRUE

    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE

    COPY_PLUGIN_AFTER_BUILD FALSE
)

target_sources(deepsvc PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h

    Source/ARA/DeepSvcDocumentController.cpp
    Source/ARA/DeepSvcDocumentController.h
    Source/ARA/DeepSvcEditorView.cpp
    Source/ARA/DeepSvcEditorView.h
    Source/ARA/DeepSvcPlaybackRenderer.cpp
    Source/ARA/DeepSvcPlaybackRenderer.h
    Source/ARA/DeepSvcAudioModification.cpp
    Source/ARA/DeepSvcAudioModification.h

    Source/Content/ContentArchive.h
    Source/Content/ContentKey.h

    Source/Engine/EngineBridge.cpp
    Source/Engine/EngineBridge.h
    Source/Engine/JobManager.cpp
    Source/Engine/JobManager.h

    Source/Plugin/PluginEditor.cpp
    Source/Plugin/PluginEditor.h

    Source/State/Parameters.cpp
    Source/State/Parameters.h
    Source/State/WorkingParamsStore.cpp
    Source/State/WorkingParamsStore.h

    Source/Timbre/TimbreLibrary.cpp
    Source/Timbre/TimbreLibrary.h

    Source/UI/CopyPanel.cpp
    Source/UI/CopyPanel.h
    Source/UI/DeepSvcLookAndFeel.cpp
    Source/UI/DeepSvcLookAndFeel.h
    Source/UI/ParameterPanel.cpp
    Source/UI/ParameterPanel.h
    Source/UI/PianoRollView.cpp
    Source/UI/PianoRollView.h
    Source/UI/ProcessCell.cpp
    Source/UI/ProcessCell.h
    Source/UI/RightColumn.cpp
    Source/UI/RightColumn.h
    Source/UI/SlotBar.cpp
    Source/UI/SlotBar.h
    Source/UI/TimbrePanel.cpp
    Source/UI/TimbrePanel.h
    Source/UI/TimelineOverviewComponent.cpp
    Source/UI/TimelineOverviewComponent.h
    Source/UI/TimelineViewportCamera.h
    Source/UI/TimelineViewportPolicy.cpp
    Source/UI/TimelineViewportPolicy.h
    Source/UI/UIColors.h
    Source/UI/ViewMapper.h
    Source/UI/WaveformMipmap.cpp
    Source/UI/WaveformMipmap.h

    Source/Utils/ContentTimelineProjection.h
    Source/Utils/TimeCoordinate.h

    Source/Directories.h
)

target_include_directories(deepsvc PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Source"
)

target_link_libraries(deepsvc
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_dsp
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra
        "${DEEPSVC_ENGINE_LIB}"
        "${DEEPSVC_SOXR_LIB}"
        ${DEEPSVC_GGUF_LIB}
        "-framework Accelerate"
        "-framework Metal"
        "-framework CoreGraphics"
        "-framework CoreFoundation"
        "-framework Foundation"
        "-liconv"
        "-lobjc"
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

target_compile_definitions(deepsvc
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_STRICT_REFCOUNTEDPOINTER=1
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_USE_FLAC=1
        JUCE_USE_OGGVORBIS=1
        JUCE_USE_MP3AUDIOFORMAT=1
)

if(APPLE)
    target_compile_options(deepsvc PRIVATE -Wall -Wno-unused-parameter)
endif()

# ---- 自包含打包：模型权重进入 bundle 的 Contents/Resources ----
set(DEEPSVC_BUNDLE_FORMATS VST3 AU)
foreach(fmt IN LISTS DEEPSVC_BUNDLE_FORMATS)
    if(TARGET deepsvc_${fmt})
        set(copy_commands
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_BUNDLE_DIR:deepsvc_${fmt}>/Contents/Resources/Models"
        )
        foreach(model IN LISTS DEEPSVC_MODEL_FILES)
            list(APPEND copy_commands
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${DEEPSVC_MODELS_DIR}/${model}"
                    "$<TARGET_BUNDLE_DIR:deepsvc_${fmt}>/Contents/Resources/Models/${model}"
            )
        endforeach()
        add_custom_command(TARGET deepsvc_${fmt} POST_BUILD
            ${copy_commands}
            COMMENT "打包模型到 ${fmt} bundle"
        )
    endif()
endforeach()
