# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Required Notice: Copyright (c) 2026 GiantLLM contributors.

set(TARGET GiantLLM)

add_executable(${TARGET} giantllm.cpp)
target_compile_features(${TARGET} PRIVATE cxx_std_17)
target_include_directories(${TARGET} PRIVATE ${CMAKE_SOURCE_DIR}/common)
target_include_directories(${TARGET} PRIVATE ${CMAKE_SOURCE_DIR}/vendor/cpp-httplib)
target_include_directories(${TARGET} PRIVATE ${CMAKE_SOURCE_DIR}/vendor)
target_link_libraries(${TARGET} PRIVATE cpp-httplib)

set(GIANTLLM_BRANDING_DIR ${CMAKE_SOURCE_DIR}/assets/branding)
set(GIANTLLM_ICON_FILE ${GIANTLLM_BRANDING_DIR}/giantllm.ico)
if (WIN32 AND EXISTS ${GIANTLLM_ICON_FILE})
    file(TO_CMAKE_PATH "${GIANTLLM_ICON_FILE}" GIANTLLM_ICON_RC_FILE)
    configure_file(giantllm.rc.in giantllm.rc @ONLY)
    target_sources(${TARGET} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/giantllm.rc)
endif()

add_custom_command(TARGET ${TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:${TARGET}>/assets/branding
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/dashboard.html
        $<TARGET_FILE_DIR:${TARGET}>/dashboard.html
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${GIANTLLM_BRANDING_DIR}/giantllm.svg
        $<TARGET_FILE_DIR:${TARGET}>/assets/branding/giantllm.svg
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${GIANTLLM_ICON_FILE}
        $<TARGET_FILE_DIR:${TARGET}>/assets/branding/giantllm.ico)

if (WIN32)
    target_link_libraries(${TARGET} PRIVATE shell32)
endif()

if (LLAMA_TOOLS_INSTALL)
    install(TARGETS ${TARGET} RUNTIME)
    install(FILES dashboard.html DESTINATION .)
    install(FILES
        ${GIANTLLM_BRANDING_DIR}/giantllm.svg
        ${GIANTLLM_ICON_FILE}
        DESTINATION assets/branding)
endif()
