set(TARGET QwenStream)

add_executable(${TARGET} qwenstream.cpp)
target_compile_features(${TARGET} PRIVATE cxx_std_17)
target_include_directories(${TARGET} PRIVATE ${CMAKE_SOURCE_DIR}/common)
target_include_directories(${TARGET} PRIVATE ${CMAKE_SOURCE_DIR}/vendor/cpp-httplib)
target_include_directories(${TARGET} PRIVATE ${CMAKE_SOURCE_DIR}/vendor)
target_link_libraries(${TARGET} PRIVATE cpp-httplib)

if (WIN32)
    target_link_libraries(${TARGET} PRIVATE shell32)
endif()

if (LLAMA_TOOLS_INSTALL)
    install(TARGETS ${TARGET} RUNTIME)
endif()
