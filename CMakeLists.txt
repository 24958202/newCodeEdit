cmake_minimum_required(VERSION 3.25)

# Automatically prefer Homebrew LLVM Clang if present (checked before project())
if(EXISTS "/opt/homebrew/opt/llvm/bin/clang++")
    set(CMAKE_CXX_COMPILER "/opt/homebrew/opt/llvm/bin/clang++")
    set(CMAKE_OBJCXX_COMPILER "/opt/homebrew/opt/llvm/bin/clang++")
elseif(EXISTS "/usr/local/opt/llvm/bin/clang++")
    set(CMAKE_CXX_COMPILER "/usr/local/opt/llvm/bin/clang++")
    set(CMAKE_OBJCXX_COMPILER "/usr/local/opt/llvm/bin/clang++")
endif()

project(newCodeEdit CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 23)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    set(CMAKE_OBJCXX_EXTENSIONS OFF)
endif()

# Path configuration for Saucer, dependencies, and UI resources
set(WORK_ROOT "/Users/jidengfeng/Downloads/MLCpplib/newCodeEditSrc" CACHE PATH "Path to current work directory")
set(SAUCER_ROOT "/Users/jidengfeng/Downloads/libs/saucer" CACHE PATH "Path to Saucer source root")
set(SAUCER_BUILD_DIR "${SAUCER_ROOT}/build" CACHE PATH "Path to Saucer build directory")
set(RESOURCES_DIR "${WORK_ROOT}/resources" CACHE PATH "Path to UI resources directory (js and vs)")

# --- MACOS ICON (.icns) & INFO.PLIST GENERATION ---
if(APPLE)
    set(ICONSET_DIR "${CMAKE_BINARY_DIR}/newCodeEdit.iconset")
    set(ICNS_FILE "${CMAKE_BINARY_DIR}/newCodeEdit.icns")
    set(ICON_GEN_SRC "${CMAKE_BINARY_DIR}/gen_iconset.mm")
    set(ICON_GEN_BIN "${CMAKE_BINARY_DIR}/gen_iconset")

    # Write a small native Cocoa tool to render logo_xpm into a Retina .iconset
    file(WRITE "${ICON_GEN_SRC}" "
#import <Cocoa/Cocoa.h>
#include <cstring>
#include <string>
#include <vector>
#include <map>

static const char* logo_xpm[] = {
\"32 32 4 1\",
\"     c None\",
\".    c #1E1E1E\",
\"X    c #2D2D2D\",
\"O    c #3794FF\",
\"................................\",
\"................................\",
\"......XXXXXXXXXXXXXXXXXXXX......\",
\"....XX....................XX....\",
\"...  X........................X  ...\",
\"..  X..........................X  ..\",
\".  X............OO............X  .\",
\".  X...........OOOO...........  .\",
\".  X..........OO..OO..........  .\",
\".  X.........OO...............  .\",
\".  X.........OO...............  .\",
\".  X..........OO..OO..........  .\",
\".  X...........OOOO...........  .\",
\".  X............OO............X  .\",
\".  X..........................X  .\",
\".  X..........................X  .\",
\".  X..........................X  .\",
\".  X............OO............X  .\",
\".  X...........OOOO...........  .\",
\".  X..........OO..OO..........  .\",
\".  X.........OO...............  .\",
\".  X.........OO...............  .\",
\".  X..........OO..OO..........  .\",
\".  X...........OOOO...........  .\",
\".  X............OO............X  .\",
\"..  X..........................X  ..\",
\"...  X........................X  ...\",
\"....XX....................XX....\",
\"......XXXXXXXXXXXXXXXXXXXX......\",
\"................................\",
\"................................\",
\"................................\"
};

void write_png(const std::vector<uint8_t>& base_px, int bw, int bh, int target_size, NSString* path) {
    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc]
        initWithBitmapDataPlanes:nil
                      pixelsWide:target_size
                      pixelsHigh:target_size
                   bitsPerSample:8
                 samplesPerPixel:4
                        hasAlpha:YES
                        isPlanar:NO
                  colorSpaceName:NSCalibratedRGBColorSpace
                     bytesPerRow:target_size * 4
                    bitsPerPixel:32];
    uint8_t* dst = [rep bitmapData];
    for (int y = 0; y < target_size; ++y) {
        int sy = (y * bh) / target_size;
        for (int x = 0; x < target_size; ++x) {
            int sx = (x * bw) / target_size;
            std::memcpy(dst + (y * target_size + x) * 4, &base_px[(sy * bw + sx) * 4], 4);
        }
    }
    NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    [png writeToFile:path atomically:YES];
}

