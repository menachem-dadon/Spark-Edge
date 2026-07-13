# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Required Notice: Copyright (c) 2026 SparkMoE contributors.

set(TARGET SparkMoE)

file(READ ${CMAKE_CURRENT_SOURCE_DIR}/dashboard.html SPARKMOE_DASHBOARD_HTML)
configure_file(dashboard.generated.h.in dashboard.generated.h @ONLY)

add_executable(${TARGET} sparkmoe.cpp)
target_compile_features(${TARGET} PRIVATE cxx_std_17)
target_include_directories(${TARGET} PRIVATE ${CMAKE_SOURCE_DIR}/common)
target_include_directories(${TARGET} PRIVATE ${CMAKE_SOURCE_DIR}/vendor/cpp-httplib)
target_include_directories(${TARGET} PRIVATE ${CMAKE_SOURCE_DIR}/vendor)
target_include_directories(${TARGET} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(${TARGET} PRIVATE cpp-httplib)

set(SPARKMOE_BRANDING_DIR ${CMAKE_SOURCE_DIR}/assets/branding)
set(SPARKMOE_ICON_FILE ${SPARKMOE_BRANDING_DIR}/SparkMoE.ico)
set(SPARKMOE_LOGO_FILE ${SPARKMOE_BRANDING_DIR}/SparkMoE.png)
if (WIN32 AND EXISTS ${SPARKMOE_ICON_FILE})
    file(TO_CMAKE_PATH "${SPARKMOE_ICON_FILE}" SPARKMOE_ICON_RC_FILE)
    configure_file(sparkmoe.rc.in sparkmoe.rc @ONLY)
    target_sources(${TARGET} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/sparkmoe.rc)
endif()

add_custom_command(TARGET ${TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:${TARGET}>/assets/branding
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/dashboard.html
        $<TARGET_FILE_DIR:${TARGET}>/dashboard.html
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${SPARKMOE_LOGO_FILE}
        $<TARGET_FILE_DIR:${TARGET}>/assets/branding/SparkMoE.png
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${SPARKMOE_ICON_FILE}
        $<TARGET_FILE_DIR:${TARGET}>/assets/branding/SparkMoE.ico)

if (WIN32)
    target_link_libraries(${TARGET} PRIVATE shell32 ole32)
endif()

if (LLAMA_TOOLS_INSTALL)
    install(TARGETS ${TARGET} RUNTIME)
    install(FILES dashboard.html DESTINATION .)
    install(FILES
        ${SPARKMOE_LOGO_FILE}
        ${SPARKMOE_ICON_FILE}
        DESTINATION assets/branding)
endif()
