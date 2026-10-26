#!/usr/bin/env bash
# ==============================================================================
# newCodeEdit — Universal Cross-Platform Installer (macOS / Linux / Windows)
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/<USER>/newCodeEdit/main/install.sh | bash
# ==============================================================================
set -euo pipefail

REPO_OWNER="${NEWCODEEDIT_REPO_OWNER:-24958202}"
REPO_NAME="${NEWCODEEDIT_REPO_NAME:-newCodeEdit}"
REPO_BRANCH="${NEWCODEEDIT_REPO_BRANCH:-main}"
REPO_URL="https://github.com/${REPO_OWNER}/${REPO_NAME}.git"
SAUCER_URL="https://github.com/saucer/saucer.git"

WORK_DIR="$(mktemp -d 2>/dev/null || mktemp -d -t 'newcodeedit_build')"
cleanup() {
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

info()  { printf "\033[1;34m[INFO]\033[0m %s\n" "$*"; }
ok()    { printf "\033[1;32m[ OK ]\033[0m %s\n" "$*"; }
error() { printf "\033[1;31m[ERR ]\033[0m %s\n" "$*" >&2; exit 1; }

# --- 1. Detect Operating System & Architecture ---
OS_UNAME="$(uname -s)"
ARCH_UNAME="$(uname -m)"

case "$OS_UNAME" in
    Darwin*)               OS="macos" ;;
    Linux*)                OS="linux" ;;
    MINGW*|MSYS*|CYGWIN*)  OS="windows" ;;
    *) error "Unsupported operating system: $OS_UNAME" ;;
esac

info "Detected platform: ${OS} (${ARCH_UNAME})"

# --- 2. Install Prerequisites per OS ---
install_deps_macos() {
    if ! command -v brew >/dev/null 2>&1; then
        info "Installing Homebrew..."
        /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
    fi
    info "Checking macOS build dependencies (llvm, cmake, ninja, nlohmann-json)..."
    for pkg in llvm cmake ninja nlohmann-json git; do
        if ! brew list "$pkg" >/dev/null 2>&1; then
            brew install "$pkg"
        fi
    done
}

install_deps_linux() {
    info "Checking Linux build dependencies..."
    if command -v apt-get >/dev/null 2>&1; then
        sudo apt-get update -y
        sudo apt-get install -y build-essential clang cmake ninja-build git pkg-config \
            nlohmann-json3-dev libgtk-4-dev libwebkitgtk-6.0-dev libadwaita-1-dev zenity
    elif command -v dnf >/dev/null 2>&1; then
        sudo dnf install -y clang cmake ninja-build git pkgconf-pkg-config \
            json-devel gtk4-devel webkitgtk6.0-devel libadwaita-devel zenity
    elif command -v pacman >/dev/null 2>&1; then
        sudo pacman -Sy --needed --noconfirm clang cmake ninja git pkgconf \
            nlohmann-json gtk4 webkitgtk-6.0 libadwaita zenity
    else
        info "Could not detect apt, dnf, or pacman. Assuming dependencies are already installed."
    fi
}

install_deps_windows() {
    info "Checking Windows build dependencies..."
    if command -v pacman >/dev/null 2>&1; then
        # MSYS2 environment
        pacman -Sy --needed --noconfirm mingw-w64-ucrt-x86_64-clang \
            mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja \
            mingw-w64-ucrt-x86_64-nlohmann-json git
    elif command -v winget >/dev/null 2>&1; then
        command -v git   >/dev/null 2>&1 || winget install --id Git.Git -e --source winget
        command -v cmake >/dev/null 2>&1 || winget install --id Kitware.CMake -e --source winget
        command -v ninja >/dev/null 2>&1 || winget install --id Ninja-build.Ninja -e --source winget
    fi
}

case "$OS" in
    macos)   install_deps_macos ;;
    linux)   install_deps_linux ;;
    windows) install_deps_windows ;;
esac

# --- 3. Locate or Build Saucer Static Libraries ---
SAUCER_ROOT="${SAUCER_ROOT:-}"
if [[ -z "$SAUCER_ROOT" ]]; then
    if [[ -d "/Users/jidengfeng/Downloads/libs/saucer/build" ]]; then
        SAUCER_ROOT="/Users/jidengfeng/Downloads/libs/saucer"
        SAUCER_BUILD_DIR="${SAUCER_ROOT}/build"
        info "Using existing local Saucer installation at ${SAUCER_ROOT}"
    else
        SAUCER_ROOT="${WORK_DIR}/saucer"
        SAUCER_BUILD_DIR="${SAUCER_ROOT}/build"
        info "Cloning and building Saucer in ${SAUCER_ROOT}..."
        git clone --depth 1 --recurse-submodules "$SAUCER_URL" "$SAUCER_ROOT"

        CMAKE_COMPILER_ARGS=()
        if [[ "$OS" == "macos" && -x "/opt/homebrew/opt/llvm/bin/clang++" ]]; then
            CMAKE_COMPILER_ARGS+=(
                "-DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++"
                "-DCMAKE_OBJCXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++"
            )
        fi

        cmake -S "$SAUCER_ROOT" -B "$SAUCER_BUILD_DIR" \
            -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -Dsaucer_static=ON \
            -Dsaucer_modules=ON \
            "${CMAKE_COMPILER_ARGS[@]}"
        cmake --build "$SAUCER_BUILD_DIR" --parallel
    fi
else
    SAUCER_BUILD_DIR="${SAUCER_BUILD_DIR:-${SAUCER_ROOT}/build}"
fi

# --- 4. Clone or Locate newCodeEdit Source ---
if [[ -f "./CMakeLists.txt" && -f "./newCodeEdit.cpp" ]]; then
    SRC_DIR="$(pwd)"
    info "Building from current local directory: ${SRC_DIR}"
else
    SRC_DIR="${WORK_DIR}/newCodeEdit"
    info "Cloning ${REPO_URL} (branch: ${REPO_BRANCH})..."
    git clone --depth 1 --branch "$REPO_BRANCH" "$REPO_URL" "$SRC_DIR"
fi

# --- 5. Configure and Build newCodeEdit ---
BUILD_DIR="${WORK_DIR}/build_app"
info "Configuring newCodeEdit with CMake..."
cmake -S "$SRC_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSAUCER_ROOT="$SAUCER_ROOT" \
    -DSAUCER_BUILD_DIR="$SAUCER_BUILD_DIR"

info "Compiling newCodeEdit..."
cmake --build "$BUILD_DIR" --config Release --parallel

# --- 6. Install newCodeEdit ---
info "Installing newCodeEdit..."
case "$OS" in
    macos)
        if [[ -w "/Applications" ]]; then
            cmake --install "$BUILD_DIR" --config Release
        else
            sudo cmake --install "$BUILD_DIR" --config Release
        fi
        ok "Installed to /Applications/newCodeEdit.app"
        ;;
    linux)
        sudo cmake --install "$BUILD_DIR" --config Release
        ok "Installed binary and desktop entry to /usr/share/applications/newCodeEdit.desktop"
        ;;
    windows)
        WIN_INSTALL_DIR="${LOCALAPPDATA:-$HOME/AppData/Local}/Programs/newCodeEdit"
        mkdir -p "$WIN_INSTALL_DIR"
        cmake --install "$BUILD_DIR" --config Release --prefix "$WIN_INSTALL_DIR"
        ok "Installed to ${WIN_INSTALL_DIR}"
        ;;
esac

ok "newCodeEdit installation complete!"
