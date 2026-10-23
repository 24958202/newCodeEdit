cmake_minimum_required(VERSION 3.25)
project(newCodeEdit CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Path configuration for Saucer and its dependencies
set(SAUCER_ROOT "/Users/jidengfeng/Downloads/libs/saucer" CACHE PATH "Path to Saucer source root")
set(SAUCER_BUILD_DIR "${SAUCER_ROOT}/build" CACHE PATH "Path to Saucer build directory")

add_executable(newCodeEdit newCodeEdit.cpp)

# Include directories
target_include_directories(newCodeEdit PRIVATE
    ${SAUCER_ROOT}/include
    ${SAUCER_BUILD_DIR}/_deps/coco-src/include
    ${SAUCER_BUILD_DIR}/_deps/rebind-src/include
    ${SAUCER_BUILD_DIR}/_deps/ereignis-src/include
    ${SAUCER_BUILD_DIR}/_deps/polo-src/include
    ${SAUCER_BUILD_DIR}/_deps/saucer-fill-src/include
    ${SAUCER_BUILD_DIR}/_deps/functional-src/include
    ${SAUCER_BUILD_DIR}/_deps/saucer-desktop-src/include
    ${SAUCER_BUILD_DIR}/_deps/glaze-src/include
)

# Preprocessor definitions required by Saucer WebKit on macOS
target_compile_definitions(newCodeEdit PRIVATE
    FILL_NAMESPACE=std
    SAUCER_WEBKIT
    SAUCER_WEBKIT_PRIVATE
)

# Compiler flags and force-inclusion of functional helper
target_compile_options(newCodeEdit PRIVATE
    -Wno-unknown-attributes
    -Wno-missing-braces
    "-include" "${SAUCER_BUILD_DIR}/_deps/saucer-fill-src/include/functional.hpp"
)

# Link static libraries
target_link_libraries(newCodeEdit PRIVATE
    ${SAUCER_BUILD_DIR}/libsaucer.a
    ${SAUCER_BUILD_DIR}/_deps/saucer-desktop-build/libsaucer-desktop.a
    ${SAUCER_BUILD_DIR}/_deps/coco-build/libcoco.a
)

# macOS Frameworks
if(APPLE)
    target_link_options(newCodeEdit PRIVATE
        -framework Cocoa
        -framework WebKit
        -framework CoreImage
    )
endif()
