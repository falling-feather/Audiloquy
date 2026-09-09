cmake_minimum_required(VERSION 3.25)

project(EnglishListeningPrototype VERSION 0.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

include(CTest)

add_library(listening_core STATIC
    src/core/editor_history.cpp
    src/core/recovery_store.cpp
    src/core/json.cpp
    src/core/output_paths.cpp
    src/core/project.cpp
    src/core/scenario_generator.cpp
    src/core/multi_question.cpp
)
target_include_directories(listening_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_library(listening_audio STATIC
    src/audio/wav_builder.cpp
)
target_include_directories(listening_audio PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_library(listening_storage STATIC src/core/project_storage.cpp)
target_link_libraries(listening_storage PUBLIC listening_core listening_audio)

if(WIN32)
    target_compile_definitions(listening_core PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_definitions(listening_storage PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    add_library(listening_windows STATIC
        src/platform/windows/windows_audio.cpp
    )
    target_include_directories(listening_windows PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(listening_windows PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(listening_windows PUBLIC sapi ole32 uuid winmm)

    find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Network)

    add_executable(audiloquy_kokoro_helper src/tools/kokoro_helper.cpp)
    set_target_properties(audiloquy_kokoro_helper PROPERTIES OUTPUT_NAME audiloquy-kokoro-helper AUTOMOC OFF AUTORCC OFF)
    target_compile_definitions(audiloquy_kokoro_helper PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(audiloquy_kokoro_helper PRIVATE normaliz)
    if(MINGW)
        target_link_options(audiloquy_kokoro_helper PRIVATE -municode -static)
    endif()

    add_library(listening_import STATIC src/app/audio_import.cpp)
    target_link_libraries(listening_import PUBLIC listening_windows Qt6::Core PRIVATE mfplat mfreadwrite mfuuid)
    target_compile_definitions(listening_import PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

    # GNU windres cannot invoke its preprocessor when the source tree contains
    # non-ASCII path components. Stage only the two icon inputs in an ASCII
    # temporary directory, compile there without inherited include flags, then
    # link the generated COFF object from the normal build directory.
    string(MD5 audiloquy_icon_stage_key "${CMAKE_CURRENT_SOURCE_DIR}")
    file(TO_CMAKE_PATH "$ENV{TEMP}/audiloquy-icon-${audiloquy_icon_stage_key}"
         audiloquy_icon_stage)
    set(audiloquy_icon_object "${CMAKE_CURRENT_BINARY_DIR}/audiloquy-icon.obj")
    add_custom_command(
        OUTPUT "${audiloquy_icon_object}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${audiloquy_icon_stage}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/brand/audiloquy.rc"
                "${audiloquy_icon_stage}/audiloquy.rc"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/brand/audiloquy.ico"
                "${audiloquy_icon_stage}/audiloquy.ico"
        COMMAND "${CMAKE_RC_COMPILER}" -i "${audiloquy_icon_stage}/audiloquy.rc"
                -o "${audiloquy_icon_stage}/audiloquy-icon.obj" -O coff
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${audiloquy_icon_stage}/audiloquy-icon.obj"
                "${audiloquy_icon_object}"
        DEPENDS assets/brand/audiloquy.rc assets/brand/audiloquy.ico
        VERBATIM
    )
    set_source_files_properties("${audiloquy_icon_object}"
                                PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)

    add_executable(EnglishListeningPrototype WIN32
        src/app/main.cpp
        tests/desktop_workflow_smoke.cpp
        tests/desktop_workflow_smoke.h
        tests/recording_workflow_smoke.cpp
        tests/recording_workflow_smoke.h
        src/app/main_window.cpp
        src/app/main_window.h
        src/app/recording_marker_dialog.cpp
        src/app/recording_marker_dialog.h
        src/app/render_job.cpp
        src/app/render_job.h
        src/app/local_voice_pack.cpp
        src/app/local_voice_pack.h
        src/app/styled_message_dialog.cpp
        src/app/styled_message_dialog.h
        src/app/deepseek_scenario_polisher.cpp
        src/app/deepseek_scenario_polisher.h
        src/app/scenario_dialog.cpp
        src/app/scenario_dialog.h
        assets/brand/audiloquy.qrc
        "${audiloquy_icon_object}"
    )
    target_compile_definitions(EnglishListeningPrototype PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    )
    target_link_libraries(EnglishListeningPrototype PRIVATE
        listening_core
        listening_audio
        listening_windows
        listening_storage
        listening_import
        Qt6::Widgets
        Qt6::Network
    )
    set_target_properties(EnglishListeningPrototype PROPERTIES OUTPUT_NAME Audiloquy)
    # The bundled moc also needs an ASCII output path for Q_OBJECT sources.
    # Keep only generated metadata in the existing per-workspace staging area.
    set_target_properties(EnglishListeningPrototype PROPERTIES
        AUTOGEN_BUILD_DIR "${audiloquy_icon_stage}/autogen-app"
    )
endif()

if(BUILD_TESTING)
    add_executable(core_tests tests/core_tests.cpp)
    target_link_libraries(core_tests PRIVATE listening_core)
    add_test(NAME core_tests COMMAND core_tests)

    add_executable(editor_history_tests tests/editor_history_tests.cpp)
    target_link_libraries(editor_history_tests PRIVATE listening_core)
    add_test(NAME editor_history_tests COMMAND editor_history_tests)

    add_executable(project_storage_tests tests/project_storage_tests.cpp)
    target_link_libraries(project_storage_tests PRIVATE listening_storage)
    add_test(NAME project_storage_tests COMMAND project_storage_tests)

    add_executable(recovery_store_tests tests/recovery_store_tests.cpp)
    target_link_libraries(recovery_store_tests PRIVATE listening_core)
    add_test(NAME recovery_store_tests COMMAND recovery_store_tests)

    add_executable(output_paths_tests tests/output_paths_tests.cpp)
    target_link_libraries(output_paths_tests PRIVATE listening_core)
    add_test(NAME output_paths_tests COMMAND output_paths_tests)

    add_executable(scenario_generator_tests tests/scenario_generator_tests.cpp)
    target_link_libraries(scenario_generator_tests PRIVATE listening_core)
    add_test(NAME scenario_generator_tests COMMAND scenario_generator_tests)

    add_executable(multi_question_tests tests/multi_question_tests.cpp)
    target_link_libraries(multi_question_tests PRIVATE listening_core)
    add_test(NAME multi_question_tests COMMAND multi_question_tests)

    add_executable(wav_builder_tests tests/wav_builder_tests.cpp)
    target_link_libraries(wav_builder_tests PRIVATE listening_audio)
    add_test(NAME wav_builder_tests COMMAND wav_builder_tests)

    if(WIN32)
        add_executable(windows_audio_tests tests/windows_audio_tests.cpp)
        target_link_libraries(windows_audio_tests PRIVATE listening_windows)
        add_test(NAME windows_audio_tests COMMAND windows_audio_tests)

        add_executable(prototype_integration_tests tests/prototype_integration_tests.cpp)
        target_link_libraries(prototype_integration_tests PRIVATE
            listening_core
            listening_audio
            listening_windows
        )
        add_test(NAME prototype_integration_tests COMMAND prototype_integration_tests)

        add_executable(deepseek_scenario_tests
            tests/deepseek_scenario_tests.cpp
            src/app/deepseek_scenario_polisher.cpp
            src/app/deepseek_scenario_polisher.h
        )
        target_link_libraries(deepseek_scenario_tests PRIVATE listening_core Qt6::Network)
        add_test(NAME deepseek_scenario_tests COMMAND deepseek_scenario_tests)

        add_executable(local_voice_pack_tests
            tests/local_voice_pack_tests.cpp
            src/app/local_voice_pack.cpp
            src/app/local_voice_pack.h
        )
        target_link_libraries(local_voice_pack_tests PRIVATE listening_windows Qt6::Core)
        add_test(NAME local_voice_pack_tests COMMAND local_voice_pack_tests)

        add_executable(render_job_tests
            tests/render_job_tests.cpp
            src/app/render_job.cpp
            src/app/render_job.h
            src/app/local_voice_pack.cpp
            src/app/local_voice_pack.h
        )
        target_compile_definitions(render_job_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
        target_link_libraries(render_job_tests PRIVATE
            listening_core listening_audio listening_windows listening_storage listening_import Qt6::Core
        )
        add_test(NAME render_job_tests COMMAND render_job_tests)
        set_target_properties(render_job_tests PROPERTIES
            AUTOGEN_BUILD_DIR "${audiloquy_icon_stage}/autogen-render-tests"
        )
        set_tests_properties(render_job_tests PROPERTIES TIMEOUT 60)

        add_executable(audio_import_tests tests/audio_import_tests.cpp src/app/render_job.cpp src/app/render_job.h src/app/local_voice_pack.cpp)
        target_link_libraries(audio_import_tests PRIVATE listening_import listening_storage Qt6::Core)
        target_compile_definitions(audio_import_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
        set_target_properties(audio_import_tests PROPERTIES AUTOGEN_BUILD_DIR "${audiloquy_icon_stage}/autogen-import-tests")
        add_test(NAME audio_import_tests COMMAND audio_import_tests)
        set_tests_properties(audio_import_tests PROPERTIES TIMEOUT 60)

        add_executable(deepseek_live_smoke EXCLUDE_FROM_ALL
            tests/deepseek_live_smoke.cpp
            src/app/deepseek_scenario_polisher.cpp
            src/app/deepseek_scenario_polisher.h
        )
        target_link_libraries(deepseek_live_smoke PRIVATE listening_core Qt6::Network)

        set(preview_smoke_directory "${CMAKE_BINARY_DIR}/preview-smoke")
        file(MAKE_DIRECTORY "${preview_smoke_directory}")
        add_test(NAME prototype_preview_smoke
            COMMAND EnglishListeningPrototype --preview-smoke-test
        )
        set_tests_properties(prototype_preview_smoke PROPERTIES
            WORKING_DIRECTORY "${preview_smoke_directory}"
            TIMEOUT 60
        )

        set(scenario_smoke_directory "${CMAKE_BINARY_DIR}/scenario-smoke")
        file(MAKE_DIRECTORY "${scenario_smoke_directory}")
        add_test(NAME prototype_scenario_smoke
            COMMAND EnglishListeningPrototype --scenario-smoke-test
        )
        set_tests_properties(prototype_scenario_smoke PROPERTIES
            WORKING_DIRECTORY "${scenario_smoke_directory}"
            TIMEOUT 60
        )

        set(ui_smoke_directory "${CMAKE_BINARY_DIR}/ui-smoke")
        file(MAKE_DIRECTORY "${ui_smoke_directory}")
        add_test(NAME prototype_ui_smoke
            COMMAND EnglishListeningPrototype --smoke-test
        )
        set_tests_properties(prototype_ui_smoke PROPERTIES
            WORKING_DIRECTORY "${ui_smoke_directory}"
            TIMEOUT 60
        )

        set(editor_smoke_directory "${CMAKE_BINARY_DIR}/editor-smoke")
        file(MAKE_DIRECTORY "${editor_smoke_directory}")
        add_test(NAME prototype_editor_smoke
            COMMAND EnglishListeningPrototype --editor-smoke-test
        )
        set_tests_properties(prototype_editor_smoke PROPERTIES
            WORKING_DIRECTORY "${editor_smoke_directory}"
            TIMEOUT 60
        )

        set(workflow_smoke_directory "${CMAKE_BINARY_DIR}/workflow-smoke")
        file(MAKE_DIRECTORY "${workflow_smoke_directory}")
        add_test(NAME prototype_workflow_smoke
            COMMAND EnglishListeningPrototype --workflow-smoke-test
        )
        set_tests_properties(prototype_workflow_smoke PROPERTIES
            WORKING_DIRECTORY "${workflow_smoke_directory}"
            TIMEOUT 50
        )
        set(classroom_smoke_directory "${CMAKE_BINARY_DIR}/classroom-smoke")
        file(MAKE_DIRECTORY "${classroom_smoke_directory}")
        add_test(NAME prototype_classroom_smoke COMMAND EnglishListeningPrototype --classroom-smoke-test)
        set_tests_properties(prototype_classroom_smoke PROPERTIES WORKING_DIRECTORY "${classroom_smoke_directory}" TIMEOUT 50)
        set(recording_smoke_directory "${CMAKE_BINARY_DIR}/recording-smoke")
        file(MAKE_DIRECTORY "${recording_smoke_directory}")
        add_test(NAME prototype_recording_smoke COMMAND EnglishListeningPrototype --recording-smoke-test)
        set_tests_properties(prototype_recording_smoke PROPERTIES WORKING_DIRECTORY "${recording_smoke_directory}" TIMEOUT 50)
        set(multi_smoke_directory "${CMAKE_BINARY_DIR}/multi-smoke")
        file(MAKE_DIRECTORY "${multi_smoke_directory}")
        add_test(NAME prototype_multi_smoke COMMAND EnglishListeningPrototype --scenario-multi-smoke-test)
        set_tests_properties(prototype_multi_smoke PROPERTIES WORKING_DIRECTORY "${multi_smoke_directory}" TIMEOUT 50)
    endif()
endif()

foreach(target listening_core listening_audio listening_storage)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()

if(WIN32)
    if(MSVC)
        target_compile_options(listening_windows PRIVATE /W4 /permissive- /utf-8)
        target_compile_options(EnglishListeningPrototype PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(listening_windows PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(EnglishListeningPrototype PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

set_target_properties(listening_core listening_audio PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)
