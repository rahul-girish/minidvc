#!/usr/bin/env bash
set -euo pipefail

echo "===> Checking and installing system dependencies..."

OS="$(uname -s)"

if [ "$OS" = "Linux" ]; then
    if [ -f /etc/os-release ]; then
        . /etc/os-release
        DISTRO="$ID"
    else
        DISTRO="unknown"
    fi

    SUDO=""
    if [ "$(id -u)" -ne 0 ]; then
        if command -v sudo >/dev/null 2>&1; then
            SUDO="sudo"
        else
            echo "Error: Need root privileges or sudo to install packages." >&2
            exit 1
        fi
    fi

    case "$DISTRO" in
        ubuntu|debian|pop|linuxmint)
            $SUDO apt-get update -y
            $SUDO apt-get install -y build-essential pkg-config libssl-dev libzstd-dev curl
            ;;
        fedora|rhel|centos|rocky|alma)
            $SUDO dnf install -y gcc gcc-c++ make pkgconfig openssl-devel libzstd-devel curl
            ;;
        arch|manjaro)
            $SUDO pacman -Sy --noconfirm base-devel openssl zstd curl pkgconf
            ;;
        opensuse*|suse)
            $SUDO zypper refresh
            $SUDO zypper install -y gcc make pkg-config libopenssl-devel libzstd-devel curl
            ;;
        *)
            echo "Warning: Unrecognized Linux distribution '$DISTRO'."
            echo "Please ensure gcc/clang, make, pkg-config, openssl (libcrypto), zstd, and curl are installed."
            ;;
    esac
elif [ "$OS" = "Darwin" ]; then
    if ! command -v brew >/dev/null 2>&1; then
        echo "Error: Homebrew is required on macOS. Install it from https://brew.sh/" >&2
        exit 1
    fi
    brew install make pkg-config openssl@3 zstd curl
else
    echo "Unsupported OS: $OS" >&2
    exit 1
fi

echo "===> Building minidvc..."
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

make clean
make

echo "===> Installing minidvc..."
PREFIX="${PREFIX:-/usr/local}"

if [ "$(id -u)" -ne 0 ] && [ "$PREFIX" = "/usr/local" ]; then
    if command -v sudo >/dev/null 2>&1; then
        sudo make install PREFIX="$PREFIX"
    else
        echo "Root privileges not available. Installing to $HOME/.local instead..."
        PREFIX="$HOME/.local"
        make install PREFIX="$PREFIX"
    fi
else
    make install PREFIX="$PREFIX"
fi

echo ""
echo "===================================================="
echo " minidvc has been successfully installed!"
echo " Binary location: $PREFIX/bin/minidvc"
echo "===================================================="
if ! command -v minidvc >/dev/null 2>&1; then
    echo "Note: Make sure '$PREFIX/bin' is included in your PATH:"
    echo "  export PATH=\"$PREFIX/bin:\$PATH\""
fi
