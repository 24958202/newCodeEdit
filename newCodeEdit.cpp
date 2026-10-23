// newCodeEdit - Updated for Saucer v7+ API with embedded logo.xpm.
// macOS only (osascript dialogs, POSIX file I/O). Needs C++23 (std::expected).
#include <saucer/app.hpp>
#include <saucer/smartview.hpp>
#include <saucer/window.hpp>

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
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

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

// --- NATIVE MAC OS DIALOGS ---
#ifdef __APPLE__
static std::string run_osascript(const char* command) {
    FILE* pipe = ::popen(command, "r");
    if (!pipe) return "";
    std::string out;
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) out += buffer;
    ::pclose(pipe);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}
std::string open_folder_dialog() {
    return run_osascript("/usr/bin/osascript -e 'POSIX path of (choose folder)' 2>/dev/null");
}
std::string open_file_dialog() {
    return run_osascript("/usr/bin/osascript -e 'POSIX path of (choose file)' 2>/dev/null");
}
#else
std::string open_folder_dialog() { return ""; }
std::string open_file_dialog() { return ""; }
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

// --- FILE I/O ALGORITHMS ---
constexpr std::uintmax_t kMaxFileBytes = 10ull * 1024 * 1024;

Result<std::string> read_file(const std::string& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return fail(std::format("Not a regular file: {}", path));

    const auto size = fs::file_size(path, ec);
    if (ec) return fail(std::format("Cannot read the size of {}: {}", path, ec.message()));
    if (size > kMaxFileBytes) {
        return fail(std::format("{} is larger than {} MB. Open it in another tool.", path, kMaxFileBytes >> 20));
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) return fail(std::format("Cannot open {}. Check permissions.", path));

    std::string content(static_cast<std::size_t>(size), '\0');
    in.read(content.data(), static_cast<std::streamsize>(content.size()));
    content.resize(static_cast<std::size_t>(in.gcount()));

    if (content.find('\0') != std::string::npos || !is_valid_utf8(content)) {
        return fail(std::format("{} is not UTF-8 text (binary file?).", path));
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
        ::close(fd);
        ::unlink(temp.c_str());
        return fail(std::format("{} {}: {}", what, path, std::strerror(err)));
    };

    struct stat st {};
    const mode_t mode = (::stat(target.c_str(), &st) == 0) ? (st.st_mode & 07777) : 0644;
    ::fchmod(fd, mode);

    const char* data = content.data();
    std::size_t left = content.size();
    while (left > 0) {
        const ssize_t n = ::write(fd, data, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            return abort_with("Cannot write", errno);
        }
        data += n;
        left -= static_cast<std::size_t>(n);
    }

#ifdef F_FULLFSYNC
    if (::fcntl(fd, F_FULLFSYNC) == -1) ::fsync(fd);
#else
    ::fsync(fd);
#endif

    if (::close(fd) != 0) {
        const int err = errno;
        ::unlink(temp.c_str());
        return fail(std::format("Cannot finish writing {}: {}", path, std::strerror(err)));
    }
    if (::rename(temp.c_str(), target.c_str()) != 0) {
        const int err = errno;
        ::unlink(temp.c_str());
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
        json += std::format(R"({{"name":"{}","path":"{}","is_dir":{}}})", json_escape(name), json_escape(full),
                            is_dir ? "true" : "false");
    }
    if (ec) return fail(std::format("Error while reading folder {}: {}", path, ec.message()));
    json += ']';
    return json;
}

// ==== END HELPERS ===========================================================

