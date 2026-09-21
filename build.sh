#!/usr/bin/env bash
set -e
set -o pipefail

BUILD_DIR="build"
DANA_PATH=""
DO_INSTALL=0
DO_UNINSTALL=0
USER_INSTALL=0

print_help() {
    cat << EOF
Usage: ./build.sh [OPTIONS]

Options:
  --dana-path <dir>     Path to DANA source repository (default: ../dana)
  --install             Install plugin system-wide (/usr/lib/vlc/plugins/codec/)
  --user                Install plugin to user directory (~/.local/lib/vlc/plugins/codec/)
  --uninstall           Remove installed plugin
  -c, --clean           Clean build directory
  -h, --help            Show this help message

Examples:
  ./build.sh --dana-path /path/to/dana
  ./build.sh --user --install
  sudo ./build.sh --install
  sudo ./build.sh --uninstall
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dana-path)
            DANA_PATH="$2"
            shift 2
            ;;
        --install)
            DO_INSTALL=1
            shift
            ;;
        --user)
            USER_INSTALL=1
            shift
            ;;
        --uninstall)
            DO_UNINSTALL=1
            shift
            ;;
        -c|--clean)
            rm -rf "$BUILD_DIR"
            echo "==> Build directory cleaned."
            exit 0
            ;;
        -h|--help)
            print_help
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            print_help
            exit 1
            ;;
    esac
done

# Resolve Dana path
if [ -z "$DANA_PATH" ]; then
    if [ -d "../dana/src" ]; then
        DANA_PATH="$(cd ../dana && pwd)"
    elif [ -d "./dana/src" ]; then
        DANA_PATH="$(cd ./dana && pwd)"
    else
        echo "Error: Dana source directory not found. Specify with --dana-path <dir>"
        exit 1
    fi
else
    DANA_PATH="$(cd "$DANA_PATH" && pwd)"
fi

# Detect VLC plugin directories
if [ "$USER_INSTALL" -eq 1 ]; then
    TARGET_DIR="${HOME}/.local/lib/vlc/plugins/codec"
else
    VLC_PLUGIN_DIR="$(pkg-config --variable=pluginsdir vlc-plugin 2>/dev/null || echo "/usr/lib/vlc/plugins")"
    TARGET_DIR="${VLC_PLUGIN_DIR}/codec"
fi

# Handle uninstall
if [ "$DO_UNINSTALL" -eq 1 ]; then
    PLUGIN_FILE="${TARGET_DIR}/libdana_plugin.so"
    echo "==> Uninstalling ${PLUGIN_FILE}..."
    if [ -f "$PLUGIN_FILE" ]; then
        if [ -w "$TARGET_DIR" ]; then
            rm -f "$PLUGIN_FILE"
        else
            sudo rm -f "$PLUGIN_FILE"
        fi
        echo "==> Removed plugin."
        if command -v vlc-cache-gen >/dev/null 2>&1; then
            echo "==> Updating VLC plugin cache..."
            if [ -w "$(dirname "$TARGET_DIR")" ]; then
                vlc-cache-gen "$(dirname "$TARGET_DIR")" || true
            else
                sudo vlc-cache-gen "$(dirname "$TARGET_DIR")" || true
            fi
        fi
    else
        echo "Plugin not found at ${PLUGIN_FILE}"
    fi
    exit 0
fi

# Configure and build
echo "==> Configuring build against DANA: ${DANA_PATH}"
cmake -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DDANA_SOURCE_DIR="$DANA_PATH"

echo "==> Building VLC Dana plugin..."
cmake --build "$BUILD_DIR" --parallel

SO_PATH="${BUILD_DIR}/libdana_plugin.so"
if [ ! -f "$SO_PATH" ]; then
    echo "Error: Build succeeded but ${SO_PATH} was not generated."
    exit 1
fi
echo "==> Successfully built ${SO_PATH}"

# Handle installation
if [ "$DO_INSTALL" -eq 1 ]; then
    echo "==> Installing to ${TARGET_DIR}..."
    if [ "$USER_INSTALL" -eq 1 ]; then
        mkdir -p "$TARGET_DIR"
        cp "$SO_PATH" "${TARGET_DIR}/"
        if command -v vlc-cache-gen >/dev/null 2>&1; then
            vlc-cache-gen "${HOME}/.local/lib/vlc/plugins" || true
        fi
    else
        if [ ! -w "$TARGET_DIR" ] && [ "$EUID" -ne 0 ]; then
            echo "Need sudo permissions to install into ${TARGET_DIR}:"
            sudo mkdir -p "$TARGET_DIR"
            sudo cp "$SO_PATH" "${TARGET_DIR}/"
            if command -v vlc-cache-gen >/dev/null 2>&1; then
                sudo vlc-cache-gen "$VLC_PLUGIN_DIR" || true
            fi
        else
            mkdir -p "$TARGET_DIR"
            cp "$SO_PATH" "${TARGET_DIR}/"
            if command -v vlc-cache-gen >/dev/null 2>&1; then
                vlc-cache-gen "$VLC_PLUGIN_DIR" || true
            fi
        fi
    fi
    echo "==> Plugin installed. Restart VLC to load."
fi