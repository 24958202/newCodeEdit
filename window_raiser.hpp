// window_raiser.hpp - find a running program's window by title and bring it to the front.
// C++23. One header, three implementations (Windows / macOS / Linux X11).
//
//   Windows : link nothing extra (user32 is linked by default).
//   macOS   : compile as Objective-C++ (.mm, or -x objective-c++);
//             link -framework Cocoa -framework ApplicationServices
//   Linux   : link -lX11   (X11 and XWayland windows only, see notes below)
//
// Usage:
//   WindowRaiser raiser;
//   if (!raiser.search_processes_exist_title("newCodeEdit - main.cpp")) { /* not running */ }
#pragma once

#include <string>
#include <string_view>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#elif defined(__APPLE__)
  #import <Cocoa/Cocoa.h>
  #import <ApplicationServices/ApplicationServices.h>
#else
  #include <X11/Xatom.h>
  #include <X11/Xlib.h>
  #include <X11/Xutil.h>
#endif

class WindowRaiser {
public:
    // Looks through all visible top-level windows of all running programs for one whose TITLE
    // CONTAINS `str_title` (case-sensitive). If found, restores it if minimized and brings it to
    // the front. Returns true if a window was found, false otherwise.
    // (Processes themselves have no "title"; windows do, so this searches window titles.)
    bool search_processes_exist_title(const std::string& str_title) const {
        if (str_title.empty()) return false;
        return raise_by_title(str_title);
    }

private:
#if defined(_WIN32)
    // ------------------------------------------------------------------ Windows
    struct Search {
        std::wstring needle;
        HWND found = nullptr;
    };

