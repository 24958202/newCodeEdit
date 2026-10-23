cmake_minimum_required(VERSION 3.25)

project(newCodeEdit LANGUAGES CXX)

# C++ configuration
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Cocoa headers require Objective-C++ on macOS.
if(APPLE)
    enable_language(OBJCXX)

    set(CMAKE_OBJCXX_STANDARD 23)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    set(CMAKE_OBJCXX_EXTENSIONS OFF)

    # Keep the .cpp filename, but compile it as Objective-C++.
    set_source_files_properties(
        newCodeEdit.cpp
        PROPERTIES LANGUAGE OBJCXX
    )
endif()

# Path configuration for Saucer and its dependencies
set(
    SAUCER_ROOT
    "/Users/jidengfeng/Downloads/libs/saucer"
    CACHE PATH "Path to Saucer source root"
)

set(
    SAUCER_BUILD_DIR
    "${SAUCER_ROOT}/build"
    CACHE PATH "Path to Saucer build directory"
)

# MACOSX_BUNDLE creates a proper macOS .app application bundle.
add_executable(
    newCodeEdit
    WIN32
    MACOSX_BUNDLE
    newCodeEdit.cpp
)

# Include directories
target_include_directories(newCodeEdit PRIVATE
    "${SAUCER_ROOT}/include"
    "${SAUCER_BUILD_DIR}/_deps/coco-src/include"
    "${SAUCER_BUILD_DIR}/_deps/rebind-src/include"
    "${SAUCER_BUILD_DIR}/_deps/ereignis-src/include"
    "${SAUCER_BUILD_DIR}/_deps/polo-src/include"
    "${SAUCER_BUILD_DIR}/_deps/saucer-fill-src/include"
    "${SAUCER_BUILD_DIR}/_deps/functional-src/include"
    "${SAUCER_BUILD_DIR}/_deps/saucer-desktop-src/include"
    "${SAUCER_BUILD_DIR}/_deps/glaze-src/include"
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
    -include
    "${SAUCER_BUILD_DIR}/_deps/saucer-fill-src/include/functional.hpp"
)

# Link static libraries
target_link_libraries(newCodeEdit PRIVATE
    "${SAUCER_BUILD_DIR}/libsaucer.a"
    "${SAUCER_BUILD_DIR}/_deps/saucer-desktop-build/libsaucer-desktop.a"
    "${SAUCER_BUILD_DIR}/_deps/coco-build/libcoco.a"
)

# macOS frameworks and bundle properties
if(APPLE)
    find_library(COCOA_FRAMEWORK NAMES Cocoa REQUIRED)
    find_library(WEBKIT_FRAMEWORK NAMES WebKit REQUIRED)
    find_library(COREIMAGE_FRAMEWORK NAMES CoreImage REQUIRED)

    target_link_libraries(newCodeEdit PRIVATE
        "${COCOA_FRAMEWORK}"
        "${WEBKIT_FRAMEWORK}"
        "${COREIMAGE_FRAMEWORK}"
    )

    set_target_properties(newCodeEdit PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "newCodeEdit"
        MACOSX_BUNDLE_GUI_IDENTIFIER "dev.ronnie.newcodeedit"
    )
endif()

# Installation rules
# Installs the .app bundle into /Applications.
install(TARGETS newCodeEdit
    BUNDLE DESTINATION "/Applications"
    RUNTIME DESTINATION "bin"
)
