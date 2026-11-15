// newCodeEdit - Full Native Setup
// macOS & Linux support. Needs C++23 (std::expected).
// Needs nlohmann/json (find_package(nlohmann_json) + link nlohmann_json::nlohmann_json).
#include <saucer/app.hpp>
#include <saucer/smartview.hpp>
#include <saucer/window.hpp>
#include <nlohmann/json.hpp>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <atomic>
#include <thread>
#include <fcntl.h>
#include <sys/stat.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <process.h>
#else
#include <unistd.h>
#endif

#ifdef __APPLE__
#import <Cocoa/Cocoa.h>
#import <CoreFoundation/CoreFoundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#endif

#ifdef __linux__
#include <dlfcn.h>
#endif

#ifndef __APPLE__
#include "window_raiser.hpp"   // title search fallback (Windows / Linux); macOS uses the registry below
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

// --- EMBEDDED LOGO (XPM format) ---
static const char* logo_xpm[] = {
"32 32 4 1",
"     c None",
".    c #1E1E1E",
"X    c #2D2D2D",
"O    c #3794FF",
"................................",
"................................",
"......XXXXXXXXXXXXXXXXXXXX......",
"....XX....................XX....",
"...\x20\x20X........................X\x20\x20...",
"..\x20\x20X..........................X\x20\x20..",
".\x20\x20X............OO............X\x20\x20.",
".\x20\x20X...........OOOO...........\x20\x20.",
".\x20\x20X..........OO..OO..........\x20\x20.",
".\x20\x20X.........OO...............\x20\x20.",
".\x20\x20X.........OO...............\x20\x20.",
".\x20\x20X..........OO..OO..........\x20\x20.",
".\x20\x20X...........OOOO...........\x20\x20.",
".\x20\x20X............OO............X\x20\x20.",
".\x20\x20X..........................X\x20\x20.",
".\x20\x20X..........................X\x20\x20.",
".\x20\x20X..........................X\x20\x20.",
".\x20\x20X............OO............X\x20\x20.",
".\x20\x20X...........OOOO...........\x20\x20.",
".\x20\x20X..........OO..OO..........\x20\x20.",
".\x20\x20X.........OO...............\x20\x20.",
".\x20\x20X.........OO...............\x20\x20.",
".\x20\x20X..........OO..OO..........\x20\x20.",
".\x20\x20X...........OOOO...........\x20\x20.",
".\x20\x20X............OO............X\x20\x20.",
"..\x20\x20X..........................X\x20\x20..",
"...\x20\x20X........................X\x20\x20...",
"....XX....................XX....",
"......XXXXXXXXXXXXXXXXXXXX......",
"................................",
"................................",
"................................"
};

// --- GLOBAL STATE FOR OPEN-WITH & CLEAN WINDOW SHUTDOWN ---
saucer::application* g_app = nullptr;
std::atomic<bool> g_has_created_any_window{false};
std::vector<std::string> g_pending_files;
std::mutex g_pending_mutex;

std::vector<fs::path> g_temp_files;
std::mutex g_temp_files_mutex;

void create_editor_window(saucer::application* app, bool minimal, const std::string& path, const std::string& code);
void process_pending_files();

void cleanup_temp_files_and_exit() {
    std::lock_guard<std::mutex> lock(g_temp_files_mutex);
    std::error_code ec;
    for (const auto& p : g_temp_files) {
        fs::remove(p, ec);
    }
    g_temp_files.clear();
    ::_exit(0);
}

// Normalizes raw paths or file:// URIs passed by OS file managers
void enqueue_incoming_path(std::string raw_path) {
    if (raw_path.empty()) return;
    if (raw_path.starts_with("file://")) {
        raw_path.erase(0, 7);
    }
    std::error_code ec;
    std::string resolved = fs::canonical(raw_path, ec).string();
    if (ec) resolved = raw_path;

    std::lock_guard<std::mutex> lock(g_pending_mutex);
    g_pending_files.push_back(resolved);
}

// ============================================================================
// CROSS-PLATFORM SYSTEM TRAY & STATUS BAR LIBRARY (macOS / Linux / Windows)
// ============================================================================
namespace sys_tray {

// Shared XPM to RGBA decoder used by all platforms for the tray icon
inline bool decode_xpm_rgba(const char* xpm[], int& width, int& height, std::vector<uint8_t>& pixels) {
    int ncolors = 0, cpp = 0;
    if (std::sscanf(xpm[0], "%d %d %d %d", &width, &height, &ncolors, &cpp) != 4) return false;

    std::map<std::string, uint32_t> color_map;
    int idx = 1;
    for (int i = 0; i < ncolors; ++i, ++idx) {
        std::string line(xpm[idx]);
        std::string key = line.substr(0, cpp);
        size_t c_pos = line.find("c ");
        if (c_pos == std::string::npos) continue;
        std::string color_val = line.substr(c_pos + 2);
        while (!color_val.empty() && color_val.front() == ' ') color_val.erase(color_val.begin());
        while (!color_val.empty() && color_val.back() == ' ') color_val.pop_back();

        uint32_t rgba = 0;
        if (color_val == "None") {
            rgba = 0x00000000;
        } else if (color_val[0] == '#') {
            unsigned int hex = std::stoul(color_val.substr(1), nullptr, 16);
            rgba = (color_val.size() == 7) ? (hex << 8) | 0xFF : hex;
        }
        color_map[key] = rgba;
    }

    pixels.assign(width * height * 4, 0);
    for (int y = 0; y < height; ++y, ++idx) {
        std::string row(xpm[idx]);
        for (int x = 0; x < width; ++x) {
            std::string key = row.substr(x * cpp, cpp);
            uint32_t rgba = color_map.count(key) ? color_map[key] : 0x00000000;
            int p_idx = (y * width + x) * 4;
            pixels[p_idx + 0] = (rgba >> 24) & 0xFF; // R
            pixels[p_idx + 1] = (rgba >> 16) & 0xFF; // G
            pixels[p_idx + 2] = (rgba >> 8) & 0xFF;  // B
            pixels[p_idx + 3] = rgba & 0xFF;         // A
        }
    }
    return true;
}

// Opens a full main editor window (with File -> New File / Open Folder) inside the current process
inline void trigger_new_document() {
    if (!g_app) return;
#ifdef __APPLE__
    dispatch_async(dispatch_get_main_queue(), ^{
        create_editor_window(g_app, false, "", "");
    });
#else
    g_app->post([] {
        create_editor_window(g_app, false, "", "");
    });
#endif
}

// Spawns an independent OS process of newCodeEdit for a new document
inline void trigger_new_process_document() {
#ifdef __APPLE__
    dispatch_async(dispatch_get_main_queue(), ^{
        NSString* bundlePath = [[NSBundle mainBundle] bundlePath];
        NSString* exePath = [[NSBundle mainBundle] executablePath];
        if (bundlePath && [bundlePath hasSuffix:@".app"]) {
            // Launch a fresh instance via macOS `open -n -a` just like opening a new app process
            NSTask* task = [[NSTask alloc] init];
            [task setLaunchPath:@"/usr/bin/open"];
            [task setArguments:@[@"-n", @"-a", bundlePath]];
            @try {
                [task launch];
                return;
            } @catch (...) {}
        }
        if (exePath) {
            NSTask* task = [[NSTask alloc] init];
            [task setLaunchPath:exePath];
            [task setArguments:@[]];
            @try {
                [task launch];
                return;
            } @catch (...) {}
        }
        create_editor_window(g_app, false, "", "");
    });
#elif defined(__linux__)
    std::error_code ec;
    fs::path self_exe = fs::read_symlink("/proc/self/exe", ec);
    if (!ec && !self_exe.empty()) {
        pid_t pid = ::fork();
        if (pid == 0) {
            ::setsid();
            ::execl(self_exe.c_str(), self_exe.c_str(), static_cast<char*>(nullptr));
            ::_exit(1);
        }
        return;
    }
    trigger_new_document();
#elif defined(_WIN32)
    wchar_t exe_path[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) > 0) {
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(exe_path, nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            return;
        }
    }
    trigger_new_document();
#endif
}

} // namespace sys_tray

// --- ONE WINDOW PER FILE (pop-out windows) ---
// Title of the pop-out window that shows `path`: "newCodeEdit - <file name>".
static std::string window_title_for(const std::string& path) {
    return "newCodeEdit - " + fs::path(path).filename().string();
}

// Same key for the same file, however its path is spelled (symlinks, "..", ...).
static std::string path_key(const std::string& p) {
    std::error_code ec;
    const fs::path canon = fs::canonical(p, ec);
    return ec ? fs::path(p).lexically_normal().string() : canon.string();
}

#ifndef __APPLE__
// Windows / Linux: look for a window with that title and raise it.
static bool raise_existing_window_for(const std::string& path) {
    WindowRaiser raiser;
    return raiser.search_processes_exist_title(window_title_for(path));
}
#endif

#ifdef __APPLE__
std::set<void*> g_editor_nswindows;
std::mutex g_nswindows_mutex;

// Which NSWindow shows which file. Looked up directly in this program (no OS window-list query, so no
// permissions, no delay, and it also works for minimized windows and windows on other desktops).
// Entries are removed when their window closes (see the close observer below).
std::map<std::string, void*> g_popout_nswindows;
std::mutex g_popout_mutex;

static std::set<void*> snapshot_nswindows() {
    std::set<void*> all;
    for (NSWindow* w in [NSApp windows]) all.insert((__bridge void*)w);
    return all;
}

// Remembers the NSWindow that appeared since `before` as the window that shows `path`.
static void register_popout_window(const std::string& path, const std::string& title, const std::set<void*>& before) {
    NSString* expected = [NSString stringWithUTF8String:title.c_str()];
    void* chosen = nullptr;
    void* fallback = nullptr;
    for (NSWindow* w in [NSApp windows]) {
        void* p = (__bridge void*)w;
        if (before.count(p)) continue;
        if (!([w styleMask] & NSWindowStyleMaskTitled)) continue;   // skip status-item / popup windows
        if (!fallback) fallback = p;
        if ([[w title] isEqualToString:expected]) { chosen = p; break; }
    }
    if (!chosen) chosen = fallback;
    if (chosen) {
        std::lock_guard<std::mutex> lock(g_popout_mutex);
        g_popout_nswindows[path_key(path)] = chosen;
    }
}

// If `path` is already open in a pop-out window: bring that window to the front and return true.
static bool raise_existing_window_for(const std::string& path) {
    const std::string key = path_key(path);
    void* target = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_popout_mutex);
        const auto it = g_popout_nswindows.find(key);
        if (it != g_popout_nswindows.end()) target = it->second;
    }
    if (!target) return false;
    for (NSWindow* w in [NSApp windows]) {
        if ((__bridge void*)w == target) {
            if ([w isMiniaturized]) [w deminiaturize:nil];
            [w makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
            return true;
        }
    }
    std::lock_guard<std::mutex> lock(g_popout_mutex);   // the window is gone: forget it
    g_popout_nswindows.erase(key);
    return false;
}
static NSStatusItem* g_status_item = nil;
static id g_tray_target = nil;