    static std::wstring to_wide(const std::string& s) {
        if (s.empty()) return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(static_cast<std::size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
        return w;
    }

    static BOOL CALLBACK enum_proc(HWND hwnd, LPARAM lparam) {
        auto* search = reinterpret_cast<Search*>(lparam);
        if (!IsWindowVisible(hwnd)) return TRUE;
        wchar_t buf[512];
        const int len = GetWindowTextW(hwnd, buf, 512);
        if (len <= 0) return TRUE;
        if (std::wstring_view(buf, static_cast<std::size_t>(len)).find(search->needle) != std::wstring_view::npos) {
            search->found = hwnd;
            return FALSE; // stop enumerating
        }
        return TRUE;
    }

    bool raise_by_title(const std::string& title) const {
        Search search{to_wide(title), nullptr};
        EnumWindows(enum_proc, reinterpret_cast<LPARAM>(&search));
        if (!search.found) return false;

        HWND hwnd = search.found;
        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);

        // Windows only lets the foreground program change the foreground window, so briefly
        // attach to its input thread to be allowed to do it.
        const DWORD fg_thread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        const DWORD my_thread = GetCurrentThreadId();
        if (fg_thread != my_thread) AttachThreadInput(my_thread, fg_thread, TRUE);
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
        if (fg_thread != my_thread) AttachThreadInput(my_thread, fg_thread, FALSE);
        return true;
    }

#elif defined(__APPLE__)
    // ------------------------------------------------------------------ macOS
    bool raise_by_title(const std::string& title) const {
        NSString* needle = [NSString stringWithUTF8String:title.c_str()];
        if (!needle || [needle length] == 0) return false;

        // 1. Check all windows belonging to THIS process first (needs no permissions,
        //    works for both visible and minimized windows).
        if (NSApp) {
            for (NSWindow* win in [NSApp windows]) {
                if (![win isVisible] && ![win isMiniaturized]) continue;
                NSString* winTitle = [win title];
                if (winTitle && [winTitle rangeOfString:needle].location != NSNotFound) {
                    if ([win isMiniaturized]) {
                        [win deminiaturize:nil];
                    }
                    [NSApp activateIgnoringOtherApps:YES];
                    [win makeKeyAndOrderFront:nil];
                    return true;
                }
            }
        }

        // 2. Fallback: search other running processes via Quartz + Accessibility APIs
        NSArray* windows = CFBridgingRelease(CGWindowListCopyWindowInfo(
            kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID));

        for (NSDictionary* w in windows) {
            if ([w[(id)kCGWindowLayer] intValue] != 0) continue;
            NSString* name = w[(id)kCGWindowName];
            if (!name || [name rangeOfString:needle].location == NSNotFound) continue;

            const pid_t pid = [w[(id)kCGWindowOwnerPID] intValue];
            NSRunningApplication* app = [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
            [app activateWithOptions:NSApplicationActivateIgnoringOtherApps];

            if (AXIsProcessTrusted()) {
                AXUIElementRef ax_app = AXUIElementCreateApplication(pid);
                CFArrayRef ax_windows = nullptr;
                if (AXUIElementCopyAttributeValue(ax_app, kAXWindowsAttribute,
                                                  reinterpret_cast<CFTypeRef*>(&ax_windows)) == kAXErrorSuccess && ax_windows) {
                    for (CFIndex i = 0; i < CFArrayGetCount(ax_windows); ++i) {
                        auto ax_win = static_cast<AXUIElementRef>(const_cast<void*>(CFArrayGetValueAtIndex(ax_windows, i)));
                        CFTypeRef ax_title = nullptr;
                        if (AXUIElementCopyAttributeValue(ax_win, kAXTitleAttribute, &ax_title) == kAXErrorSuccess && ax_title) {
                            NSString* t = (__bridge NSString*)ax_title;
                            if ([t rangeOfString:needle].location != NSNotFound) {
                                AXUIElementPerformAction(ax_win, kAXRaiseAction);
                                CFRelease(ax_title);
                                break;
                            }
                            CFRelease(ax_title);
                        }
                    }
                    CFRelease(ax_windows);
                }
                CFRelease(ax_app);
            }
            return true;
        }
        return false;
    }

#else
    // ------------------------------------------------------------------ Linux (X11)
    // Notes: works for X11 windows and XWayland windows. A pure Wayland session does not allow
    // one program to list or raise another program's windows at all, so this returns false there.
    static std::string window_title(Display* d, Window w, Atom net_wm_name, Atom utf8) {
        Atom type;
        int format;
        unsigned long n, after;
        unsigned char* data = nullptr;
        std::string result;
        if (XGetWindowProperty(d, w, net_wm_name, 0, 1024, False, utf8, &type, &format, &n, &after, &data) == Success && data) {
            result.assign(reinterpret_cast<char*>(data), n);
            XFree(data);
        } else {
            char* name = nullptr;
            if (XFetchName(d, w, &name) && name) { result = name; XFree(name); }
        }
        return result;
    }

    bool raise_by_title(const std::string& title) const {
        Display* d = XOpenDisplay(nullptr);
        if (!d) return false;
        const Window root = DefaultRootWindow(d);
        const Atom client_list = XInternAtom(d, "_NET_CLIENT_LIST", True);
        const Atom net_wm_name = XInternAtom(d, "_NET_WM_NAME", False);
        const Atom utf8 = XInternAtom(d, "UTF8_STRING", False);
        const Atom active = XInternAtom(d, "_NET_ACTIVE_WINDOW", False);

        bool found = false;
        Atom type;
        int format;
        unsigned long n = 0, after;
        unsigned char* data = nullptr;
        if (client_list != None &&
            XGetWindowProperty(d, root, client_list, 0, ~0L, False, XA_WINDOW, &type, &format, &n, &after, &data) == Success && data) {
            const auto* wins = reinterpret_cast<Window*>(data);
            for (unsigned long i = 0; i < n && !found; ++i) {
                if (window_title(d, wins[i], net_wm_name, utf8).find(title) == std::string::npos) continue;
                XEvent ev{};
                ev.xclient.type = ClientMessage;
                ev.xclient.window = wins[i];
                ev.xclient.message_type = active;
                ev.xclient.format = 32;
                ev.xclient.data.l[0] = 1;            // request comes from an application
                ev.xclient.data.l[1] = CurrentTime;
                XSendEvent(d, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
                XMapRaised(d, wins[i]);              // also un-minimize
                found = true;
            }
            XFree(data);
        }
        XFlush(d);
        XCloseDisplay(d);
        return found;
    }
#endif
};
