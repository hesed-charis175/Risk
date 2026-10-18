#!/usr/bin/env bash
# Build and install the Risk compiler (riskc) from source.
#
#   ./build.sh              build, install riskc and the standard library
#   ./build.sh uninstall    remove everything this script installed
#
# Optional environment variables:
#   INSTALL_DIR      where riskc is copied (by default: /usr/local/bin,
#                    or ~/.local/bin when sudo is not available)
#   RISK_STDLIB_DIR  where standard library goes (default: ~/.risk/lib)
#
# Log legend:  [~] info   [+] passed   [!] warning   [x] failed

set -euo pipefail

BINARY_NAME="riskc"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
STDLIB_SRC="$SCRIPT_DIR/stdlib"
STDLIB_DIR="${RISK_STDLIB_DIR:-$HOME/.risk/lib}"

MARK_START="# >>> risk >>>"
MARK_END="# <<< risk <<<"

# ---------------------------------------------------------------- output
if [ -t 1 ]; then
  TTY=1
  BOLD="\033[1m"; GREEN="\033[0;32m"; RED="\033[0;31m"; YELLOW="\033[0;33m"; RESET="\033[0m"
else
  TTY=0
  BOLD=""; GREEN=""; RED=""; YELLOW=""; RESET=""
fi

info() { echo -e "${BOLD}[~]${RESET} $1"; }
ok()   { echo -e "${GREEN}[+]${RESET} $1"; }
warn() { echo -e "${YELLOW}[!]${RESET} $1"; }
fail() { echo -e "${RED}[x]${RESET} $1" >&2; exit 1; }

# bar_string PERCENT  ->  [=========>          ]
bar_string() {
  local pct=$1 width=30 filled i out=""
  filled=$(( pct * width / 100 ))
  for ((i = 0; i < width; i++)); do
    if   [ "$i" -lt "$filled" ]; then out+="="
    elif [ "$i" -eq "$filled" ] && [ "$pct" -lt 100 ]; then out+=">"
    else out+=" "
    fi
  done
  printf '[%s]' "$out"
}

# live single-line bar used while building
draw_bar() {
  printf "\r\033[K${BOLD}[~]${RESET} %s %3d%% %s" "$(bar_string "$1")" "$1" "$2"
}

TOTAL_STEPS=6
DONE_STEPS=0
advance() {
  DONE_STEPS=$((DONE_STEPS + 1))
  local pct=$(( DONE_STEPS * 100 / TOTAL_STEPS ))
  echo -e "${BOLD}[~]${RESET} $(bar_string "$pct") ${pct}%  (step ${DONE_STEPS}/${TOTAL_STEPS})"
  echo ""
}

# ---------------------------------------------------------------- setup
SUDO=""
if [ "$(id -u)" != "0" ] && command -v sudo &>/dev/null; then
  SUDO="sudo"
fi

if [ -z "${INSTALL_DIR:-}" ]; then
  if [ "$(id -u)" = "0" ] || [ -n "$SUDO" ]; then
    INSTALL_DIR="/usr/local/bin"
  else
    INSTALL_DIR="$HOME/.local/bin"
  fi
fi

RC_FILES=("$HOME/.profile" "$HOME/.bashrc" "$HOME/.zshrc")

remove_rc_block() {
  local f="$1"
  [ -f "$f" ] || return 0
  if grep -qF "$MARK_START" "$f"; then
    sed -i "\|$MARK_START|,\|$MARK_END|d" "$f"
  fi
}

# ---------------------------------------------------------------- uninstall
if [ "${1:-}" = "uninstall" ]; then
  info "Uninstalling Risk..."
  if [ -e "$INSTALL_DIR/$BINARY_NAME" ]; then
    if [ -w "$INSTALL_DIR" ]; then rm -f "$INSTALL_DIR/$BINARY_NAME"
    else $SUDO rm -f "$INSTALL_DIR/$BINARY_NAME"; fi
    ok "Removed $INSTALL_DIR/$BINARY_NAME"
  else
    warn "No binary found at $INSTALL_DIR/$BINARY_NAME"
  fi
  if [ -d "$STDLIB_DIR" ]; then
    rm -rf "$STDLIB_DIR"
    rmdir "$(dirname "$STDLIB_DIR")" 2>/dev/null || true
    ok "Removed $STDLIB_DIR"
  fi
  for f in "${RC_FILES[@]}"; do remove_rc_block "$f"; done
  ok "Removed Risk settings from your shell config"
  exit 0
fi

echo ""
echo -e "${BOLD}Risk installer${RESET}"
echo ""

[ "$(uname -s)" = "Linux" ] || fail "This script supports Linux only."
[ -f "$SCRIPT_DIR/CMakeLists.txt" ] || fail "CMakeLists.txt not found. Run this script from the Risk repository."

# ---------------------------------------------------------------- 1. dependencies
info "Checking build dependencies (gcc, g++, cmake, make)..."
need_install=0
for tool in gcc g++ cmake make; do
  command -v "$tool" &>/dev/null || need_install=1
done

if [ "$need_install" = "1" ]; then
  info "Installing missing dependencies..."
  if command -v apt-get &>/dev/null; then
    $SUDO apt-get update -qq
    $SUDO apt-get install -y gcc g++ cmake make
  elif command -v dnf &>/dev/null; then
    $SUDO dnf install -y gcc gcc-c++ cmake make
  elif command -v yum &>/dev/null; then
    $SUDO yum install -y gcc gcc-c++ cmake make
  elif command -v pacman &>/dev/null; then
    $SUDO pacman -Sy --noconfirm gcc cmake make
  elif command -v apk &>/dev/null; then
    $SUDO apk add --no-cache build-base cmake
  else
    fail "No supported package manager found. Install gcc, g++, cmake and make, then re-run."
  fi
