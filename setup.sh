#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT_DIR"

SWARC_REPO="https://github.com/JTM-rootstorm/SwARc.git"
SWARC_COMMIT="6b25b3c"

usage() {
  cat <<'EOF'
Usage: ./setup.sh <path-to-gog-installer.exe | path-to-installed-game-dir>

Builds Rayman 2: The Great Escape natively for Apple Silicon (arm64-apple-macos)
from your own legal GOG copy of the game:
  1. Extracts the GOG installer into ./game/ (using innoextract), or uses an existing directory.
  2. Clones and patches SwARc into ./third_party/SwARc/.
  3. Generates symbol tables and statically lifts Rayman2.exe + audio/video DLLs into ./generated/.
  4. Compiles ./build/Rayman2 and packages ./build/Rayman 2.app.
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

INPUT_PATH="${1:-}"
GAME_DIR="$ROOT_DIR/game"

for tool in cmake ninja python3 git; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "Error: missing required tool '$tool'. Install with Homebrew: brew install cmake ninja python3 git" >&2
    exit 1
  fi
done

if ! command -v i686-w64-mingw32-objdump >/dev/null 2>&1; then
  echo "Error: missing i686-w64-mingw32-objdump. Install with: brew install mingw-w64" >&2
  exit 1
fi

if [[ -n "$INPUT_PATH" ]]; then
  if [[ -f "$INPUT_PATH" ]]; then
    if ! command -v innoextract >/dev/null 2>&1; then
      echo "Error: innoextract is required to extract the GOG installer. Install with: brew install innoextract" >&2
      exit 1
    fi
    echo "==> Extracting GOG installer into $GAME_DIR ..."
    rm -rf "$GAME_DIR"
    mkdir -p "$GAME_DIR"
    innoextract --silent --output-dir "$GAME_DIR" "$INPUT_PATH"
    if [[ -d "$GAME_DIR/app" && -f "$GAME_DIR/app/Rayman2.exe" ]]; then
      shopt -s dotglob
      mv "$GAME_DIR/app/"* "$GAME_DIR/"
      shopt -u dotglob
      rmdir "$GAME_DIR/app" 2>/dev/null || true
    fi
  elif [[ -d "$INPUT_PATH" ]]; then
    GAME_DIR="$(cd "$INPUT_PATH" && pwd)"
  else
    echo "Error: path not found: $INPUT_PATH" >&2
    exit 1
  fi
fi

if [[ ! -f "$GAME_DIR/Rayman2.exe" ]]; then
  echo "Error: Rayman2.exe not found in $GAME_DIR" >&2
  usage
  exit 1
fi
echo "==> Using game directory: $GAME_DIR"

if [[ ! -d "$ROOT_DIR/third_party/SwARc/.git" ]]; then
  echo "==> Cloning SwARc ($SWARC_COMMIT) ..."
  mkdir -p "$ROOT_DIR/third_party"
  git clone --depth 1 "$SWARC_REPO" "$ROOT_DIR/third_party/SwARc"
  git -C "$ROOT_DIR/third_party/SwARc" fetch --depth 1 origin "$SWARC_COMMIT" || true
  git -C "$ROOT_DIR/third_party/SwARc" checkout "$SWARC_COMMIT" || true
fi

if [[ -f "$ROOT_DIR/patches/swarc.patch" ]]; then
  if git -C "$ROOT_DIR/third_party/SwARc" apply --check "$ROOT_DIR/patches/swarc.patch" 2>/dev/null; then
    echo "==> Applying patches/swarc.patch ..."
    git -C "$ROOT_DIR/third_party/SwARc" apply "$ROOT_DIR/patches/swarc.patch"
  else
    echo "==> patches/swarc.patch already applied (or clean), skipping."
  fi
fi

echo "==> Configuring CMake (arm64-release) ..."
if [[ ! -d "$ROOT_DIR/generated" || ! -f "$ROOT_DIR/generated/table.cpp" ]]; then
  # Build recomp_lift directly from SwARc first because recomp_add_lifted() expects generated/
  cmake -S "$ROOT_DIR/third_party/SwARc" -B "$ROOT_DIR/build/swarc_tools" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DRECOMP_LLVM_ROOT="${RECOMP_LLVM_ROOT:-/opt/homebrew/opt/llvm}" \
    -DRECOMP_ALLOW_MISSING_TEST_TOOLS=ON \
    -DBUILD_TESTING=OFF
  cmake --build "$ROOT_DIR/build/swarc_tools" --target recomp_lift -j"$(sysctl -n hw.ncpu)"
  RECOMP_LIFT="$ROOT_DIR/build/swarc_tools/tools/lifter_x86/recomp_lift"

  echo "==> Generating symbols ..."
  PE_SYMS="$ROOT_DIR/third_party/SwARc/tools/pe_symbols.py"
  python3 "$PE_SYMS" "$GAME_DIR/Rayman2.exe" --out "$ROOT_DIR/symbols/rayman2"
  python3 "$PE_SYMS" "$GAME_DIR/DLL/GliVd1vf.dll" --out "$ROOT_DIR/symbols/glivd1vf"
  python3 "$PE_SYMS" "$GAME_DIR/DLL/WAVPWBVR.dll" --out "$ROOT_DIR/symbols/wavpwbvr"
  python3 "$PE_SYMS" "$GAME_DIR/DLL/APMP5BVR.dll" --out "$ROOT_DIR/symbols/apmp5bvr"

  ACP_ARG=()
  if [[ -d "$ROOT_DIR/third_party/ACP_Ray2/src" ]]; then
    ACP_ARG=(--acp-src "$ROOT_DIR/third_party/ACP_Ray2/src")
  fi
  python3 "$ROOT_DIR/tools/generate_symbols.py" --pe "$GAME_DIR/Rayman2.exe" --symdir "$ROOT_DIR/symbols/rayman2" --name rayman2 ${ACP_ARG[@]+"${ACP_ARG[@]}"}
  python3 "$ROOT_DIR/tools/generate_symbols.py" --pe "$GAME_DIR/DLL/GliVd1vf.dll" --symdir "$ROOT_DIR/symbols/glivd1vf" --name glivd1vf
  python3 "$ROOT_DIR/tools/generate_symbols.py" --pe "$GAME_DIR/DLL/WAVPWBVR.dll" --symdir "$ROOT_DIR/symbols/wavpwbvr" --name wavpwbvr
  python3 "$ROOT_DIR/tools/generate_symbols.py" --pe "$GAME_DIR/DLL/APMP5BVR.dll" --symdir "$ROOT_DIR/symbols/apmp5bvr" --name apmp5bvr

  echo "==> Statically lifting Rayman2.exe and plugins into ./generated/ ..."
  "$RECOMP_LIFT" \
    --exe "$GAME_DIR/Rayman2.exe" --symbols "$ROOT_DIR/symbols/rayman2" --manifest "$ROOT_DIR/manifests/rayman2.manifest" \
    --module "$GAME_DIR/DLL/GliVd1vf.dll" --symbols "$ROOT_DIR/symbols/glivd1vf" --manifest "$ROOT_DIR/manifests/glivd1vf.manifest" \
    --module "$GAME_DIR/DLL/WAVPWBVR.dll" --symbols "$ROOT_DIR/symbols/wavpwbvr" --manifest "$ROOT_DIR/manifests/wavpwbvr.manifest" --base 0x11000000 \
    --module "$GAME_DIR/DLL/APMP5BVR.dll" --symbols "$ROOT_DIR/symbols/apmp5bvr" --manifest "$ROOT_DIR/manifests/apmp5bvr.manifest" --base 0x12000000 \
    --out "$ROOT_DIR/generated"
fi

cmake --preset arm64-release
cmake --build "$ROOT_DIR/build" -j"$(sysctl -n hw.ncpu)"

echo "==> Packaging standalone macOS app bundle ..."
RAY2_GAME_DIR="$GAME_DIR" "$ROOT_DIR/packaging/build_app_bundle.sh"

echo ""
echo "Done! Launch the game with:"
echo "  open \"$ROOT_DIR/build/Rayman 2.app\""