// Objective-C action handlers for the macOS Status Bar, App Menu & Dock right-click menu
static void macos_tray_new_doc(id, SEL, id) {
    sys_tray::trigger_new_document();
}

static void macos_tray_new_proc(id, SEL, id) {
    sys_tray::trigger_new_process_document();
}

static void macos_tray_quit(id, SEL, id) {
    cleanup_temp_files_and_exit();
}

static void ensure_macos_tray_target() {
    if (g_tray_target) return;
    Class cls = objc_getClass("NewCodeEditTrayTarget");
    if (!cls) {
        cls = objc_allocateClassPair([NSObject class], "NewCodeEditTrayTarget", 0);
        if (cls) {
            class_addMethod(cls, @selector(onNewDocument:), (IMP)macos_tray_new_doc, "v@:@");
            class_addMethod(cls, @selector(onNewProcessDocument:), (IMP)macos_tray_new_proc, "v@:@");
            class_addMethod(cls, @selector(onQuitApp:), (IMP)macos_tray_quit, "v@:@");
            objc_registerClassPair(cls);
        }
    }
    if (cls) {
        g_tray_target = [[cls alloc] init];
        CFRetain((__bridge CFTypeRef)g_tray_target);
    }
}

static NSMenu* build_macos_new_doc_menu(bool include_quit) {
    ensure_macos_tray_target();

    NSMenu* menu = [[NSMenu alloc] initWithTitle:@"newCodeEdit"];
    [menu setAutoenablesItems:NO];

    NSMenuItem* newDocItem = [[NSMenuItem alloc] initWithTitle:@"New Document"
                                                        action:@selector(onNewDocument:)
                                                 keyEquivalent:@""];
    [newDocItem setTarget:g_tray_target];
    [newDocItem setEnabled:YES];
    [menu addItem:newDocItem];

    // NSMenuItem* newProcItem = [[NSMenuItem alloc] initWithTitle:@"New Document (New Process)"
    //                                                      action:@selector(onNewProcessDocument:)
    //                                               keyEquivalent:@""];
    // [newProcItem setTarget:g_tray_target];
    // [newProcItem setEnabled:YES];
    // [menu addItem:newProcItem];

    if (include_quit) {
        [menu addItem:[NSMenuItem separatorItem]];
        NSMenuItem* quitItem = [[NSMenuItem alloc] initWithTitle:@"Quit newCodeEdit"
                                                          action:@selector(onQuitApp:)
                                                   keyEquivalent:@"q"];
        [quitItem setTarget:g_tray_target];
        [quitItem setEnabled:YES];
        [menu addItem:quitItem];
    }

    return menu;
}

// Dock right-click menu handler (exactly like Apple's TextEdit 'New Document' option in the Dock)
static NSMenu* macos_application_dock_menu(id, SEL, NSApplication*) {
    return build_macos_new_doc_menu(false);
}

void init_macos_system_tray(const char* xpm[]) {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (g_status_item != nil) return;

        // Ensure the app has regular activation policy so both Dock & Menu Bar items work
        if ([NSApp activationPolicy] == NSApplicationActivationPolicyProhibited) {
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        }

        // Create top-right macOS Menu Bar Status Item with explicit CFRetain so ARC never frees it
        g_status_item = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
        if (!g_status_item) return;
        CFRetain((__bridge CFTypeRef)g_status_item);

        if ([g_status_item respondsToSelector:@selector(setVisible:)]) {
            [g_status_item setVisible:YES];
        }

        int width = 0, height = 0;
        std::vector<uint8_t> pixels;
        NSImage* trayIcon = nil;
        if (sys_tray::decode_xpm_rgba(xpm, width, height, pixels)) {
            NSBitmapImageRep* rep = [[NSBitmapImageRep alloc]
                initWithBitmapDataPlanes:nil
                              pixelsWide:width
                              pixelsHigh:height
                           bitsPerSample:8
                         samplesPerPixel:4
                                hasAlpha:YES
                                isPlanar:NO
                          colorSpaceName:NSDeviceRGBColorSpace
                             bytesPerRow:width * 4
                            bitsPerPixel:32];
            if (rep && [rep bitmapData]) {
                std::memcpy([rep bitmapData], pixels.data(), pixels.size());
                trayIcon = [[NSImage alloc] initWithSize:NSMakeSize(18, 18)];
                [trayIcon addRepresentation:rep];
                CFRetain((__bridge CFTypeRef)trayIcon);
            }
        }

        if (g_status_item.button) {
            if (trayIcon) {
                g_status_item.button.image = trayIcon;
                g_status_item.button.imagePosition = NSImageLeft;
            }
            // Also show a crisp "<>" label next to the icon so it is unmistakably visible
            // on both Dark and Light macOS menu bars
            g_status_item.button.title = @"[]";
            g_status_item.button.toolTip = @"newCodeEdit - Click for New Document";
        }

        NSMenu* statusMenu = build_macos_new_doc_menu(true);
        CFRetain((__bridge CFTypeRef)statusMenu);
        [g_status_item setMenu:statusMenu];
    });
}

void record_incoming_mac_path(const char* utf8_path) {
    if (!utf8_path || !*utf8_path) return;
    enqueue_incoming_path(utf8_path);
    if (g_app) {
        dispatch_async(dispatch_get_main_queue(), ^{
            process_pending_files();
        });
    }
}

static void macos_open_urls(id, SEL, NSApplication*, NSArray<NSURL*>* urls) {
    for (NSURL* url in urls) {
        if ([url isFileURL] && [url path]) {
            record_incoming_mac_path([[url path] UTF8String]);
        }
    }
}

static void macos_open_files(id, SEL, NSApplication* sender, NSArray<NSString*>* filenames) {
    for (NSString* path in filenames) {
        record_incoming_mac_path([path UTF8String]);
    }
    [sender replyToOpenOrPrint:NSApplicationDelegateReplySuccess];
}

static BOOL macos_open_file(id, SEL, NSApplication*, NSString* filename) {
    if (filename) {
        record_incoming_mac_path([filename UTF8String]);
        return YES;
    }
    return NO;
}

// Intercepts NSDocumentController so AppKit NEVER shows 'cannot open files in the ... format'
static void hooked_open_doc_url(id, SEL, NSURL* url, BOOL, void (^completionHandler)(NSDocument*, BOOL, NSError*)) {
    if (url && [url isFileURL] && [url path]) {
        record_incoming_mac_path([[url path] UTF8String]);
    }
    if (completionHandler) {
        completionHandler(nil, NO, nil);
    }
}

static void attach_open_handlers_to_class(Class cls) {
    if (!cls) return;
    class_replaceMethod(cls, @selector(application:openURLs:), (IMP)macos_open_urls, "v@:@@");
    class_replaceMethod(cls, @selector(application:openFiles:), (IMP)macos_open_files, "v@:@@");
    class_replaceMethod(cls, @selector(application:openFile:), (IMP)macos_open_file, "B@:@@");
    class_replaceMethod(cls, @selector(applicationDockMenu:), (IMP)macos_application_dock_menu, "@@:@");
}

static void attach_open_handlers_to_delegate(id delegate) {
    if (!delegate) return;
    attach_open_handlers_to_class(object_getClass(delegate));
}

static void (*g_orig_set_delegate)(id, SEL, id) = nullptr;

static void hooked_set_delegate(id self, SEL _cmd, id delegate) {
    attach_open_handlers_to_delegate(delegate);
    if (g_orig_set_delegate) {
        g_orig_set_delegate(self, _cmd, delegate);
    }
}

void register_active_editor_nswindows() {
    std::lock_guard<std::mutex> lock(g_nswindows_mutex);
    for (NSWindow* win in [NSApp windows]) {
        if (![win isVisible]) continue;
        NSString* title = [win title];
        if (title && ([title hasPrefix:@"newCodeEdit - "] || [title isEqualToString:@"template"])) {
            g_editor_nswindows.insert((__bridge void*)win);
        }
    }
}

void install_macos_lifecycle_hooks() {
    // 1. Add open handlers & Dock menu to NSObject so ANY delegate class responds immediately
    attach_open_handlers_to_class([NSObject class]);

    // 2. Override NSDocumentController's openDocumentWithContentsOfURL:display:completionHandler:
    Method doc_m = class_getInstanceMethod([NSDocumentController class], @selector(openDocumentWithContentsOfURL:display:completionHandler:));
    if (doc_m) {
        method_setImplementation(doc_m, (IMP)hooked_open_doc_url);
    }

    [NSApplication sharedApplication];

    // 3. Hook -[NSApplication setDelegate:]
    Method m = class_getInstanceMethod([NSApplication class], @selector(setDelegate:));
    if (m && !g_orig_set_delegate) {
        g_orig_set_delegate = (void (*)(id, SEL, id))method_getImplementation(m);
        method_setImplementation(m, (IMP)hooked_set_delegate);
    }
    if ([NSApp delegate]) {
        id del = [NSApp delegate];
        attach_open_handlers_to_delegate(del);
        [NSApp setDelegate:del];
    }

    NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
    [nc addObserverForName:NSWindowWillCloseNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification* note) {
                    NSWindow* win = [note object];
                    if (!win) return;
                    {
                        std::lock_guard<std::mutex> lock(g_popout_mutex);
                        for (auto it = g_popout_nswindows.begin(); it != g_popout_nswindows.end();) {
                            if (it->second == (__bridge void*)win) it = g_popout_nswindows.erase(it);
                            else ++it;
                        }
                    }
                    bool should_exit = false;
                    {
                        std::lock_guard<std::mutex> lock(g_nswindows_mutex);
                        if (g_editor_nswindows.erase((__bridge void*)win) > 0) {
                            should_exit = g_editor_nswindows.empty();
                        }
                    }
                    if (should_exit) {
                        cleanup_temp_files_and_exit();
                    }
                }];

    [nc addObserverForName:NSApplicationWillTerminateNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification*) {
                    cleanup_temp_files_and_exit();
                }];
}

void set_app_icon_from_xpm(const char* xpm[]) {
    int width = 0, height = 0;
    std::vector<uint8_t> pixels;
    if (!sys_tray::decode_xpm_rgba(xpm, width, height, pixels)) return;

    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nil pixelsWide:width pixelsHigh:height bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:width * 4 bitsPerPixel:32];
    std::memcpy([rep bitmapData], pixels.data(), pixels.size());
    NSImage *image = [[NSImage alloc] initWithSize:NSMakeSize(width, height)];
    [image addRepresentation:rep];
    [NSApp setApplicationIconImage:image];
}
#endif // __APPLE__

