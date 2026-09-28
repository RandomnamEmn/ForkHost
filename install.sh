#!/usr/bin/env bash
set -euo pipefail

REPO="RandomnamEmn/ForkHost"
INSTALL_DIR="${LHC_INSTALL_DIR:-/usr/local/bin}"

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "lhc: this installer currently supports macOS only." >&2
    echo "On Windows, use install.ps1 from the ForkHost repository." >&2
    exit 1
fi

case "$(uname -m)" in
    arm64|aarch64)
        ASSET="ForkHost-macos-arm64.zip"
        ;;
    x86_64|amd64)
        ASSET="ForkHost-macos-x64.zip"
        ;;
    *)
        echo "lhc: unsupported macOS architecture: $(uname -m)" >&2
        exit 1
        ;;
esac

if ! command -v curl >/dev/null 2>&1; then
    echo "lhc: curl is required." >&2
    exit 1
fi

if ! command -v unzip >/dev/null 2>&1; then
    echo "lhc: unzip is required." >&2
    exit 1
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

ARCHIVE="$TMP_DIR/lhc.zip"
EXTRACTED="$TMP_DIR/extracted"
URL="https://github.com/$REPO/releases/latest/download/$ASSET"

echo "Downloading latest lhc release..."
if ! curl --proto '=https' --tlsv1.2 -fsSL "$URL" -o "$ARCHIVE"; then
    echo "lhc: failed to download $ASSET from the latest release." >&2
    echo "The latest release may predate the lhc CLI; publish a newer tagged release and retry." >&2
    exit 1
fi

mkdir -p "$EXTRACTED"
unzip -q "$ARCHIVE" -d "$EXTRACTED"

LHC_PATH="$(find "$EXTRACTED" -type f -name lhc -print -quit)"
if [[ -z "$LHC_PATH" ]]; then
    echo "lhc: the latest release archive does not contain lhc." >&2
    exit 1
fi

DSYM_PATH="$(find "$EXTRACTED" -type d -name 'lhc.dSYM' -print -quit || true)"

install_without_sudo() {
    mkdir -p "$INSTALL_DIR"
    install -m 0755 "$LHC_PATH" "$INSTALL_DIR/lhc"

    if [[ -n "$DSYM_PATH" ]]; then
        rm -rf "$INSTALL_DIR/lhc.dSYM"
        cp -R "$DSYM_PATH" "$INSTALL_DIR/lhc.dSYM"
    fi
}

install_with_sudo() {
    if ! command -v sudo >/dev/null 2>&1; then
        echo "lhc: $INSTALL_DIR is not writable and sudo is unavailable." >&2
        echo "Set LHC_INSTALL_DIR to a writable directory and retry." >&2
        exit 1
    fi

    sudo mkdir -p "$INSTALL_DIR"
    sudo install -m 0755 "$LHC_PATH" "$INSTALL_DIR/lhc"

    if [[ -n "$DSYM_PATH" ]]; then
        sudo rm -rf "$INSTALL_DIR/lhc.dSYM"
        sudo cp -R "$DSYM_PATH" "$INSTALL_DIR/lhc.dSYM"
    fi
}

if [[ -d "$INSTALL_DIR" ]]; then
    if [[ -w "$INSTALL_DIR" ]]; then
        install_without_sudo
    else
        install_with_sudo
    fi
elif [[ -w "$(dirname "$INSTALL_DIR")" ]]; then
    install_without_sudo
else
    install_with_sudo
fi

echo "Installed lhc to $INSTALL_DIR/lhc"
if [[ ":$PATH:" != *":$INSTALL_DIR:"* ]]; then
    echo "Note: $INSTALL_DIR is not currently on PATH."
fi
