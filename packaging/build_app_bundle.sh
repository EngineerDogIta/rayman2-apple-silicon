#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_BIN="${1:-$ROOT_DIR/build/Rayman2}"
APP_DIR="${2:-$ROOT_DIR/build/Rayman 2.app}"
GAME_DATA_SRC="${RAY2_GAME_DIR:-$ROOT_DIR/game}"

if [[ ! -x "$BUILD_BIN" ]]; then
  echo "Error: executable not found at $BUILD_BIN" >&2
  exit 1
fi

CONTENTS="$APP_DIR/Contents"
MACOS="$CONTENTS/MacOS"
FRAMEWORKS="$CONTENTS/Frameworks"
RESOURCES="$CONTENTS/Resources"

rm -rf "$APP_DIR"
mkdir -p "$MACOS" "$FRAMEWORKS" "$RESOURCES"

cp "$BUILD_BIN" "$MACOS/Rayman2"
chmod 755 "$MACOS/Rayman2"

# Copy all local SwARc dylibs built alongside Rayman2 (exclude build/swarc_test stale libs)
find "$(dirname "$BUILD_BIN")/third_party/SwARc" -maxdepth 4 -name '*.dylib' -exec cp -L {} "$FRAMEWORKS/" \; 2>/dev/null || true

# Recursively discover and copy all non-system /opt/homebrew or build dylib dependencies
changed=1
while [[ "$changed" -eq 1 ]]; do
  changed=0
  for bin in "$MACOS/Rayman2" "$FRAMEWORKS"/*.dylib; do
    [[ -e "$bin" ]] || continue
    while IFS= read -r dep; do
      [[ -z "$dep" ]] && continue
      base="$(basename "$dep")"
      [[ -e "$FRAMEWORKS/$base" ]] && continue
      src=""
      if [[ "$dep" == /opt/homebrew/* && -f "$dep" ]]; then
        src="$dep"
      elif [[ "$dep" == "$ROOT_DIR/build/"* && -f "$dep" ]]; then
        src="$dep"
      elif [[ "$dep" == @rpath/* ]]; then
        src="$(find /opt/homebrew/opt -maxdepth 3 -name "$base" 2>/dev/null | head -n 1 || true)"
      fi
      if [[ -n "$src" && -f "$src" ]]; then
        cp -L "$src" "$FRAMEWORKS/$base"
        chmod 755 "$FRAMEWORKS/$base"
        changed=1
      fi
    done < <(otool -L "$bin" | awk 'NR>1 && /\.dylib/{print $1}')
  done
done

# Relink all binaries ($MACOS/Rayman2 + $FRAMEWORKS/*.dylib) to only use @rpath and clean RPATHs
for bin in "$MACOS/Rayman2" "$FRAMEWORKS"/*.dylib; do
  [[ -e "$bin" ]] || continue
  chmod u+w "$bin"
  codesign --remove-signature "$bin" 2>/dev/null || true

  # Strip all existing build / Homebrew LC_RPATH entries
  while IFS= read -r rp; do
    [[ -z "$rp" ]] && continue
    install_name_tool -delete_rpath "$rp" "$bin" 2>/dev/null || true
  done < <(otool -l "$bin" | awk '/cmd LC_RPATH/{getline; getline; if ($1=="path") print $2}')

  # Rewrite ID for bundled dylibs
  if [[ "$bin" == "$FRAMEWORKS/"* ]]; then
    base="$(basename "$bin")"
    install_name_tool -id "@rpath/$base" "$bin"
    install_name_tool -add_rpath "@loader_path" "$bin" 2>/dev/null || true
  else
    install_name_tool -add_rpath "@executable_path/../Frameworks" "$bin" 2>/dev/null || true
  fi

  # Rewrite any non-system dependencies to @rpath/<basename>
  while IFS= read -r dep; do
    [[ -z "$dep" ]] && continue
    if [[ "$dep" == /opt/homebrew/* || "$dep" == "$ROOT_DIR/build/"* ]]; then
      dep_base="$(basename "$dep")"
      install_name_tool -change "$dep" "@rpath/$dep_base" "$bin"
    fi
  done < <(otool -L "$bin" | awk 'NR>1 && /\.dylib/{print $1}')
done

cat > "$CONTENTS/Info.plist" << 'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key>
  <string>English</string>
  <key>CFBundleExecutable</key>
  <string>Rayman2</string>
  <key>CFBundleIdentifier</key>
  <string>com.ubisoft.rayman2.macsilicon</string>
  <key>CFBundleInfoDictionaryVersion</key>
  <string>6.0</string>
  <key>CFBundleName</key>
  <string>Rayman 2</string>
  <key>CFBundleDisplayName</key>
  <string>Rayman 2: The Great Escape</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>CFBundleShortVersionString</key>
  <string>1.0.0</string>
  <key>LSMinimumSystemVersion</key>
  <string>14.0</string>
  <key>NSHighResolutionCapable</key>
  <true/>
</dict>
</plist>
EOF

if [[ -d "$GAME_DATA_SRC" && ! -e "$RESOURCES/game" ]]; then
  # Copy retail GOG assets locally into the gitignored .app bundle (never tracked in git)
  mkdir -p "$RESOURCES/game"
  rsync -a "$GAME_DATA_SRC/" "$RESOURCES/game/"
fi

for bin in "$FRAMEWORKS"/*.dylib; do
  [[ -e "$bin" ]] || continue
  codesign --force --sign - "$bin" >/dev/null
done
codesign --force --deep --sign - "$APP_DIR" >/dev/null
echo "Packaged standalone bundle: $APP_DIR"