// --- HTML / JS / CSS PAYLOAD ---
static constexpr std::string_view ui_html = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title>newCodeEdit</title>
    <style>
        body, html { margin: 0; padding: 0; height: 100%; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif; background: #1e1e1e; color: #ccc; overflow: hidden; }
        #toolbar { height: 40px; background: #252526; display: flex; align-items: center; padding: 0 10px; border-bottom: 1px solid #333; }
        button { background: #333; color: white; border: none; padding: 6px 12px; margin-right: 8px; border-radius: 4px; cursor: pointer; font-size: 12px; }
        button:hover { background: #444; }
        button:focus-visible, .tree-item:focus-visible { outline: 2px solid #3794ff; outline-offset: -2px; }
        #status { margin-left: 12px; min-width: 0; font-size: 13px; color: #888; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
        #status.error { color: #f48771; }
        #app { display: flex; height: calc(100vh - 41px); }
        #sidebar { width: 250px; flex-shrink: 0; background: #252526; overflow-y: auto; border-right: 1px solid #333; padding: 10px 0; }
        #editor-container { flex: 1; min-width: 0; position: relative; }

        .tree-item { padding: 4px 10px 4px 20px; cursor: pointer; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; font-size: 13px; }
        .tree-item:hover { background: #37373d; }
        .tree-item.active { background: #094771; color: #fff; }
        .tree-item.folder { font-weight: bold; color: #fff; }

        .breakpoint-glyph { background-color: #e51400; border-radius: 50%; width: 10px !important; height: 10px !important; margin-top: 4px; margin-left: 4px; }
    </style>
</head>
<body>
    <div id="toolbar">
        <button onclick="openFile()">Open File</button>
        <button onclick="openFolder()">Open Folder</button>
        <button onclick="saveCurrent()">Save (Cmd+S)</button>
        <span id="status">No file opened</span>
    </div>
    <div id="app">
        <div id="sidebar"></div>
        <div id="editor-container"></div>
    </div>

    <script src="https://cdnjs.cloudflare.com/ajax/libs/require.js/2.3.6/require.min.js"></script>
    <script>
        const MONACO_BASE = 'https://cdnjs.cloudflare.com/ajax/libs/monaco-editor/0.39.0/min';
        const BREAKPOINT_CLASS = 'breakpoint-glyph';

        let editor = null;
        let activePath = '';
        let statusTimer = 0;
        const openFiles = new Map();

        const statusEl = document.getElementById('status');
        const sidebar = document.getElementById('sidebar');

        function rpc(name, ...args) {
            return window.saucer.call(name, args);
        }

        function showStatus(text, isError = false) {
            statusEl.textContent = text;
            statusEl.title = text;
            statusEl.classList.toggle('error', isError);
        }

        function reportError(prefix, err) {
            showStatus(prefix + ': ' + err, true);
        }

        function isDirty(entry) {
            return entry.model.getAlternativeVersionId() !== entry.savedVersion;
        }

        function updateStatus() {
            const entry = openFiles.get(activePath);
            if (!entry) return showStatus('No file opened');
            showStatus(activePath + (isDirty(entry) ? '  \u25CF  unsaved changes' : ''));
        }

        function flashStatus(text) {
            showStatus(text);
            clearTimeout(statusTimer);
            statusTimer = setTimeout(updateStatus, 2000);
        }

        window.MonacoEnvironment = {
            getWorkerUrl: () => 'data:text/javascript;charset=utf-8,' + encodeURIComponent(
                "self.MonacoEnvironment = { baseUrl: '" + MONACO_BASE + "/' };" +
                "importScripts('" + MONACO_BASE + "/vs/base/worker/workerMain.js');")
        };

        const editorReady = new Promise((resolve) => {
            if (typeof require === 'undefined') {
                showStatus('The editor could not be loaded. Check your internet connection.', true);
                return;
            }
            require.config({ paths: { vs: MONACO_BASE + '/vs' } });
            require.onError = () => showStatus('The editor could not be loaded.', true);
            require(['vs/editor/editor.main'], function () {
                editor = monaco.editor.create(document.getElementById('editor-container'), {
                    value: '// newCodeEdit\n// Open a file or folder to begin.',
                    language: 'cpp',
                    theme: 'vs-dark',
                    automaticLayout: true,
                    glyphMargin: true,
                    wordWrap: 'on'
                });

                editor.onMouseDown(function (e) {
                    if (e.target.type === monaco.editor.MouseTargetType.GUTTER_GLYPH_MARGIN) {
                        toggleBreakpoint(e.target.position.lineNumber);
                    }
                });
                resolve();
            });
        });

        window.addEventListener('keydown', function (e) {
            if ((e.metaKey || e.ctrlKey) && !e.shiftKey && !e.altKey && e.key.toLowerCase() === 's') {
                e.preventDefault();
                saveCurrent();
            }
        }, true);

        function toggleBreakpoint(line) {
            const entry = openFiles.get(activePath);
            if (!entry) return;
            const model = entry.model;
            const existing = model.getLineDecorations(line)
                .filter((d) => d.options.glyphMarginClassName === BREAKPOINT_CLASS);
            if (existing.length > 0) {
                model.deltaDecorations(existing.map((d) => d.id), []);
            } else {
                model.deltaDecorations([], [{
                    range: new monaco.Range(line, 1, line, 1),
                    options: {
                        glyphMarginClassName: BREAKPOINT_CLASS,
                        stickiness: monaco.editor.TrackedRangeStickiness.NeverGrowsWhenTypingAtEdges
                    }
                }]);
            }
        }

        async function openFile() {
            try {
                const path = await rpc('open_file_dialog');
                if (path) await loadFile(path);
            } catch (err) {
                reportError('Could not open the file', err);
            }
        }

        async function openFolder() {
            try {
                const path = await rpc('open_folder_dialog');
                if (!path) return;
                sidebar.replaceChildren();
                await renderDir(path, sidebar, 0);
            } catch (err) {
                reportError('Could not open the folder', err);
            }
        }

        async function loadFile(path) {
            await editorReady;
            if (!openFiles.has(path)) {
                let content;
                try {
                    content = await rpc('read_file', path);
                } catch (err) {
                    reportError('Could not open ' + path, err);
                    return;
                }
                if (!openFiles.has(path)) {
                    const model = monaco.editor.createModel(content, undefined, monaco.Uri.file(path));
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
            editor.focus();
            updateStatus();
            markActiveRow();
        }

        async function saveCurrent() {
            const entry = openFiles.get(activePath);
            if (!entry) return;
            const version = entry.model.getAlternativeVersionId();
            try {
                await rpc('atomic_save', activePath, entry.model.getValue());
                entry.savedVersion = version;
                flashStatus(activePath + '  (Saved)');
            } catch (err) {
                reportError('Could not save ' + activePath, err);
            }
        }

        function markActiveRow() {
            for (const row of sidebar.querySelectorAll('.tree-item.file')) {
                row.classList.toggle('active', row.dataset.path === activePath);
            }
        }

        async function renderDir(dirPath, container, depth) {
            let nodes;
            try {
                nodes = JSON.parse(await rpc('list_directory', dirPath));
            } catch (err) {
                reportError('Could not read the folder', err);
                return;
            }
            nodes.sort((a, b) => Number(b.is_dir) - Number(a.is_dir) || a.name.localeCompare(b.name));

            for (const node of nodes) {
                const row = document.createElement('div');
                row.className = 'tree-item ' + (node.is_dir ? 'folder' : 'file');
                row.style.paddingLeft = (10 + depth * 14) + 'px';
                row.tabIndex = 0;
                row.dataset.path = node.path;
                container.appendChild(row);
                row.addEventListener('keydown', (e) => {
                    if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); row.click(); }
                });

                if (!node.is_dir) {
                    row.textContent = '\uD83D\uDCC4 ' + node.name;
                    row.addEventListener('click', () => loadFile(node.path));
                    continue;
                }

                const children = document.createElement('div');
                children.hidden = true;
                container.appendChild(children);
                const label = () => { row.textContent = (children.hidden ? '\u25B8 ' : '\u25BE ') + '\uD83D\uDCC1 ' + node.name; };
                label();

                let loaded = false;
                row.addEventListener('click', async () => {
                    if (!loaded) {
                        loaded = true;
                        await renderDir(node.path, children, depth + 1);
                        markActiveRow();
                    }
                    children.hidden = !children.hidden;
                    label();
                });
            }
        }
    </script>
</body>
</html>
)HTML";

// --- SAUCER V7+ LIFECYCLE ---
coco::stray start(saucer::application* app) {
    auto window = saucer::window::create(app).value();
    auto webview = saucer::smartview::create({.window = window});

    window->set_title("newCodeEdit");
    window->set_size({.w = 1000, .h = 700});

    webview->expose("open_folder_dialog", [] { return open_folder_dialog(); });
    webview->expose("open_file_dialog", [] { return open_file_dialog(); });
    webview->expose("list_directory", [](const std::string& path) { 
        return guarded<std::string>([&] { return list_directory(path); }); 
    });
    webview->expose("read_file", [](const std::string& path) { 
        return guarded<std::string>([&] { return read_file(path); }); 
    });
    webview->expose("atomic_save", [](const std::string& path, const std::string& content) {
        return guarded<bool>([&] { return atomic_save(path, content); });
    });

    std::error_code ec;
    const fs::path ui_path = fs::temp_directory_path(ec) / std::format("newcode_ui_{}.html", ::getpid());
    if (ec) {
        std::fprintf(stderr, "newCodeEdit: no temp directory: %s\n", ec.message().c_str());
        co_return;
    }

    std::ofstream out(ui_path, std::ios::binary | std::ios::trunc);
    out << ui_html;
    out.close();

    if (!out) {
        std::fprintf(stderr, "newCodeEdit: cannot write %s\n", ui_path.c_str());
        co_return;
    }

    webview->set_url("file://" + ui_path.string());
    window->show();
    
    co_await app->finish();

    fs::remove(ui_path, ec);
}

int main() {
    auto app = saucer::application::create({.id = "dev.ronnie.newcodeedit"});
    return app->run(start);
}