#ifdef __linux__
// ============================================================================
// LINUX SYSTEM TRAY (Dynamic GTK3 + AppIndicator3 / GtkStatusIcon)
// ============================================================================
namespace linux_tray {

using GCallback = void(*)();
using GSignalConnectDataFn = unsigned long(*)(void*, const char*, GCallback, void*, void*, int);
using GtkMenuNewFn = void*(*)();
using GtkMenuItemNewWithLabelFn = void*(*)(const char*);
using GtkSeparatorMenuItemNewFn = void*(*)();
using GtkMenuShellAppendFn = void(*)(void*, void*);
using GtkWidgetShowAllFn = void(*)(void*);
using GtkMenuPopupAtPointerFn = void(*)(void*, const void*);
using GdkPixbufNewFromDataFn = void*(*)(const uint8_t*, int, int, int, int, int, int, void*, void*);
using GtkStatusIconNewFromPixbufFn = void*(*)(void*);
using GtkStatusIconSetTooltipTextFn = void(*)(void*, const char*);
using GtkStatusIconSetVisibleFn = void(*)(void*, int);

using AppIndicatorNewFn = void*(*)(const char*, const char*, int);
using AppIndicatorSetStatusFn = void(*)(void*, int);
using AppIndicatorSetMenuFn = void(*)(void*, void*);
using AppIndicatorSetTitleFn = void(*)(void*, const char*);

static void* g_tray_menu = nullptr;
static GtkMenuPopupAtPointerFn g_gtk_menu_popup_at_pointer = nullptr;
static std::vector<uint8_t> g_icon_rgba_keepalive;

extern "C" void on_linux_tray_new_doc(void*, void*) {
    sys_tray::trigger_new_document();
}

extern "C" void on_linux_tray_new_proc(void*, void*) {
    sys_tray::trigger_new_process_document();
}

extern "C" void on_linux_tray_quit(void*, void*) {
    cleanup_temp_files_and_exit();
}

extern "C" void on_linux_status_icon_popup(void*, unsigned int, unsigned int, void*) {
    if (g_tray_menu && g_gtk_menu_popup_at_pointer) {
        g_gtk_menu_popup_at_pointer(g_tray_menu, nullptr);
    }
}

void init_linux_system_tray(const char* xpm[]) {
    void* rtld = RTLD_DEFAULT;

    auto g_signal_connect_data_fn = reinterpret_cast<GSignalConnectDataFn>(dlsym(rtld, "g_signal_connect_data"));
    auto gtk_menu_new_fn = reinterpret_cast<GtkMenuNewFn>(dlsym(rtld, "gtk_menu_new"));
    auto gtk_menu_item_new_with_label_fn = reinterpret_cast<GtkMenuItemNewWithLabelFn>(dlsym(rtld, "gtk_menu_item_new_with_label"));
    auto gtk_separator_menu_item_new_fn = reinterpret_cast<GtkSeparatorMenuItemNewFn>(dlsym(rtld, "gtk_separator_menu_item_new"));
    auto gtk_menu_shell_append_fn = reinterpret_cast<GtkMenuShellAppendFn>(dlsym(rtld, "gtk_menu_shell_append"));
    auto gtk_widget_show_all_fn = reinterpret_cast<GtkWidgetShowAllFn>(dlsym(rtld, "gtk_widget_show_all"));
    g_gtk_menu_popup_at_pointer = reinterpret_cast<GtkMenuPopupAtPointerFn>(dlsym(rtld, "gtk_menu_popup_at_pointer"));

    if (!g_signal_connect_data_fn || !gtk_menu_new_fn || !gtk_menu_item_new_with_label_fn ||
        !gtk_menu_shell_append_fn || !gtk_widget_show_all_fn) {
        return;
    }

    g_tray_menu = gtk_menu_new_fn();
    void* item_new_doc = gtk_menu_item_new_with_label_fn("New Document");
    void* item_new_proc = gtk_menu_item_new_with_label_fn("New Document (New Process)");
    void* item_quit = gtk_menu_item_new_with_label_fn("Quit newCodeEdit");

    g_signal_connect_data_fn(item_new_doc, "activate", reinterpret_cast<GCallback>(on_linux_tray_new_doc), nullptr, nullptr, 0);
    g_signal_connect_data_fn(item_new_proc, "activate", reinterpret_cast<GCallback>(on_linux_tray_new_proc), nullptr, nullptr, 0);
    g_signal_connect_data_fn(item_quit, "activate", reinterpret_cast<GCallback>(on_linux_tray_quit), nullptr, nullptr, 0);

    gtk_menu_shell_append_fn(g_tray_menu, item_new_doc);
    gtk_menu_shell_append_fn(g_tray_menu, item_new_proc);
    if (gtk_separator_menu_item_new_fn) {
        gtk_menu_shell_append_fn(g_tray_menu, gtk_separator_menu_item_new_fn());
    }
    gtk_menu_shell_append_fn(g_tray_menu, item_quit);
    gtk_widget_show_all_fn(g_tray_menu);

    void* appind_lib = dlopen("libayatana-appindicator3.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!appind_lib) appind_lib = dlopen("libappindicator3.so.1", RTLD_LAZY | RTLD_LOCAL);

    if (appind_lib) {
        auto app_indicator_new_fn = reinterpret_cast<AppIndicatorNewFn>(dlsym(appind_lib, "app_indicator_new"));
        auto app_indicator_set_status_fn = reinterpret_cast<AppIndicatorSetStatusFn>(dlsym(appind_lib, "app_indicator_set_status"));
        auto app_indicator_set_menu_fn = reinterpret_cast<AppIndicatorSetMenuFn>(dlsym(appind_lib, "app_indicator_set_menu"));
        auto app_indicator_set_title_fn = reinterpret_cast<AppIndicatorSetTitleFn>(dlsym(appind_lib, "app_indicator_set_title"));

        if (app_indicator_new_fn && app_indicator_set_status_fn && app_indicator_set_menu_fn) {
            void* indicator = app_indicator_new_fn("dev.ronnie.newcodeedit", "accessories-text-editor", 0);
            if (indicator) {
                if (app_indicator_set_title_fn) app_indicator_set_title_fn(indicator, "newCodeEdit");
                app_indicator_set_status_fn(indicator, 1);
                app_indicator_set_menu_fn(indicator, g_tray_menu);
                return;
            }
        }
    }

    auto gdk_pixbuf_new_from_data_fn = reinterpret_cast<GdkPixbufNewFromDataFn>(dlsym(rtld, "gdk_pixbuf_new_from_data"));
    auto gtk_status_icon_new_from_pixbuf_fn = reinterpret_cast<GtkStatusIconNewFromPixbufFn>(dlsym(rtld, "gtk_status_icon_new_from_pixbuf"));
    auto gtk_status_icon_set_tooltip_text_fn = reinterpret_cast<GtkStatusIconSetTooltipTextFn>(dlsym(rtld, "gtk_status_icon_set_tooltip_text"));
    auto gtk_status_icon_set_visible_fn = reinterpret_cast<GtkStatusIconSetVisibleFn>(dlsym(rtld, "gtk_status_icon_set_visible"));

    if (gdk_pixbuf_new_from_data_fn && gtk_status_icon_new_from_pixbuf_fn) {
        int w = 0, h = 0;
        if (sys_tray::decode_xpm_rgba(xpm, w, h, g_icon_rgba_keepalive)) {
            void* pixbuf = gdk_pixbuf_new_from_data_fn(g_icon_rgba_keepalive.data(), 0, 1, 8, w, h, w * 4, nullptr, nullptr);
            if (pixbuf) {
                void* status_icon = gtk_status_icon_new_from_pixbuf_fn(pixbuf);
                if (status_icon) {
                    if (gtk_status_icon_set_tooltip_text_fn) gtk_status_icon_set_tooltip_text_fn(status_icon, "newCodeEdit");
                    if (gtk_status_icon_set_visible_fn) gtk_status_icon_set_visible_fn(status_icon, 1);
                    g_signal_connect_data_fn(status_icon, "popup-menu", reinterpret_cast<GCallback>(on_linux_status_icon_popup), nullptr, nullptr, 0);
                    g_signal_connect_data_fn(status_icon, "activate", reinterpret_cast<GCallback>(on_linux_tray_new_doc), nullptr, nullptr, 0);
                }
            }
        }
    }
}

} // namespace linux_tray
#endif // __linux__

#ifdef _WIN32
// ============================================================================
// WINDOWS SYSTEM TRAY (Win32 Shell_NotifyIconW)
// ============================================================================
namespace win32_tray {

constexpr UINT WM_TRAYICON = WM_USER + 101;
constexpr UINT ID_TRAY_NEW_DOC  = 40001;
constexpr UINT ID_TRAY_NEW_PROC = 40002;
constexpr UINT ID_TRAY_QUIT     = 40003;

static HICON create_hicon_from_xpm(const char* xpm[]) {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
    if (!sys_tray::decode_xpm_rgba(xpm, width, height, rgba)) {
        return LoadIconW(nullptr, IDI_APPLICATION);
    }

    BITMAPV5HEADER bi{};
    bi.bV5Size        = sizeof(BITMAPV5HEADER);
    bi.bV5Width       = width;
    bi.bV5Height      = -height;
    bi.bV5Planes      = 1;
    bi.bV5BitCount    = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask     = 0x00FF0000;
    bi.bV5GreenMask   = 0x0000FF00;
    bi.bV5BlueMask    = 0x000000FF;
    bi.bV5AlphaMask   = 0xFF000000;

    void* bits = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP hColor = CreateDIBSection(hdc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!hColor || !bits) return LoadIconW(nullptr, IDI_APPLICATION);

    uint8_t* dst = static_cast<uint8_t*>(bits);
    for (size_t i = 0; i < rgba.size(); i += 4) {
        dst[i + 0] = rgba[i + 2];
        dst[i + 1] = rgba[i + 1];
        dst[i + 2] = rgba[i + 0];
        dst[i + 3] = rgba[i + 3];
    }

    HBITMAP hMask = CreateBitmap(width, height, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon    = TRUE;
    ii.hbmMask  = hMask;
    ii.hbmColor = hColor;
    HICON hIcon = CreateIconIndirect(&ii);

    DeleteObject(hColor);
    DeleteObject(hMask);
    return hIcon ? hIcon : LoadIconW(nullptr, IDI_APPLICATION);
}

static LRESULT CALLBACK tray_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_TRAYICON) {
        if (LOWORD(lparam) == WM_RBUTTONUP || LOWORD(lparam) == WM_CONTEXTMENU) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_NEW_DOC,  L"New Document");
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_NEW_PROC, L"New Document (New Process)");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_QUIT,     L"Quit newCodeEdit");

            SetForegroundWindow(hwnd);
            UINT clicked = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(hMenu);

            if (clicked == ID_TRAY_NEW_DOC) {
                sys_tray::trigger_new_document();
            } else if (clicked == ID_TRAY_NEW_PROC) {
                sys_tray::trigger_new_process_document();
            } else if (clicked == ID_TRAY_QUIT) {
                cleanup_temp_files_and_exit();
            }
            return 0;
        } else if (LOWORD(lparam) == WM_LBUTTONDBLCLK) {
            sys_tray::trigger_new_document();
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void init_win32_system_tray(const char* xpm[]) {
    std::thread([xpm]() {
        HINSTANCE hInst = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(WNDCLASSEXW);
        wc.lpfnWndProc   = tray_wnd_proc;
        wc.hInstance     = hInst;
        wc.lpszClassName = L"NewCodeEditTrayWindowClass";
        RegisterClassExW(&wc);

        HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"newCodeEditTray", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);
        if (!hwnd) return;

        NOTIFYICONDATAW nid{};
        nid.cbSize           = sizeof(NOTIFYICONDATAW);
        nid.hWnd             = hwnd;
        nid.uID              = 1;
        nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = WM_TRAYICON;
        nid.hIcon            = create_hicon_from_xpm(xpm);
        wcsncpy_s(nid.szTip, L"newCodeEdit", _TRUNCATE);

        Shell_NotifyIconW(NIM_ADD, &nid);

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        Shell_NotifyIconW(NIM_DELETE, &nid);
    }).detach();
}

} // namespace win32_tray
#endif // _WIN32

