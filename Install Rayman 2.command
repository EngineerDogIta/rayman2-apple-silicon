#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT_DIR"

if [[ -x /opt/homebrew/bin/brew ]]; then
  eval "$(/opt/homebrew/bin/brew shellenv)"
fi

echo "============================================================"
echo "  Rayman 2: The Great Escape — Apple Silicon Installer"
echo "============================================================"
echo ""

if ! command -v brew >/dev/null 2>&1; then
  osascript -e 'display alert "Homebrew Required" message "Homebrew is needed to install build dependencies. Visit https://brew.sh to install Homebrew, then double-click Install Rayman 2.command again." as critical' >/dev/null 2>&1 || true
  echo "Error: Homebrew not found. Install from https://brew.sh and retry."
  exit 1
fi

SELECTED_PATH=""
if [[ -f "$ROOT_DIR/game/Rayman2.exe" ]]; then
  CHOICE="$(osascript -e 'button returned of (display dialog "An extracted Rayman 2 game folder was already found in ./game/.\n\nUse it or select a new GOG installer?" buttons {"Cancel", "Select GOG Installer", "Use ./game/"} default button "Use ./game/" with title "Rayman 2 Apple Silicon")' 2>/dev/null || echo "Cancel")"
  if [[ "$CHOICE" == "Use ./game/" ]]; then
    SELECTED_PATH="$ROOT_DIR/game"
  elif [[ "$CHOICE" == "Cancel" ]]; then
    exit 0
  fi
fi

if [[ -z "$SELECTED_PATH" ]]; then
  MODE="$(osascript -e 'button returned of (display dialog "Select your legal PC copy of Rayman 2: The Great Escape:\n\n• GOG Installer (.exe, keep the .bin file in the same folder)\n• Or an already-installed Rayman 2 game folder" buttons {"Cancel", "Choose Folder", "Choose GOG .exe"} default button "Choose GOG .exe" with title "Rayman 2 Apple Silicon")' 2>/dev/null || echo "Cancel")"
  if [[ "$MODE" == "Choose GOG .exe" ]]; then
    SELECTED_PATH="$(osascript -e 'POSIX path of (choose file with prompt "Select your Rayman 2 GOG installer (.exe):" of type {"exe", "com.microsoft.windows-executable"})' 2>/dev/null || true)"
  elif [[ "$MODE" == "Choose Folder" ]]; then
    SELECTED_PATH="$(osascript -e 'POSIX path of (choose folder with prompt "Select your installed Rayman 2 folder (containing Rayman2.exe):")' 2>/dev/null || true)"
  else
    exit 0
  fi
fi

if [[ -z "$SELECTED_PATH" ]]; then
  echo "Cancelled."
  exit 0
fi

"$ROOT_DIR/setup.sh" --auto-deps "$SELECTED_PATH"

APP_SRC="$ROOT_DIR/build/Rayman 2.app"
if [[ -d "$APP_SRC" ]]; then
  POST_ACTION="$(osascript -e 'button returned of (display dialog "Rayman 2.app is ready!\n\nWould you like to copy it to /Applications and launch it now?" buttons {"Done", "Launch Only", "Install to /Applications & Launch"} default button "Install to /Applications & Launch" with title "Rayman 2 Apple Silicon")' 2>/dev/null || echo "Launch Only")"
  if [[ "$POST_ACTION" == "Install to /Applications & Launch" ]]; then
    rm -rf "/Applications/Rayman 2.app"
    cp -R "$APP_SRC" "/Applications/Rayman 2.app"
    open "/Applications/Rayman 2.app"
  elif [[ "$POST_ACTION" == "Launch Only" ]]; then
    open "$APP_SRC"
  fi
fi
