# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Required Notice: Copyright (c) 2026 SparkMoE contributors.

set(TARGET SparkMoE)
file(STRINGS "${CMAKE_SOURCE_DIR}/VERSION" SPARKMOE_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/VERSION")

if (WIN32)
    add_executable(${TARGET} WIN32 sparkmoe.cpp)
else()
    add_executable(${TARGET} sparkmoe.cpp)
endif()
target_compile_definitions(${TARGET} PRIVATE SPARKMOE_VERSION="${SPARKMOE_VERSION}")
target_compile_features(${TARGET} PRIVATE cxx_std_17)
target_include_directories(${TARGET} PRIVATE ${CMAKE_SOURCE_DIR}/common)
target_include_directories(${TARGET} PRIVATE ${CMAKE_SOURCE_DIR}/vendor/cpp-httplib)
target_include_directories(${TARGET} PRIVATE ${CMAKE_SOURCE_DIR}/vendor)
target_link_libraries(${TARGET} PRIVATE cpp-httplib)

set(SPARKMOE_BRANDING_DIR ${CMAKE_SOURCE_DIR}/assets/branding)
set(SPARKMOE_ICON_FILE ${SPARKMOE_BRANDING_DIR}/SparkMoE.ico)
set(SPARKMOE_LOGO_FILE ${SPARKMOE_BRANDING_DIR}/SparkMoE.png)
if (WIN32 AND EXISTS ${SPARKMOE_ICON_FILE})
    configure_file("${SPARKMOE_ICON_FILE}" SparkMoE.ico COPYONLY)
    set(SPARKMOE_ICON_RC_FILE "SparkMoE.ico")
    configure_file(sparkmoe.rc.in sparkmoe.rc @ONLY)
    target_sources(${TARGET} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/sparkmoe.rc)
endif()

if (WIN32)
    target_link_libraries(${TARGET} PRIVATE shell32 ole32)
endif()

if (LLAMA_TOOLS_INSTALL)
    install(TARGETS ${TARGET} RUNTIME)
endif()