// Unified entry point to initialize the system tray on any OS
void init_cross_platform_system_tray() {
    static std::atomic<bool> initialized{false};
    if (initialized.exchange(true)) return;

#ifdef __APPLE__
    init_macos_system_tray(logo_xpm);
#elif defined(__linux__)
    linux_tray::init_linux_system_tray(logo_xpm);
#elif defined(_WIN32)
    win32_tray::init_win32_system_tray(logo_xpm);
#endif
}

// ==== BEGIN HELPERS (plain C++, no Saucer types) ============================

template <typename T>
using Result = std::expected<T, std::string>;

inline std::unexpected<std::string> fail(std::string message) {
    return std::unexpected<std::string>(std::move(message));
}

template <typename T, typename F>
Result<T> guarded(F&& fn) {
    try {
        return std::forward<F>(fn)();
    } catch (const std::exception& e) {
        return fail(e.what());
    } catch (...) {
        return fail("Unexpected error");
    }
}

void replace_all(std::string& str, const std::string& from, const std::string& to) {
    if(from.empty()) return;
    size_t start_pos = 0;
    while((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
}

// --- NATIVE OS DIALOGS ---
static std::string run_shell_command(const char* command) {
#ifdef _WIN32
    FILE* pipe = ::_popen(command, "r");
#else
    FILE* pipe = ::popen(command, "r");
#endif
    if (!pipe) return "";
    std::string out;
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) out += buffer;
#ifdef _WIN32
    ::_pclose(pipe);
#else
    ::pclose(pipe);
#endif
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

#ifdef __APPLE__
std::string open_folder_dialog() {
    return run_shell_command("/usr/bin/osascript -e 'POSIX path of (choose folder)' 2>/dev/null");
}
std::string open_file_dialog() {
    return run_shell_command("/usr/bin/osascript -e 'POSIX path of (choose file)' 2>/dev/null");
}
std::string save_file_dialog() {
    return run_shell_command("/usr/bin/osascript -e 'POSIX path of (choose file name with prompt \"Save File As:\" default name \"untitled.cpp\")' 2>/dev/null");
}
#else
std::string open_folder_dialog() {
    return run_shell_command("zenity --file-selection --directory 2>/dev/null || kdialog --getexistingdirectory . 2>/dev/null");
}
std::string open_file_dialog() {
    return run_shell_command("zenity --file-selection 2>/dev/null || kdialog --getopenfilename . 2>/dev/null");
}
std::string save_file_dialog() {
    return run_shell_command("zenity --file-selection --save --confirm-overwrite --filename=untitled.cpp 2>/dev/null || kdialog --getsavefilename untitled.cpp 2>/dev/null");
}
#endif

// --- UTF-8 / JSON helpers ---
bool is_valid_utf8(std::string_view s) {
    std::size_t i = 0;
    const std::size_t n = s.size();
    while (i < n) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { ++i; continue; }

        std::size_t len;
        std::uint32_t cp;
        if ((c & 0xE0) == 0xC0)      { len = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
        else return false;

        if (i + len > n) return false;
        for (std::size_t k = 1; k < len; ++k) {
            const auto cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000)) return false;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += len;
    }
    return true;
}

// Fallback Latin-1 (ISO-8859-1) to UTF-8 transcoder (Blueprint §11.1 step 5)
std::string latin1_to_utf8(std::string_view s) {
    std::string out;
    out.reserve(s.size() + s.size() / 4);
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x80) {
            out.push_back(ch);
        } else {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) out += std::format("\\u{:04x}", static_cast<unsigned>(c));
                else out += ch;
        }
    }
    return out;
}

// Escapes text for safe embedding inside a JS string literal that lives in an HTML <script> block.
// '<' becomes \u003c so file contents such as "</script>" or "<!--" (very common in .html files,
// and in source files that contain such strings) can never terminate or confuse the script block.
std::string js_string_escape(std::string_view s) {
    std::string out = json_escape(s);
    replace_all(out, "<", "\\u003c");
    return out;
}

// --- FILE I/O ALGORITHMS ---
constexpr std::uintmax_t kMaxFileBytes = 10ull * 1024 * 1024;

Result<std::string> read_file(const std::string& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return fail(std::format("Not a regular file: {}", path));
    const auto size = fs::file_size(path, ec);
    if (ec) return fail(std::format("Cannot read the size of {}: {}", path, ec.message()));
    if (size > kMaxFileBytes) return fail(std::format("{} is larger than {} MB. Open it in another tool.", path, kMaxFileBytes >> 20));

    std::ifstream in(path, std::ios::binary);
    if (!in) return fail(std::format("Cannot open {}. Check permissions.", path));

    std::string content(static_cast<std::size_t>(size), '\0');
    in.read(content.data(), static_cast<std::streamsize>(content.size()));
    content.resize(static_cast<std::size_t>(in.gcount()));
    if (content.find('\0') != std::string::npos) {
        return fail(std::format("{} appears to be a binary file.", path));
    }
    if (!is_valid_utf8(content)) {
        content = latin1_to_utf8(content);
    }
    return content;
}

Result<bool> atomic_save(const std::string& path, const std::string& content) {
    if (path.empty()) return fail("No file path given.");
    std::error_code ec;
    fs::path target(path);
    if (fs::is_symlink(fs::symlink_status(target, ec))) {
        const fs::path resolved = fs::canonical(target, ec);
        if (!ec) target = resolved;
    }
    if (target.filename().empty()) return fail(std::format("Not a file path: {}", path));

    std::string temp = (target.parent_path() / ("." + target.filename().string() + ".XXXXXX")).string();
    const int fd = ::mkstemp(temp.data());
    if (fd < 0) return fail(std::format("Cannot create temp file next to {}: {}", path, std::strerror(errno)));

    const auto abort_with = [&](const char* what, int err) {
        ::close(fd); ::unlink(temp.c_str());
        return fail(std::format("{} {}: {}", what, path, std::strerror(err)));
    };

    struct stat st {};
    const mode_t mode = (::stat(target.c_str(), &st) == 0) ? (st.st_mode & 07777) : 0644;
    ::fchmod(fd, mode);

    const char* data = content.data();
    std::size_t left = content.size();
    while (left > 0) {
        const ssize_t n = ::write(fd, data, left);
        if (n < 0) { if (errno == EINTR) continue; return abort_with("Cannot write", errno); }
        data += n; left -= static_cast<std::size_t>(n);
    }
#ifdef F_FULLFSYNC
    if (::fcntl(fd, F_FULLFSYNC) == -1) ::fsync(fd);
#else
    ::fsync(fd);
#endif
    if (::close(fd) != 0) {
        const int err = errno; ::unlink(temp.c_str());
        return fail(std::format("Cannot finish writing {}: {}", path, std::strerror(err)));
    }
    if (::rename(temp.c_str(), target.c_str()) != 0) {
        const int err = errno; ::unlink(temp.c_str());
        return fail(std::format("Cannot replace {}: {}", path, std::strerror(err)));
    }
    return true;
}

Result<std::string> list_directory(const std::string& path) {
    std::error_code ec;
    fs::directory_iterator it(path, ec);
    if (ec) return fail(std::format("Cannot open folder {}: {}", path, ec.message()));

    std::string json = "[";
    bool first = true;
    for (; it != fs::directory_iterator{}; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        const std::string full = it->path().string();
        if (name == ".git" || name == ".DS_Store") continue;
        if (!is_valid_utf8(name) || !is_valid_utf8(full)) continue;
        std::error_code ignore;
        const bool is_dir = it->is_directory(ignore);

        if (!first) json += ',';
        first = false;
        json += std::format(R"({{"name":"{}","path":"{}","is_dir":{}}})", json_escape(name), json_escape(full), is_dir ? "true" : "false");
    }
    if (ec) return fail(std::format("Error while reading folder {}: {}", path, ec.message()));
    json += ']';
    return json;
}

// --- USER CONFIG (theme / font / font size) stored with nlohmann::json ---
fs::path config_file_path() {
    const char* home_env = std::getenv("HOME");
    const fs::path home = (home_env && *home_env) ? fs::path(home_env) : fs::path(".");
#ifdef __APPLE__
    return home / "Library" / "Application Support" / "newCodeEdit" / "config.json";
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return fs::path(xdg) / "newCodeEdit" / "config.json";
    return home / ".config" / "newCodeEdit" / "config.json";
#endif
}

json default_config() {
    return json{{"theme", "dark-plus"}, {"font", "auto"}, {"fontSize", 14}};
}

bool is_safe_id(const std::string& s) {
    if (s.empty() || s.size() > 40) return false;
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (!(std::isalnum(c) || c == '-' || c == '_')) return false;
    }
    return true;
}

// Copies only valid, sanitized values from `src` into `cfg`.
void merge_valid_config(json& cfg, const json& src) {
    if (!src.is_object()) return;
    for (const char* key : {"theme", "font"}) {
        if (src.contains(key) && src[key].is_string()) {
            const std::string v = src[key].get<std::string>();
            if (is_safe_id(v)) cfg[key] = v;
        }
    }
    if (src.contains("fontSize") && src["fontSize"].is_number_integer()) {
        const long long v = src["fontSize"].get<long long>();
        if (v >= 8 && v <= 48) cfg["fontSize"] = v;
    }
}

