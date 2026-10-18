#!/usr/bin/env bash
# Install the Risk compiler (riskc) and standard library from the latest GitHub release.
#
#   curl -fsSL https://raw.githubusercontent.com/hesed-charis175/Risk/main/install.sh | bash
#
# To uninstall:
#   curl -fsSL https://raw.githubusercontent.com/hesed-charis175/Risk/main/install.sh | bash -s -- uninstall
#
# Optional environment variables:
#   RISK_VERSION     release tag to install, e.g. v0.1.0 (default: latest)
#   INSTALL_DIR      where riskc is copied (default: /usr/local/bin,
#                    or ~/.local/bin when sudo is not available)
#   RISK_STDLIB_DIR  where the standard library goes (default: ~/.risk/lib)
#
# Log legend:  [~] info   [+] passed   [!] warning   [x] failed

set -euo pipefail

REPO="hesed-charis175/Risk"
VERSION="${RISK_VERSION:-latest}"
BINARY_NAME="riskc"
BIN_ARCHIVE="riskc-linux-x86_64.tar.gz"
STD_ARCHIVE="risk-stdlib.tar.gz"
STDLIB_DIR="${RISK_STDLIB_DIR:-$HOME/.risk/lib}"

if [ -n "${RISK_BASE_URL:-}" ]; then
  BASE_URL="$RISK_BASE_URL"
elif [ "$VERSION" = "latest" ]; then
  BASE_URL="https://github.com/$REPO/releases/latest/download"
else
  BASE_URL="https://github.com/$REPO/releases/download/$VERSION"
fi

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

TOTAL_STEPS=7
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

TMP_DIR=""
cleanup() { if [ -n "$TMP_DIR" ]; then rm -rf "$TMP_DIR"; fi; }
trap cleanup EXIT

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
echo -e "${BOLD}Risk installer${RESET}  (${VERSION})"
echo ""

# ---------------------------------------------------------------- 1. system checks
info "Checking your system..."
[ "$(uname -s)" = "Linux" ]  || fail "Unsupported OS: $(uname -s). Risk supports Linux only."
ARCH="$(uname -m)"
[ "$ARCH" = "x86_64" ]       || fail "Unsupported architecture: $ARCH. Only x86_64 is supported right now."
for tool in curl tar sha256sum; do
  command -v "$tool" &>/dev/null || fail "$tool is required but not installed."
done
ok "Linux x86_64, curl and tar found"
advance

# ---------------------------------------------------------------- 2. gcc
info "Checking for gcc (riskc needs it to produce executables)..."
if ! command -v gcc &>/dev/null; then
  info "gcc not found, installing..."
  if command -v apt-get &>/dev/null; then
    $SUDO apt-get update -qq
    $SUDO apt-get install -y gcc
  elif command -v dnf &>/dev/null; then
    $SUDO dnf install -y gcc
  elif command -v yum &>/dev/null; then
    $SUDO yum install -y gcc
  elif command -v pacman &>/dev/null; then
    $SUDO pacman -Sy --noconfirm gcc
  elif command -v apk &>/dev/null; then
    $SUDO apk add --no-cache gcc musl-dev
  else
    fail "No supported package manager found. Install gcc manually and re-run."
  fi
fi
command -v gcc &>/dev/null || fail "gcc installation failed. Install it manually and re-run."
ok "gcc is ready ($(gcc --version | head -1))"
advance

# ---------------------------------------------------------------- 3. download
TMP_DIR="$(mktemp -d)"
download() {
  local name="$1"
  info "Downloading $name..."
  if [ "$TTY" = "1" ]; then
    curl -fL --retry 2 --progress-bar "$BASE_URL/$name" -o "$TMP_DIR/$name" \
      || fail "Failed to download $name. Check your connection and that release '$VERSION' exists."
  else
    curl -fsSL --retry 2 "$BASE_URL/$name" -o "$TMP_DIR/$name" \
      || fail "Failed to download $name. Check your connection and that release '$VERSION' exists."
  fi
}
download "$BIN_ARCHIVE"
download "$STD_ARCHIVE"
if curl -fsSL --retry 2 "$BASE_URL/SHA256SUMS" -o "$TMP_DIR/SHA256SUMS" 2>/dev/null; then
  HAVE_SUMS=1
else
  HAVE_SUMS=0
fi
ok "Downloaded release files"
advance

# ---------------------------------------------------------------- 4. verify
info "Verifying checksums..."
if [ "$HAVE_SUMS" = "1" ]; then
  : > "$TMP_DIR/check.txt"
  for name in "$BIN_ARCHIVE" "$STD_ARCHIVE"; do
    grep " $name\$" "$TMP_DIR/SHA256SUMS" >> "$TMP_DIR/check.txt" \
      || fail "$name is missing from SHA256SUMS."
  done
  ( cd "$TMP_DIR" && sha256sum -c check.txt > /dev/null 2>&1 ) \
    || fail "Checksum mismatch. The download is corrupted or has been tampered with."
  ok "Checksums match"
else
  warn "This release has no SHA256SUMS file, skipping verification"
fi
advance

# ---------------------------------------------------------------- 5. install riskc
info "Installing riskc to $INSTALL_DIR/$BINARY_NAME..."
mkdir -p "$TMP_DIR/bin"
tar -xzf "$TMP_DIR/$BIN_ARCHIVE" -C "$TMP_DIR/bin"
[ -f "$TMP_DIR/bin/riskc" ] || fail "The archive does not contain riskc."
chmod +x "$TMP_DIR/bin/riskc"
if ! mkdir -p "$INSTALL_DIR" 2>/dev/null; then
  $SUDO mkdir -p "$INSTALL_DIR" || fail "Cannot create $INSTALL_DIR."
fi
if [ -w "$INSTALL_DIR" ]; then
  install -m 755 "$TMP_DIR/bin/riskc" "$INSTALL_DIR/$BINARY_NAME"
else
  $SUDO install -m 755 "$TMP_DIR/bin/riskc" "$INSTALL_DIR/$BINARY_NAME" \
    || fail "Failed to install riskc. Do you have sudo access?"
fi
ok "riskc installed to $INSTALL_DIR/$BINARY_NAME"
advance

# ---------------------------------------------------------------- 6. standard library
info "Installing standard library to $STDLIB_DIR..."
mkdir -p "$STDLIB_DIR"
tar -xzf "$TMP_DIR/$STD_ARCHIVE" -C "$STDLIB_DIR" --strip-components=1
ok "Standard library installed"

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
    if ! RISK_STDLIB="$STDLIB_DIR" "$INSTALL_DIR/$BINARY_NAME" "$TARGET" > "$TMP_DIR/stdlib.log" 2>&1; then
      tail -n 20 "$TMP_DIR/stdlib.log" >&2
      fail "Failed to compile $line"
    fi
    ok "compiled $line  ($n/$total_lines)"
  done < "$MANIFEST"
  ok "Standard library compiled"
else
  warn "No manifest at $MANIFEST, skipping stdlib compilation"
fi
advance

# ---------------------------------------------------------------- 7. shell config
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
ok "Shell configured"
advance

# ---------------------------------------------------------------- done
ok "Risk is installed."
echo ""
echo "    Open a new terminal (or run: source ~/.profile), then try:"
echo "      riskc main.risk -o app"
echo ""
echo "    Docs:      https://github.com/$REPO"
echo "    Uninstall: curl -fsSL https://raw.githubusercontent.com/$REPO/main/install.sh | bash -s -- uninstall"
echo ""