int main(int argc, char* argv[]) {
    if (argc < 2) return 1;
    @autoreleasepool {
        int w = 32, h = 32, nc = 4, cpp = 1;
        std::map<char, uint32_t> cmap = {
            {' ', 0x00000000},
            {'.', 0x1E1E1EFF},
            {'X', 0x2D2D2DFF},
            {'O', 0x3794FFFF}
        };
        std::vector<uint8_t> px(w * h * 4, 0);
        for (int y = 0; y < h; ++y) {
            const char* row = logo_xpm[5 + y];
            for (int x = 0; x < w; ++x) {
                uint32_t c = cmap[row[x]];
                int i = (y * w + x) * 4;
                px[i + 0] = (c >> 24) & 0xFF;
                px[i + 1] = (c >> 16) & 0xFF;
                px[i + 2] = (c >> 8) & 0xFF;
                px[i + 3] = c & 0xFF;
            }
        }
        NSString* dir = [NSString stringWithUTF8String:argv[1]];
        struct Spec { int sz; const char* name; } specs[] = {
            {16,   \"icon_16x16.png\"},
            {32,   \"icon_16x16@2x.png\"},
            {32,   \"icon_32x32.png\"},
            {64,   \"icon_32x32@2x.png\"},
            {128,  \"icon_128x128.png\"},
            {256,  \"icon_128x128@2x.png\"},
            {256,  \"icon_256x256.png\"},
            {512,  \"icon_256x256@2x.png\"},
            {512,  \"icon_512x512.png\"},
            {1024, \"icon_512x512@2x.png\"}
        };
        for (const auto& s : specs) {
            NSString* p = [dir stringByAppendingPathComponent:[NSString stringWithUTF8String:s.name]];
            write_png(px, w, h, s.sz, p);
        }
    }
    return 0;
}
")

    file(MAKE_DIRECTORY "${ICONSET_DIR}")
    execute_process(
        COMMAND "${CMAKE_OBJCXX_COMPILER}" -std=c++23 "${ICON_GEN_SRC}" -framework Cocoa -o "${ICON_GEN_BIN}"
        RESULT_VARIABLE GEN_COMPILE_RES
    )
    if(GEN_COMPILE_RES EQUAL 0)
        execute_process(COMMAND "${ICON_GEN_BIN}" "${ICONSET_DIR}")
        execute_process(COMMAND "/usr/bin/iconutil" -c icns "${ICONSET_DIR}" -o "${ICNS_FILE}")
    endif()

    # Generate Info.plist with CFBundleIconFile pointing to newCodeEdit.icns
    file(WRITE "${CMAKE_BINARY_DIR}/Info.plist.in" "<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>English</string>
    <key>CFBundleExecutable</key>
    <string>\${MACOSX_BUNDLE_EXECUTABLE_NAME}</string>
    <key>CFBundleIconFile</key>
    <string>newCodeEdit.icns</string>
    <key>CFBundleIdentifier</key>
    <string>\${MACOSX_BUNDLE_GUI_IDENTIFIER}</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>\${MACOSX_BUNDLE_BUNDLE_NAME}</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>1.0.0</string>
    <key>CFBundleVersion</key>
    <string>1</string>
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>CFBundleDocumentTypes</key>
    <array>
        <dict>
            <key>CFBundleTypeName</key>
            <string>Text and Source Code Document</string>
            <key>CFBundleTypeIconFile</key>
            <string>newCodeEdit.icns</string>
            <key>CFBundleTypeRole</key>
            <string>Editor</string>
            <key>LSHandlerRank</key>
            <string>Alternate</string>
            <key>LSItemContentTypes</key>
            <array>
                <string>public.text</string>
                <string>public.plain-text</string>
                <string>public.source-code</string>
                <string>public.script</string>
                <string>public.shell-script</string>
                <string>public.json</string>
                <string>public.xml</string>
                <string>public.yaml</string>
                <string>public.data</string>
            </array>
            <key>CFBundleTypeExtensions</key>
            <array>
                <string>*</string>
                <string>txt</string>
                <string>cpp</string>
                <string>hpp</string>
                <string>cc</string>
                <string>cxx</string>
                <string>c</string>
                <string>h</string>
                <string>mm</string>
                <string>m</string>
                <string>json</string>
                <string>js</string>
                <string>ts</string>
                <string>jsx</string>
                <string>tsx</string>
                <string>md</string>
                <string>py</string>
                <string>rs</string>
                <string>go</string>
                <string>java</string>
                <string>swift</string>
                <string>sh</string>
                <string>zsh</string>
                <string>bash</string>
                <string>cmake</string>
                <string>html</string>
		<string>htm</string>
                <string>css</string>
                <string>xml</string>
                <string>yaml</string>
                <string>yml</string>
                <string>toml</string>
                <string>ini</string>
                <string>conf</string>
                <string>log</string>
                <string>sql</string>
                <string>lua</string>
            </array>
        </dict>
    </array>
</dict>
</plist>
")
endif()

# Create the executable and bundle newCodeEdit.icns into Contents/Resources/
if(APPLE AND EXISTS "${ICNS_FILE}")
    add_executable(newCodeEdit MACOSX_BUNDLE newCodeEdit.cpp "${ICNS_FILE}")
    set_source_files_properties("${ICNS_FILE}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    set_source_files_properties(newCodeEdit.cpp PROPERTIES LANGUAGE OBJCXX)
else()
    add_executable(newCodeEdit WIN32 MACOSX_BUNDLE newCodeEdit.cpp)
    if(APPLE)
        set_source_files_properties(newCodeEdit.cpp PROPERTIES LANGUAGE OBJCXX)
    endif()
endif()

# Include directories
target_include_directories(newCodeEdit PRIVATE
    "/opt/homebrew/include"
    "/usr/local/include"
    "${SAUCER_ROOT}/include"
    "${SAUCER_BUILD_DIR}/_deps/coco-src/include"
    "${SAUCER_BUILD_DIR}/_deps/rebind-src/include"
    "${SAUCER_BUILD_DIR}/_deps/ereignis-src/include"
    "${SAUCER_BUILD_DIR}/_deps/polo-src/include"
    "${SAUCER_BUILD_DIR}/_deps/saucer-fill-src/include"
    "${SAUCER_BUILD_DIR}/_deps/functional-src/include"
    "${SAUCER_BUILD_DIR}/_deps/saucer-desktop-src/include"
    "${SAUCER_BUILD_DIR}/_deps/glaze-src/include"
    "${WORK_ROOT}"
    "${WORK_ROOT}/reg"
)

if(EXISTS "/Users/jidengfeng/Downloads/MLCpplib/deepseek/workbuddy/saucer")
    target_include_directories(newCodeEdit PRIVATE "/Users/jidengfeng/Downloads/MLCpplib/deepseek/workbuddy/saucer")
endif()

# Preprocessor definitions
target_compile_definitions(newCodeEdit PRIVATE FILL_NAMESPACE=std)
if(APPLE)
    target_compile_definitions(newCodeEdit PRIVATE SAUCER_WEBKIT SAUCER_WEBKIT_PRIVATE)
elseif(UNIX)
    target_compile_definitions(newCodeEdit PRIVATE SAUCER_WEBKITGTK)
elseif(WIN32)
    target_compile_definitions(newCodeEdit PRIVATE SAUCER_WEBVIEW2)
endif()

# Compiler flags
if(EXISTS "${SAUCER_BUILD_DIR}/_deps/saucer-fill-src/include/functional.hpp")
    target_compile_options(newCodeEdit PRIVATE
        -Wno-unknown-attributes
        -Wno-missing-braces
        "SHELL:-include ${SAUCER_BUILD_DIR}/_deps/saucer-fill-src/include/functional.hpp"
    )
endif()

# Link static libraries
target_link_libraries(newCodeEdit PRIVATE
    "${SAUCER_BUILD_DIR}/libsaucer.a"
    "${SAUCER_BUILD_DIR}/_deps/saucer-desktop-build/libsaucer-desktop.a"
    "${SAUCER_BUILD_DIR}/_deps/coco-build/libcoco.a"
)

# macOS Frameworks, Bundle Properties, Resource Bundling, Code Signing, and Installation
if(APPLE)
    if(EXISTS "/opt/homebrew/opt/llvm/lib")
        target_link_directories(newCodeEdit PRIVATE "/opt/homebrew/opt/llvm/lib")
    endif()
    target_link_libraries(newCodeEdit PRIVATE
        "-framework Cocoa"
        "-framework WebKit"
        "-framework CoreImage"
    )
    set_target_properties(newCodeEdit PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "newCodeEdit"
        MACOSX_BUNDLE_ICON_FILE "newCodeEdit.icns"
        MACOSX_BUNDLE_GUI_IDENTIFIER "dev.ronnie.newcodeedit"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_BINARY_DIR}/Info.plist.in"
    )

    # Copy js/ and vs/ into build bundle Contents/Resources BEFORE ad-hoc signing
    add_custom_command(TARGET newCodeEdit POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:newCodeEdit>/Resources"
         COMMAND "${CMAKE_COMMAND}" -E copy "${RESOURCES_DIR}/icon.xpm" "$<TARGET_BUNDLE_CONTENT_DIR:newCodeEdit>/MacOS"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${RESOURCES_DIR}/js" "$<TARGET_BUNDLE_CONTENT_DIR:newCodeEdit>/Resources/js"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${RESOURCES_DIR}/vs" "$<TARGET_BUNDLE_CONTENT_DIR:newCodeEdit>/Resources/vs"
        COMMAND "/usr/bin/find" "$<TARGET_BUNDLE_CONTENT_DIR:newCodeEdit>/Resources" -name ".DS_Store" -delete
        COMMAND "/usr/bin/codesign" --force --deep --sign - "$<TARGET_BUNDLE_DIR:newCodeEdit>"
        COMMAND "/usr/bin/touch" "$<TARGET_BUNDLE_DIR:newCodeEdit>"
        COMMENT "Bundling js and vs into Contents/Resources and ad-hoc signing newCodeEdit.app"
    )

    # Install the .app bundle and explicitly install js/ and vs/ into /Applications/newCodeEdit.app/Contents/Resources
    install(TARGETS newCodeEdit BUNDLE DESTINATION "/Applications" RUNTIME DESTINATION "bin")
    install(DIRECTORY "${RESOURCES_DIR}/js" "${RESOURCES_DIR}/vs"
        DESTINATION "/Applications/newCodeEdit.app/Contents/Resources"
        PATTERN ".DS_Store" EXCLUDE
    )

    # Re-sign after installing resources so Gatekeeper seals the updated Contents/Resources folder
    install(CODE "
        execute_process(COMMAND \"/usr/bin/codesign\" --force --deep --sign - \"/Applications/newCodeEdit.app\" ERROR_QUIET)
        execute_process(COMMAND \"/usr/bin/touch\" \"/Applications/newCodeEdit.app\" ERROR_QUIET)
        execute_process(
            COMMAND \"/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister\" -f \"/Applications/newCodeEdit.app\"
            ERROR_QUIET
        )
    ")
endif()

# Linux desktop integration
if(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WEBKITGTK REQUIRED IMPORTED_TARGET webkitgtk-6.0 gtk4 libadwaita-1)
    target_link_libraries(newCodeEdit PRIVATE PkgConfig::WEBKITGTK)

    # Copy js/ and vs/ next to the executable in the build folder
    add_custom_command(TARGET newCodeEdit POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${RESOURCES_DIR}/js" "$<TARGET_FILE_DIR:newCodeEdit>/js"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${RESOURCES_DIR}/vs" "$<TARGET_FILE_DIR:newCodeEdit>/vs"
        COMMENT "Copying js and vs directories next to newCodeEdit binary"
    )

    # Install newCodeEdit together with js/ and vs/ into bin/
    install(TARGETS newCodeEdit RUNTIME DESTINATION "bin")
    install(DIRECTORY "${RESOURCES_DIR}/js" "${RESOURCES_DIR}/vs"
        DESTINATION "bin"
        PATTERN ".DS_Store" EXCLUDE
    )
endif()

# Windows installation rules
if(WIN32)
    target_link_libraries(newCodeEdit PRIVATE gdiplus dwmapi shlwapi ole32 user32)

    # Copy js/ and vs/ next to newCodeEdit.exe in the build folder
    add_custom_command(TARGET newCodeEdit POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${RESOURCES_DIR}/js" "$<TARGET_FILE_DIR:newCodeEdit>/js"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${RESOURCES_DIR}/vs" "$<TARGET_FILE_DIR:newCodeEdit>/vs"
        COMMENT "Copying js and vs directories next to newCodeEdit.exe"
    )

    # Install newCodeEdit.exe together with js/ and vs/ into bin/
    install(TARGETS newCodeEdit RUNTIME DESTINATION "bin")
    install(DIRECTORY "${RESOURCES_DIR}/js" "${RESOURCES_DIR}/vs"
        DESTINATION "bin"
        PATTERN ".DS_Store" EXCLUDE
    )
endif()

# ==============================================================================
# CROSS-PLATFORM PACKAGING VIA CPACK (.dmg / .deb / .exe)
# ==============================================================================
set(CPACK_PACKAGE_NAME "newCodeEdit")
set(CPACK_PACKAGE_VENDOR "Ronnie")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "newCodeEdit - Lightweight Code Editor")
set(CPACK_PACKAGE_EXECUTABLES "newCodeEdit" "newCodeEdit")

if(APPLE)
    # Generates a .dmg with an /Applications shortcut automatically
    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_DMG_VOLUME_NAME "newCodeEdit")
elseif(UNIX AND NOT APPLE)
    # Generates a .deb package (add ";RPM" if you also want a .rpm on Fedora/RHEL)
    set(CPACK_GENERATOR "DEB")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/opt/newCodeEdit")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Ronnie <dev@ronnie.dev>")
    set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
elseif(WIN32)
    # Generates a Windows Setup .exe installer (requires NSIS or Inno Setup installed)
    # If using CMake 3.27+, you can also use "INNOSETUP" instead of "NSIS"
    set(CPACK_GENERATOR "NSIS")
    set(CPACK_NSIS_DISPLAY_NAME "newCodeEdit")
    set(CPACK_NSIS_PACKAGE_NAME "newCodeEdit")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
endif()

include(CPack)
