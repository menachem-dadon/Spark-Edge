set(TARGET qwenstream-iobench)

add_executable(${TARGET} qwenstream-iobench.cpp)
target_compile_features(${TARGET} PRIVATE cxx_std_17)

if (LLAMA_TOOLS_INSTALL)
    install(TARGETS ${TARGET} RUNTIME)
endif()