json load_config_json() {
    json cfg = default_config();
    try {
        std::ifstream in(config_file_path());
        if (in) {
            const json loaded = json::parse(in, nullptr, false);
            merge_valid_config(cfg, loaded);
        }
    } catch (...) {
        // Corrupt or unreadable config: fall back to defaults.
    }
    return cfg;
}

Result<bool> save_config(const std::string& payload) {
    const json incoming = json::parse(payload, nullptr, false);
    if (!incoming.is_object()) return fail("Invalid settings payload.");

    json cfg = load_config_json();
    merge_valid_config(cfg, incoming);

    const fs::path file = config_file_path();
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (ec) return fail(std::format("Cannot create config folder {}: {}", file.parent_path().string(), ec.message()));
    return atomic_save(file.string(), cfg.dump(2) + "\n");
}

// ==== END HELPERS ===========================================================

// --- HTML / JS / CSS PAYLOAD ---
static constexpr std::string_view ui_html = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title>::: newCodeEdit - sfxpop.com :::</title>
    <style>
        :root {
            --bg: #1e1e1e; --panel: #252526; --border: #333333;
            --btn: #333333; --btn-hover: #444444; --btn-fg: #ffffff;
            --fg: #cccccc; --strong: #ffffff; --muted: #888888;
            --accent: #3794ff; --sel: #094771; --hover: #37373d;
            --menu-border: #454545; --close-bg: #3a3d41;
        }
        body, html { margin: 0; padding: 0; height: 100%; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif; background: var(--bg); color: var(--fg); overflow: hidden; }
        #toolbar { height: 40px; background: var(--panel); display: flex; align-items: center; padding: 0 10px; border-bottom: 1px solid var(--border); position: relative; }
        button { background: var(--btn); color: var(--btn-fg); border: none; padding: 6px 12px; margin-right: 8px; border-radius: 4px; cursor: pointer; font-size: 12px; user-select: none; }
        button:hover { background: var(--btn-hover); }
        button:focus-visible, .tree-item:focus-visible { outline: 2px solid var(--accent); outline-offset: -2px; }

        /* Theme / Font / Size selectors */
        .tb-select { flex-shrink: 0; background: var(--btn); color: var(--btn-fg); border: none; border-radius: 4px; padding: 5px 6px; margin-right: 8px; font-size: 12px; cursor: pointer; max-width: 170px; }
        .tb-select:hover { background: var(--btn-hover); }
        .tb-select:focus-visible { outline: 2px solid var(--accent); outline-offset: -2px; }
        
        /* File Menu Dropdown */
        .menu-container { position: relative; display: inline-block; }
        #file-menu-dropdown {
            position: absolute;
            top: calc(100% + 4px);
            left: 0;
            z-index: 10001;
            background: var(--panel);
            border: 1px solid var(--menu-border);
            box-shadow: 0 4px 12px rgba(0, 0, 0, 0.5);
            border-radius: 4px;
            padding: 4px 0;
            min-width: 160px;
        }
        .menu-item {
            padding: 6px 14px;
            font-size: 13px;
            color: var(--fg);
            cursor: pointer;
            user-select: none;
            white-space: nowrap;
        }
        .menu-item:hover {
            background: var(--sel);
            color: var(--strong);
        }

        .menu-sep { height: 1px; background: var(--menu-border); margin: 4px 0; }

        #status { margin-left: 12px; min-width: 0; flex: 1; font-size: 13px; color: var(--muted); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
        #status.error { color: #f48771; }

        /* Close Document Button at Upper-Right Corner */
        #btn-close-doc {
            margin-left: auto;
            margin-right: 0;
            background: var(--close-bg);
            color: var(--fg);
            font-weight: 500;
        }
        #btn-close-doc:hover {
            background: #c72e0f;
            color: #ffffff;
        }

        #app { display: flex; height: calc(100vh - 41px); }
        #sidebar-col { width: 250px; flex-shrink: 0; min-height: 0; display: flex; flex-direction: column; background: var(--panel); border-right: 1px solid var(--border); }
        #sidebar-head { flex-shrink: 0; padding: 8px 10px; border-bottom: 1px solid var(--border); }
        #btn-refresh { width: 100%; margin-right: 0; }
        #sidebar { flex: 1; min-height: 0; overflow-y: auto; padding: 10px 0; }
        #splitter { width: 5px; margin-left: -3px; flex-shrink: 0; position: relative; z-index: 5; cursor: col-resize; background: transparent; touch-action: none; }
        #splitter:hover, #splitter.dragging { background: var(--accent); }
        #editor-container { flex: 1; min-width: 0; position: relative; }
        .tree-item { padding: 4px 10px 4px 20px; cursor: pointer; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; font-size: 13px; user-select: none; }
        .tree-item:hover { background: var(--hover); }
        .tree-item.active { background: var(--sel); color: var(--strong); }
        .tree-item.folder { font-weight: bold; color: var(--strong); }
        .breakpoint-glyph { background-color: #e51400; border-radius: 50%; width: 10px !important; height: 10px !important; margin-top: 4px; margin-left: 4px; }

        /* Right-click context menu for sidebar file items */
        #file-context-menu {
            position: fixed;
            z-index: 10000;
            background: var(--panel);
            border: 1px solid var(--menu-border);
            box-shadow: 0 2px 8px rgba(0, 0, 0, 0.5);
            border-radius: 4px;
            padding: 4px 0;
            min-width: 190px;
        }
        .ctx-item {
            padding: 6px 14px;
            font-size: 13px;
            color: var(--fg);
            cursor: pointer;
            user-select: none;
        }
        .ctx-item:hover {
            background: var(--sel);
            color: var(--strong);
        }
    </style>