fi

for tool in gcc g++ cmake make; do
  command -v "$tool" &>/dev/null || fail "$tool is still missing. Please install it and re-run."
done

CMAKE_VER="$(cmake --version | head -1 | awk '{print $3}')"
if [ "$(printf '%s\n%s\n' "3.10" "$CMAKE_VER" | sort -V | head -1)" != "3.10" ]; then
  fail "CMake 3.10 or newer is required (found $CMAKE_VER)."
fi
ok "Dependencies ready (gcc $(gcc -dumpversion), cmake $CMAKE_VER)"
advance

# ---------------------------------------------------------------- 2. build
info "Building riskc..."
mkdir -p "$BUILD_DIR"
LOG="$BUILD_DIR/build.log"
: > "$LOG"

if ! cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release >> "$LOG" 2>&1; then
  tail -n 20 "$LOG" >&2
  fail "CMake configuration failed (full log: $LOG)"
fi

pct_re='^\[ *([0-9]+)%\]'
set +e
cmake --build "$BUILD_DIR" -j"$(nproc 2>/dev/null || echo 2)" 2>&1 | tee -a "$LOG" | while IFS= read -r line; do
  if [ "$TTY" = "1" ] && [[ $line =~ $pct_re ]]; then
    draw_bar "${BASH_REMATCH[1]}" "Building"
  fi
done
build_rc=${PIPESTATUS[0]}
set -e
if [ "$TTY" = "1" ]; then printf "\r\033[K"; fi

if [ "$build_rc" -ne 0 ]; then
  tail -n 20 "$LOG" >&2
  fail "Build failed (full log: $LOG)"
fi
[ -f "$BUILD_DIR/$BINARY_NAME" ] || fail "Build finished but $BUILD_DIR/$BINARY_NAME was not produced."
ok "Built $BUILD_DIR/$BINARY_NAME"
advance

# ---------------------------------------------------------------- 3. install binary
info "Installing to $INSTALL_DIR/$BINARY_NAME..."
if ! mkdir -p "$INSTALL_DIR" 2>/dev/null; then
  $SUDO mkdir -p "$INSTALL_DIR" || fail "Cannot create $INSTALL_DIR."
fi
if [ -w "$INSTALL_DIR" ]; then
  install -m 755 "$BUILD_DIR/$BINARY_NAME" "$INSTALL_DIR/$BINARY_NAME"
else
  $SUDO install -m 755 "$BUILD_DIR/$BINARY_NAME" "$INSTALL_DIR/$BINARY_NAME" \
    || fail "Failed to install the binary. Do you have sudo access?"
fi
ok "riskc installed to $INSTALL_DIR/$BINARY_NAME"
advance

# ---------------------------------------------------------------- 4. stdlib files
if [ -d "$STDLIB_SRC" ]; then
  info "Installing standard library to $STDLIB_DIR..."
  mkdir -p "$STDLIB_DIR"
  cp -a "$STDLIB_SRC/." "$STDLIB_DIR/"
  ok "Standard library installed"
else
  warn "$STDLIB_SRC not found, skipping standard library"
fi
advance

# ---------------------------------------------------------------- 5. stdlib compile
MANIFEST="$STDLIB_DIR/manifest"
if [ -f "$MANIFEST" ]; then
  info "Compiling standard library..."
  total_lines=$(grep -cvE '^\s*(#|$)' "$MANIFEST" || true)
  [ "$total_lines" -gt 0 ] || total_lines=1
  n=0
  while IFS= read -r line || [ -n "$line" ]; do
    [[ -z "$line" || "$line" == \#* ]] && continue
    TARGET="$STDLIB_DIR/$line"
    n=$((n + 1))
    if [ ! -f "$TARGET" ]; then
      warn "$line not found, skipping"
      continue
    fi
    if ! RISK_STDLIB="$STDLIB_DIR" "$INSTALL_DIR/$BINARY_NAME" "$TARGET" >> "$LOG" 2>&1; then
      tail -n 20 "$LOG" >&2
      fail "Failed to compile $line"
    fi
    ok "compiled $line  ($n/$total_lines)"
  done < "$MANIFEST"
  ok "Standard library compiled"
else
  warn "No manifest at $MANIFEST, skipping standard library compilation"
fi
advance

# ---------------------------------------------------------------- 6. environment
info "Configuring your shell (PATH and RISK_STDLIB)..."
touch "$HOME/.profile"
for f in "${RC_FILES[@]}"; do
  [ -f "$f" ] || continue
  remove_rc_block "$f"
  {
    echo "$MARK_START"
    echo "export RISK_STDLIB=\"$STDLIB_DIR\""
    case ":$PATH:" in
      *":$INSTALL_DIR:"*) ;;
      *) echo "export PATH=\"$INSTALL_DIR:\$PATH\"" ;;
    esac
    echo "$MARK_END"
  } >> "$f"
done
export RISK_STDLIB="$STDLIB_DIR"
ok "Shell configured"
advance

# ---------------------------------------------------------------- done
ok "Risk is installed."
echo ""
echo "    Open a new terminal (or run: source ~/.profile), then try:"
echo "      riskc main.risk -o app"
echo ""
echo "    To uninstall later: ./build.sh uninstall"
echo ""