</head>
<body>
    <div id="toolbar">
        <div class="menu-container" id="file-menu-container">
            <button id="btn-file-menu" onclick="toggleFileMenu(event)">File &#9662;</button>
            <div id="file-menu-dropdown" hidden>
                <div class="menu-item" id="menu-new" onclick="onFileMenuAction('new')">New File</div>
                <div class="menu-item" id="menu-open" onclick="onFileMenuAction('open')">Open File</div>
                <div class="menu-item" id="menu-folder" onclick="onFileMenuAction('folder')">Open Folder</div>
                <div class="menu-item" id="menu-save" onclick="onFileMenuAction('save')">Save(Cmd+S)</div>
                <div class="menu-sep" id="menu-sep"></div>
                <div class="menu-item" id="menu-exit" onclick="onFileMenuAction('exit')">Exit</div>
            </div>
        </div>
        <button id="btn-undo" onclick="doUndo()" title="Undo (Cmd/Ctrl+Z)">&#8630; Undo</button>
        <button id="btn-redo" onclick="doRedo()" title="Redo (Cmd+Shift+Z / Ctrl+Y)">&#8631; Redo</button>
        <select id="sel-theme" class="tb-select" title="Theme" onchange="onAppearanceChange()"></select>
        <select id="sel-font" class="tb-select" title="Font" onchange="onAppearanceChange()"></select>
        <select id="sel-size" class="tb-select" title="Font size" onchange="onAppearanceChange()"></select>
        <span id="status">No file opened</span>
        <button id="btn-close-doc" onclick="closeCurrentDocument()" title="Save and close current document">&#10005; Close</button>
    </div>
    <div id="app">
        <div id="sidebar-col">
            <div id="sidebar-head">
                <button id="btn-refresh" onclick="refreshTree()" title="Refresh the file list">&#10227; Refresh</button>
            </div>
            <div id="sidebar"></div>
        </div>
        <div id="splitter" title="Drag to resize"></div>
        <div id="editor-container"></div>
    </div>
    <div id="file-context-menu" hidden>
        <div class="ctx-item" id="ctx-open-new-win">Open Code in New Window</div>
    </div>

    <script src="/Applications/newCodeEdit.app/Contents/Resources/js/require.min.js"></script>
    <script>
        const IS_MINIMAL = false;
        const INITIAL_PATH = "";
        const INITIAL_CONTENT = "";
        const INITIAL_CONFIG = {};

        const MONACO_BASE = '/Applications/newCodeEdit.app/Contents/Resources/';
        const BREAKPOINT_CLASS = 'breakpoint-glyph';

        let editor = null;
        let activePath = '';
        let statusTimer = 0;
        let contextTargetFilePath = '';
        let currentRoot = '';
        const expandedDirs = new Set();
        const openFiles = new Map();

        const statusEl = document.getElementById('status');
        const sidebar = document.getElementById('sidebar');
        const ctxMenu = document.getElementById('file-context-menu');
        const fileMenuDropdown = document.getElementById('file-menu-dropdown');
        const fileMenuContainer = document.getElementById('file-menu-container');

        function rpc(name, ...args) { return window.saucer.call(name, args); }
        function showStatus(text, isError = false) { statusEl.textContent = text; statusEl.classList.toggle('error', isError); }
        function reportError(prefix, err) { showStatus(prefix + ': ' + err, true); }
        function isDirty(entry) { return entry && entry.model.getAlternativeVersionId() !== entry.savedVersion; }
        function updateStatus() {
            const entry = openFiles.get(activePath);
            if (!entry) return showStatus('No file opened');
            showStatus((activePath || 'New File') + (isDirty(entry) ? '  \u25CF  unsaved changes' : ''));
        }
        function flashStatus(text) { showStatus(text); clearTimeout(statusTimer); statusTimer = setTimeout(updateStatus, 2000); }

        // ---------- UNDO / REDO (Monaco keeps an unlimited per-file history) ----------
        function doUndo() {
            if (!editor) return;
            editor.focus();
            editor.trigger('toolbar', 'undo', null);
        }
        function doRedo() {
            if (!editor) return;
            editor.focus();
            editor.trigger('toolbar', 'redo', null);
        }

        // ---------- THEMES / FONTS / SIZES ----------
        function uiVars(bg, panel, border, btn, btnHover, fg, strong, muted, accent, sel, hover, menuBorder, closeBg, btnFg) {
            return { 'bg': bg, 'panel': panel, 'border': border, 'btn': btn, 'btn-hover': btnHover, 'fg': fg, 'strong': strong,
                     'muted': muted, 'accent': accent, 'sel': sel, 'hover': hover, 'menu-border': menuBorder, 'close-bg': closeBg, 'btn-fg': btnFg };
        }
        function edColors(bg, fg, lineNo, selection, lineHl, cursor) {
            return { 'editor.background': bg, 'editor.foreground': fg, 'editorLineNumber.foreground': lineNo,
                     'editor.selectionBackground': selection, 'editor.lineHighlightBackground': lineHl,
                     'editorCursor.foreground': cursor, 'editorGutter.background': bg };
        }
        function tokenRules(comment, keyword, str, num, type) {
            return [
                { token: 'comment', foreground: comment, fontStyle: 'italic' },
                { token: 'keyword', foreground: keyword },
                { token: 'string', foreground: str },
                { token: 'number', foreground: num },
                { token: 'type', foreground: type },
                { token: 'type.identifier', foreground: type },
                { token: 'predefined', foreground: type }
            ];
        }

        const THEMES = {
            'dark-plus': { label: 'Dark+ (Default)', base: 'vs-dark',
                ui: uiVars('#1e1e1e', '#252526', '#333333', '#333333', '#444444', '#cccccc', '#ffffff', '#888888', '#3794ff', '#094771', '#37373d', '#454545', '#3a3d41', '#ffffff'),
                colors: edColors('#1e1e1e', '#d4d4d4', '#858585', '#264f78', '#2a2d2e', '#aeafad'), rules: [] },
            'light': { label: 'Daylight', base: 'vs',
                ui: uiVars('#ffffff', '#f3f3f3', '#dddddd', '#e1e1e1', '#d0d0d0', '#333333', '#000000', '#666666', '#0066cc', '#cce4ff', '#e8e8e8', '#c8c8c8', '#e1e1e1', '#222222'),
                colors: edColors('#ffffff', '#333333', '#999999', '#add6ff', '#f5f5f5', '#000000'), rules: tokenRules('008000', '0000ff', 'a31515', '098658', '267f99') },
            'dracula': { label: 'Dracula', base: 'vs-dark',
                ui: uiVars('#282a36', '#21222c', '#44475a', '#44475a', '#6272a4', '#f8f8f2', '#ffffff', '#6272a4', '#bd93f9', '#44475a', '#343746', '#44475a', '#44475a', '#f8f8f2'),
                colors: edColors('#282a36', '#f8f8f2', '#6272a4', '#44475a', '#2f3142', '#f8f8f0'), rules: tokenRules('6272a4', 'ff79c6', 'f1fa8c', 'bd93f9', '8be9fd') },
            'monokai': { label: 'Monokai Pro', base: 'vs-dark',
                ui: uiVars('#272822', '#1e1f1c', '#3e3d32', '#3e3d32', '#49483e', '#f8f8f2', '#ffffff', '#90908a', '#a6e22e', '#49483e', '#3e3d32', '#555549', '#3e3d32', '#f8f8f2'),
                colors: edColors('#272822', '#f8f8f2', '#90908a', '#49483e', '#3e3d32', '#f8f8f0'), rules: tokenRules('75715e', 'f92672', 'e6db74', 'ae81ff', '66d9ef') },
            'nord': { label: 'Nord Frost', base: 'vs-dark',
                ui: uiVars('#2e3440', '#3b4252', '#434c5e', '#434c5e', '#4c566a', '#d8dee9', '#eceff4', '#8f9bb3', '#88c0d0', '#4c566a', '#3f4758', '#4c566a', '#434c5e', '#eceff4'),
                colors: edColors('#2e3440', '#d8dee9', '#616e88', '#434c5e', '#3b4252', '#d8dee9'), rules: tokenRules('616e88', '81a1c1', 'a3be8c', 'b48ead', '8fbcbb') },
            'solarized-dark': { label: 'Solarized Dark', base: 'vs-dark',
                ui: uiVars('#002b36', '#073642', '#0a4a5a', '#0a4a5a', '#115e70', '#93a1a1', '#eee8d5', '#657b83', '#b58900', '#0a4a5a', '#073642', '#115e70', '#0a4a5a', '#eee8d5'),
                colors: edColors('#002b36', '#93a1a1', '#586e75', '#094b5a', '#073642', '#93a1a1'), rules: tokenRules('586e75', '859900', '2aa198', 'd33682', 'b58900') },
            'tokyo-night': { label: 'Tokyo Night', base: 'vs-dark',
                ui: uiVars('#1a1b26', '#16161e', '#292e42', '#292e42', '#3b4261', '#a9b1d6', '#c0caf5', '#565f89', '#7aa2f7', '#283457', '#202336', '#3b4261', '#292e42', '#c0caf5'),
                colors: edColors('#1a1b26', '#a9b1d6', '#3b4261', '#283457', '#202336', '#c0caf5'), rules: tokenRules('565f89', 'bb9af7', '9ece6a', 'ff9e64', '2ac3de') },
            'cyberpunk': { label: 'Cyberpunk Neon', base: 'vs-dark',
                ui: uiVars('#0d0221', '#150634', '#2a0e5c', '#2a0e5c', '#3d1a80', '#e0d7ff', '#ffffff', '#8a7fb8', '#ff2a6d', '#3d1a80', '#241046', '#4a2a8a', '#2a0e5c', '#ffffff'),
                colors: edColors('#0d0221', '#e0d7ff', '#6a5aa0', '#3d1a80', '#150634', '#ff2a6d'), rules: tokenRules('6a5aa0', 'ff2a6d', '05ffa1', 'ffb86c', '01cdfe') }
        };

        // Every stack ends with fonts that exist on macOS, Windows and Linux, then generic monospace.
        const FONTS = {
            'auto':       { label: 'Auto (Menlo / Consolas / DejaVu)', stack: 'Menlo, Monaco, Consolas, "DejaVu Sans Mono", "Liberation Mono", "Courier New", monospace' },
            'monaco':     { label: 'Monaco',            stack: 'Monaco, Menlo, Consolas, "DejaVu Sans Mono", "Liberation Mono", monospace' },
            'sfmono':     { label: 'SF Mono',           stack: '"SF Mono", SFMono-Regular, Menlo, Consolas, "DejaVu Sans Mono", "Liberation Mono", monospace' },
            'consolas':   { label: 'Consolas',          stack: 'Consolas, "DejaVu Sans Mono", Menlo, "Liberation Mono", monospace' },
            'dejavu':     { label: 'DejaVu Sans Mono',  stack: '"DejaVu Sans Mono", "Liberation Mono", Menlo, Consolas, monospace' },
            'liberation': { label: 'Liberation Mono',   stack: '"Liberation Mono", "Courier New", Menlo, Consolas, "DejaVu Sans Mono", monospace' },
            'courier':    { label: 'Courier New',       stack: '"Courier New", Courier, "Liberation Mono", Menlo, monospace' },
            'lucida':     { label: 'Lucida Console',    stack: '"Lucida Console", "Lucida Sans Typewriter", "DejaVu Sans Mono", Menlo, monospace' },
            'coding':     { label: 'Fira Code / Cascadia / JetBrains', stack: '"Fira Code", "Cascadia Code", "JetBrains Mono", Menlo, Consolas, "DejaVu Sans Mono", monospace' }
        };
        const FONT_SIZES = [10, 11, 12, 13, 14, 15, 16, 18, 20, 22, 24, 28];

        const cfg = Object.assign({ theme: 'dark-plus', font: 'auto', fontSize: 14 }, INITIAL_CONFIG);
        if (!THEMES[cfg.theme]) cfg.theme = 'dark-plus';
        if (!FONTS[cfg.font]) cfg.font = 'auto';
        if (!Number.isInteger(cfg.fontSize) || cfg.fontSize < 8 || cfg.fontSize > 48) cfg.fontSize = 14;

        const selTheme = document.getElementById('sel-theme');
        const selFont = document.getElementById('sel-font');
        const selSize = document.getElementById('sel-size');

        function fillSelect(sel, items, current) {
            sel.replaceChildren();
            for (const [value, label] of items) {
                const opt = document.createElement('option');
                opt.value = String(value);
                opt.textContent = label;
                sel.appendChild(opt);
            }
            sel.value = String(current);
        }

        function initAppearanceControls() {
            fillSelect(selTheme, Object.entries(THEMES).map(([id, t]) => [id, '\uD83C\uDFA8 ' + t.label]), cfg.theme);
            fillSelect(selFont, Object.entries(FONTS).map(([id, f]) => [id, 'Aa ' + f.label]), cfg.font);
            const sizes = FONT_SIZES.includes(cfg.fontSize) ? FONT_SIZES : [...FONT_SIZES, cfg.fontSize].sort((a, b) => a - b);
            fillSelect(selSize, sizes.map((s) => [s, s + ' px']), cfg.fontSize);
        }

        function applyUiTheme() {
            const t = THEMES[cfg.theme];
            const rootStyle = document.documentElement.style;
            for (const [k, v] of Object.entries(t.ui)) rootStyle.setProperty('--' + k, v);
            rootStyle.colorScheme = t.base === 'vs' ? 'light' : 'dark';
        }

        function applyEditorAppearance() {
            if (!editor) return;
            monaco.editor.setTheme(cfg.theme);
            editor.updateOptions({ fontFamily: FONTS[cfg.font].stack, fontSize: cfg.fontSize });
        }

        async function onAppearanceChange() {
            cfg.theme = selTheme.value;
            cfg.font = selFont.value;
            cfg.fontSize = parseInt(selSize.value, 10);
            applyUiTheme();
            applyEditorAppearance();
            try { await rpc('save_config', JSON.stringify(cfg)); }
            catch (err) { reportError('Could not save settings', err); }
            if (editor) editor.focus();
        }

        initAppearanceControls();
        applyUiTheme();
        // ---------- END THEMES / FONTS / SIZES ----------

        function toggleFileMenu(e) {
            e.stopPropagation();
            hideFileContextMenu();
            fileMenuDropdown.hidden = !fileMenuDropdown.hidden;
        }

        function hideFileMenu() {
            fileMenuDropdown.hidden = true;
        }

        async function onFileMenuAction(action) {
            hideFileMenu();
            if (action === 'new') await newFile();
            else if (action === 'open') await openFile();
            else if (action === 'folder') await openFolder();
            else if (action === 'save') await saveCurrent();
            else if (action === 'exit') await exitApp();
        }

        // Saves every modified file, then quits the whole application.
        // If an untitled file's Save dialog is cancelled (or a save fails), the app stays open.
        async function exitApp() {
            await editorReady;
            try {
                for (const [p, entry] of openFiles) {
                    if (p !== '' && isDirty(entry)) {
                        const version = entry.model.getAlternativeVersionId();
                        await rpc('atomic_save', p, entry.model.getValue());
                        entry.savedVersion = version;
                    }
                }
                const current = openFiles.get(activePath);
                if (activePath === '' && current && isDirty(current)) {
                    await saveCurrent();
                    if (activePath === '') return; // Save dialog cancelled or failed: do not exit
                }
            } catch (err) { reportError('Could not save before exit', err); return; }
            try { await rpc('exit_app'); } catch (err) { reportError('Could not exit', err); }
        }

        function applyLanguageFromPath(model, path) {
            if (!path) return;
            const ext = path.includes('.') ? '.' + path.split('.').pop().toLowerCase() : '';
            const langs = monaco.languages.getLanguages();
            for (const l of langs) {
                if (l.extensions && l.extensions.some(e => e.toLowerCase() === ext)) {
                    monaco.editor.setModelLanguage(model, l.id);
                    return;
                }
            }
        }

        function showFileContextMenu(x, y, filePath) {
            hideFileMenu();
            contextTargetFilePath = filePath;
            ctxMenu.hidden = false;
            const rect = ctxMenu.getBoundingClientRect();
            const maxX = window.innerWidth - rect.width - 4;
            const maxY = window.innerHeight - rect.height - 4;
            ctxMenu.style.left = Math.max(4, Math.min(x, maxX)) + 'px';
            ctxMenu.style.top = Math.max(4, Math.min(y, maxY)) + 'px';
        }

        function hideFileContextMenu() {
            ctxMenu.hidden = true;
            contextTargetFilePath = '';
        }

        document.getElementById('ctx-open-new-win').addEventListener('click', async () => {
            const targetPath = contextTargetFilePath;
            hideFileContextMenu();
            if (!targetPath) return;
            try {
                let content;
                if (openFiles.has(targetPath)) {
                    content = openFiles.get(targetPath).model.getValue();
                } else {
                    content = await rpc('read_file', targetPath);
                }
                await rpc('open_new_window', content, targetPath);
            } catch (err) {
                reportError('Could not open in new window', err);
            }
        });

        window.addEventListener('click', (e) => {
            if (!ctxMenu.hidden && !ctxMenu.contains(e.target)) {
                hideFileContextMenu();
            }
            if (!fileMenuDropdown.hidden && !fileMenuContainer.contains(e.target)) {
                hideFileMenu();
            }
        });

        window.addEventListener('keydown', (e) => {
            if (e.key === 'Escape') {
                if (!ctxMenu.hidden) hideFileContextMenu();
                if (!fileMenuDropdown.hidden) hideFileMenu();
            }
        });

        window.MonacoEnvironment = {
            getWorkerUrl: () => 'data:text/javascript;charset=utf-8,' + encodeURIComponent("self.MonacoEnvironment = { baseUrl: '" + MONACO_BASE + "/' };importScripts('" + MONACO_BASE + "/vs/base/worker/workerMain.js');")
        };

        const editorReady = new Promise((resolve) => {
            if (typeof require === 'undefined') { showStatus('The editor could not be loaded. Check your internet connection.', true); return; }
            require.config({ paths: { vs: MONACO_BASE + '/vs' } });
            require.onError = () => showStatus('The editor could not be loaded.', true);
            require(['vs/editor/editor.main'], function () {
                for (const [id, t] of Object.entries(THEMES)) {
                    monaco.editor.defineTheme(id, { base: t.base, inherit: true, rules: t.rules, colors: t.colors });
                }
                editor = monaco.editor.create(document.getElementById('editor-container'), {
                    value: IS_MINIMAL ? INITIAL_CONTENT : '// newCodeEdit\n// Open a file or folder to begin.',
                    language: 'cpp', theme: cfg.theme, automaticLayout: true, glyphMargin: true, wordWrap: 'on',
                    fontFamily: FONTS[cfg.font].stack, fontSize: cfg.fontSize
                });

                const initialModel = editor.getModel();
                initialModel.onDidChangeContent(updateStatus);

                editor.onMouseDown(function (e) {
                    hideFileContextMenu();
                    hideFileMenu();
                    if (e.target.type === monaco.editor.MouseTargetType.GUTTER_GLYPH_MARGIN) toggleBreakpoint(e.target.position.lineNumber);
                });

                // Feature: Context Menu for Popout Window inside Editor
                editor.addAction({
                    id: 'open-new-window',
                    label: 'Open Code in New Window',
                    contextMenuGroupId: 'navigation',
                    contextMenuOrder: 1.5,
                    run: function(ed) { rpc('open_new_window', ed.getValue(), activePath); }
                });

                resolve();

                if (IS_MINIMAL) {
                    document.getElementById('sidebar-col').style.display = 'none';
                    document.getElementById('splitter').style.display = 'none';
                    document.getElementById('sidebar').style.display = 'none';
                    document.getElementById('menu-new').style.display = 'none';
                    document.getElementById('menu-open').style.display = 'none';
                    document.getElementById('menu-folder').style.display = 'none';
                    document.getElementById('btn-close-doc').title = 'Save and close this window';
                    
                    activePath = INITIAL_PATH;
                    applyLanguageFromPath(initialModel, activePath);
                    openFiles.set(activePath, { model: initialModel, viewState: null, savedVersion: initialModel.getAlternativeVersionId() });
                    updateStatus();
                } else if (INITIAL_PATH !== "") {
                    loadFile(INITIAL_PATH);
                }
            });
        });

        window.addEventListener('keydown', function (e) {
            if ((e.metaKey || e.ctrlKey) && !e.shiftKey && !e.altKey && e.key.toLowerCase() === 's') {
                e.preventDefault(); saveCurrent();
            }
        }, true);

        function toggleBreakpoint(line) {
            const entry = openFiles.get(activePath);
            if (!entry) return;
            const model = entry.model;
            const existing = model.getLineDecorations(line).filter((d) => d.options.glyphMarginClassName === BREAKPOINT_CLASS);
            if (existing.length > 0) {
                model.deltaDecorations(existing.map((d) => d.id), []);
            } else {
                model.deltaDecorations([], [{ range: new monaco.Range(line, 1, line, 1), options: { glyphMarginClassName: BREAKPOINT_CLASS, stickiness: monaco.editor.TrackedRangeStickiness.NeverGrowsWhenTypingAtEdges } }]);
            }
        }

        async function newFile() {
            if (activePath && isDirty(openFiles.get(activePath))) {
                await saveCurrent();
            }
            await editorReady;
            const leaving = openFiles.get(activePath);
            if (leaving) leaving.viewState = editor.saveViewState();

            activePath = '';
            const model = monaco.editor.createModel('', 'cpp');
            model.onDidChangeContent(updateStatus);
            editor.setModel(model);
            openFiles.set('', { model, viewState: null, savedVersion: model.getAlternativeVersionId() });
            editor.focus();
            updateStatus();
            markActiveRow();
        }

        async function closeCurrentDocument() {
            await editorReady;

            // "Open Code in New Window" popout: save the code, then close this window.
            if (IS_MINIMAL) {
                await saveCurrent();
                const current = openFiles.get(activePath);
                if (!activePath || (current && isDirty(current))) return; // save cancelled or failed: keep window open
                try { await rpc('close_window'); } catch (err) { reportError('Could not close window', err); }
                return;
            }

            const entry = openFiles.get(activePath);
            if (entry) {
                // Automatically save the current open document before closing it
                if (activePath !== '' || isDirty(entry)) {
                    await saveCurrent();
                }
                const closedPath = activePath;
                const oldModel = entry.model;
                activePath = '';
                const blankModel = monaco.editor.createModel('', 'cpp');
                blankModel.onDidChangeContent(updateStatus);
                editor.setModel(blankModel);
                openFiles.delete(closedPath);
                if (oldModel && oldModel !== blankModel) {
                    oldModel.dispose();
                }
                markActiveRow();
                showStatus('No file opened');
            } else {
                activePath = '';
                const blankModel = monaco.editor.createModel('', 'cpp');
                blankModel.onDidChangeContent(updateStatus);
                editor.setModel(blankModel);
                markActiveRow();
                showStatus('No file opened');
            }
        }

        async function openFile() {
            try { const path = await rpc('open_file_dialog'); if (path) await loadFile(path); } catch (err) { reportError('Could not open file', err); }
        }

        // Window title of the main window: "newCodeEdit", or "newCodeEdit - <opened folder name>".
        function updateWindowTitle() {
            if (IS_MINIMAL) return;
            const name = currentRoot ? currentRoot.split(/[\\/]/).filter(Boolean).pop() : '';
            Promise.resolve(rpc('set_window_title', name ? 'newCodeEdit - ' + name : 'newCodeEdit')).catch(() => {});
        }

        async function openFolder() {
            try {
                const path = await rpc('open_folder_dialog');
                if (!path) return;
                currentRoot = path;
                updateWindowTitle();
                expandedDirs.clear();
                sidebar.replaceChildren();
                await renderDir(path, sidebar, 0);
            } catch (err) { reportError('Could not open folder', err); }
        }

        // Re-reads the opened folder from disk, keeping expanded sub-folders and scroll position.
        async function refreshTree() {
            if (IS_MINIMAL) return;
            if (!currentRoot) { flashStatus('Open a folder first'); return; }
            const top = sidebar.scrollTop;
            sidebar.replaceChildren();
            await renderDir(currentRoot, sidebar, 0);
            markActiveRow();
            sidebar.scrollTop = top;
            flashStatus('File list refreshed');
        }

        async function loadFile(path) {
            await editorReady;
            if (!openFiles.has(path)) {
                let content;
                try { content = await rpc('read_file', path); } catch (err) { reportError('Could not open ' + path, err); return; }
                if (!openFiles.has(path)) {
                    const model = monaco.editor.createModel(content, undefined, monaco.Uri.file(path));
                    applyLanguageFromPath(model, path);
                    model.onDidChangeContent(updateStatus);
                    openFiles.set(path, { model, viewState: null, savedVersion: model.getAlternativeVersionId() });
                }
            }
            const leaving = openFiles.get(activePath);
            if (leaving) leaving.viewState = editor.saveViewState();
            const entry = openFiles.get(path);
            activePath = path;
            editor.setModel(entry.model);
            if (entry.viewState) editor.restoreViewState(entry.viewState);
            editor.focus(); updateStatus(); markActiveRow();
        }

        async function saveCurrent() {
            await editorReady;
            let entry = openFiles.get(activePath);

            // If the user is editing the initial scratch buffer or a new untitled file
            if (!entry) {
                const model = editor.getModel();
                entry = { model, viewState: null, savedVersion: model.getAlternativeVersionId() };
                openFiles.set(activePath, entry);
            }

            let targetPath = activePath;
            if (!targetPath) {
                try {
                    const chosenPath = await rpc('save_file_dialog');
                    if (!chosenPath) return; // User cancelled Save dialog
                    targetPath = chosenPath;
                } catch (err) {
                    reportError('Could not open save dialog', err);
                    return;
                }
            }

            const version = entry.model.getAlternativeVersionId();
            try {
                await rpc('atomic_save', targetPath, entry.model.getValue());
                entry.savedVersion = version;
                if (activePath !== targetPath) {
                    openFiles.delete(activePath);
                    activePath = targetPath;
                    openFiles.set(activePath, entry);
                    applyLanguageFromPath(entry.model, activePath);
                    markActiveRow();
                }
                flashStatus(activePath + '  (Saved)');
            } catch (err) { reportError('Could not save ' + targetPath, err); }
        }

        // ---------- RESIZABLE LEFT COLUMN (drag the vertical border) ----------
        (function initSplitter() {
            const splitter = document.getElementById('splitter');
            const col = document.getElementById('sidebar-col');
            const MIN_COL = 120, MIN_EDITOR = 200;
            const maxCol = () => Math.max(MIN_COL, window.innerWidth - MIN_EDITOR);
            const setWidth = (w) => { col.style.width = Math.max(MIN_COL, Math.min(w, maxCol())) + 'px'; };
            let dragging = false;

            splitter.addEventListener('pointerdown', (e) => {
                dragging = true;
                splitter.setPointerCapture(e.pointerId);
                splitter.classList.add('dragging');
                document.body.style.userSelect = 'none';
                e.preventDefault();
            });
            splitter.addEventListener('pointermove', (e) => {
                if (!dragging) return;
                setWidth(e.clientX - col.getBoundingClientRect().left);
            });
            const endDrag = (e) => {
                if (!dragging) return;
                dragging = false;
                splitter.classList.remove('dragging');
                document.body.style.userSelect = '';
                try { splitter.releasePointerCapture(e.pointerId); } catch (_) {}
            };
            splitter.addEventListener('pointerup', endDrag);
            splitter.addEventListener('pointercancel', endDrag);
            window.addEventListener('resize', () => {
                const w = col.getBoundingClientRect().width;
                if (w > maxCol()) setWidth(w);
            });
        })();

        function markActiveRow() {
            for (const row of sidebar.querySelectorAll('.tree-item.file')) {
                row.classList.toggle('active', row.dataset.path === activePath);
            }
        }

        async function renderDir(dirPath, container, depth) {
            let nodes;
            try { nodes = JSON.parse(await rpc('list_directory', dirPath)); } catch (err) { reportError('Could not read folder', err); return; }
            nodes.sort((a, b) => Number(b.is_dir) - Number(a.is_dir) || a.name.localeCompare(b.name));

            for (const node of nodes) {
                const row = document.createElement('div');
                row.className = 'tree-item ' + (node.is_dir ? 'folder' : 'file');
                row.style.paddingLeft = (10 + depth * 14) + 'px'; row.tabIndex = 0; row.dataset.path = node.path;
                container.appendChild(row);
                row.addEventListener('keydown', (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); row.click(); } });

                if (!node.is_dir) {
                    row.textContent = '\uD83D\uDCC4 ' + node.name;
                    row.addEventListener('click', () => loadFile(node.path));
                    row.addEventListener('contextmenu', (e) => {
                        e.preventDefault();
                        e.stopPropagation();
                        showFileContextMenu(e.clientX, e.clientY, node.path);
                    });
                    continue;
                }

                const children = document.createElement('div'); children.hidden = true; container.appendChild(children);
                const label = () => { row.textContent = (children.hidden ? '\u25B8 ' : '\u25BE ') + '\uD83D\uDCC1 ' + node.name; };
                label();
                let loaded = false;
                // Re-open folders that were expanded before a refresh
                if (expandedDirs.has(node.path)) {
                    loaded = true;
                    children.hidden = false;
                    label();
                    await renderDir(node.path, children, depth + 1);
                }
                row.addEventListener('click', async () => {
                    if (!loaded) { loaded = true; await renderDir(node.path, children, depth + 1); markActiveRow(); }
                    children.hidden = !children.hidden; label();
                    if (children.hidden) expandedDirs.delete(node.path); else expandedDirs.add(node.path);
                });
            }
        }
    </script>
</body>
</html>
)HTML";

// --- SAUCER WINDOW MANAGEMENT ---
using SaucerWindowPtr  = std::decay_t<decltype(saucer::window::create(nullptr).value())>;
using SaucerWebviewPtr = std::decay_t<decltype(saucer::smartview::create({.window = std::declval<SaucerWindowPtr>()}))>;

struct EditorWindow {
    SaucerWindowPtr window;
    SaucerWebviewPtr webview;
};
std::vector<EditorWindow> g_windows;
std::mutex g_windows_mutex;
std::atomic<int> g_window_counter{0};

void create_editor_window(saucer::application* app, bool minimal, const std::string& path, const std::string& code) {
    if (!app) return;
    // A file that is already open in a pop-out window is brought to the front instead of being opened again.
    if (minimal && !path.empty() && raise_existing_window_for(path)) return;
    const std::string check_openned_win = window_title_for(path);
#ifdef __APPLE__
    const std::set<void*> windows_before = (minimal && !path.empty()) ? snapshot_nswindows() : std::set<void*>{};
#endif
    g_has_created_any_window = true;
    auto window = saucer::window::create(app).value();
    auto webview = saucer::smartview::create({.window = window});
    window->set_title(minimal && !path.empty() ? (check_openned_win) : "newCodeEdit");
    window->set_size({.w = 1000, .h = 700});
    webview->expose("open_folder_dialog", [] { return open_folder_dialog(); });
    webview->expose("open_file_dialog", [] { return open_file_dialog(); });
    webview->expose("save_file_dialog", [] { return save_file_dialog(); });
    webview->expose("list_directory", [](const std::string& p) { 
        return guarded<std::string>([&] { return list_directory(p); }); 
    });
    webview->expose("read_file", [](const std::string& p) { 
        return guarded<std::string>([&] { return read_file(p); }); 
    });
    webview->expose("atomic_save", [](const std::string& p, const std::string& content) {
        return guarded<bool>([&] { return atomic_save(p, content); });
    });
    webview->expose("open_new_window", [app](const std::string& content, const std::string& p) {
        create_editor_window(app, true, p, content);
    });
    webview->expose("save_config", [](const std::string& payload) {
        return guarded<bool>([&] { return save_config(payload); });
    });
    // Closes this editor window (used by the popout window's Close button after it has saved).
    // A raw pointer is captured on purpose: capturing the shared window would create a reference cycle.
    auto* raw_window = window.get();
    webview->expose("close_window", [raw_window] {
        raw_window->close();
    });
    // Lets the main window show the opened folder in its title: "newCodeEdit - <folder name>".
    webview->expose("set_window_title", [raw_window](const std::string& title) {
        raw_window->set_title(title);
    });
    // Quits the whole application (used by File > Exit after it has saved modified files).
    webview->expose("exit_app", [] {
        cleanup_temp_files_and_exit();
    });

    std::string html = std::string(ui_html);

    // Inject the saved theme / font / font size so they are applied at startup.
    {
        std::string cfg_json = load_config_json().dump();
        replace_all(cfg_json, "</", "<\\/");
        replace_all(html, "const INITIAL_CONFIG = {};", "const INITIAL_CONFIG = " + cfg_json + ";");
    }

    if (minimal) {
        replace_all(html, "const IS_MINIMAL = false;", "const IS_MINIMAL = true;");
    }
    if (!path.empty()) {
        replace_all(html, "const INITIAL_PATH = \"\";", "const INITIAL_PATH = \"" + js_string_escape(path) + "\";");
    }
    // File content is injected LAST and with '<' escaped, so neither "</script>" inside the file
    // nor text that looks like one of our placeholders can corrupt the page or get substituted.
    if (minimal) {
        replace_all(html, "const INITIAL_CONTENT = \"\";", "const INITIAL_CONTENT = \"" + js_string_escape(code) + "\";");
    }

    std::error_code ec;
#ifdef _WIN32
    const int pid = ::_getpid();
#else
    const int pid = ::getpid();
#endif
    const fs::path ui_path = fs::temp_directory_path(ec) / std::format("newcode_ui_{}_{}.html", pid, ++g_window_counter);
    
    std::ofstream out(ui_path, std::ios::binary | std::ios::trunc);
    out << html;
    out.close();

    {
        std::lock_guard<std::mutex> lock(g_temp_files_mutex);
        g_temp_files.push_back(ui_path);
    }

    webview->set_url("file://" + ui_path.string());
    window->show();

#ifdef __APPLE__
    if (minimal && !path.empty()) register_popout_window(path, check_openned_win, windows_before);
    // Ensure system tray and Dock menu are active whenever any window shows
    init_cross_platform_system_tray();
    register_active_editor_nswindows();
    [NSApp activateIgnoringOtherApps:YES];
#endif
    
    std::lock_guard<std::mutex> lock(g_windows_mutex);
    g_windows.push_back(EditorWindow{std::move(window), std::move(webview)});
}

// Opens every incoming "Open With" file in its own independent minimal window
void process_pending_files() {
    if (!g_app) return;
    std::vector<std::string> files;
    {
        std::lock_guard<std::mutex> lock(g_pending_mutex);
        if (g_pending_files.empty()) return;
        files.swap(g_pending_files);
    }
    for (const auto& file_path : files) {
        auto res = read_file(file_path);
        std::string content = res.has_value() ? *res : ("// Error opening file:\n// " + res.error());
        create_editor_window(g_app, true, file_path, content);
    }
}

coco::stray start(saucer::application* app) {
    g_app = app;

    init_cross_platform_system_tray();

#ifdef __APPLE__
    if ([NSApp delegate]) {
        id del = [NSApp delegate];
        attach_open_handlers_to_delegate(del);
        [NSApp setDelegate:del];
    }
    set_app_icon_from_xpm(logo_xpm);

    // Drain any files already queued before start()
    process_pending_files();

    // Poll on the Cocoa main runloop for any incoming Finder "Open With" files
    // and open the main workspace window if the app was launched normally without files
    __block bool initial_check_done = false;
    [NSTimer scheduledTimerWithTimeInterval:0.08
                                    repeats:YES
                                      block:^(NSTimer*) {
                                          if (g_status_item == nil) {
                                              init_macos_system_tray(logo_xpm);
                                          }
                                          process_pending_files();
                                          if (!initial_check_done) {
                                              initial_check_done = true;
                                              if (!g_has_created_any_window.load()) {
                                                  create_editor_window(g_app, false, "", "");
                                              }
                                          }
                                      }];
#else
    process_pending_files();
    if (!g_has_created_any_window.load()) {
        create_editor_window(app, false, "", "");
    }
#endif

    co_await app->finish();
    cleanup_temp_files_and_exit();
}

int main(int argc, char* argv[]) {

#ifdef __APPLE__
    install_macos_lifecycle_hooks();
#endif

    // Accept all non-flag file arguments (e.g. from Linux %F or CLI)
    // Ignore macOS Finder's legacy -psn_0_... process serial number argument
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] != '-') {
            enqueue_incoming_path(argv[i]);
        }
    }

    auto app = saucer::application::create({.id = "dev.ronnie.newcodeedit"});
#ifdef __APPLE__
    if ([NSApp delegate]) {
        id del = [NSApp delegate];
        attach_open_handlers_to_delegate(del);
        [NSApp setDelegate:del];
    }
#endif

    return app->run(start);
}
