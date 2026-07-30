#!/usr/bin/env bash
#
# launch.sh — interactive launcher for the Micro:bit Remote ESP32 firmware.
#
# Numbered menu to check / install / compile / upload / monitor on either
# PlatformIO or arduino-cli. Run from anywhere; it self-locates.
#
#   chmod +x launch.sh
#   ./launch.sh
#

set -u   # error on unset vars; we handle command failures per-action

# ---------------------------------------------------------------------------
#  Self-locate so the script works no matter where it is invoked from
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

PIO_DIR="$SCRIPT_DIR/platformio"
ACLI_SKETCH_DIR="$SCRIPT_DIR/arduino/esp32_rxy"
ACLI_SKETCH_FILE="$ACLI_SKETCH_DIR/esp32_rxy.ino"
ACLI_BUILD_DIR="$ACLI_SKETCH_DIR/build"

# Board configuration
PIO_ENV="esp32-c3-supermini"
ACLI_CORE="esp32:esp32"
# CDCOnBoot=cdc enables Serial via the C3's native USB peripheral (matches
# the ARDUINO_USB_CDC_ON_BOOT=1 flag in platformio.ini).
ACLI_FQBN="esp32:esp32:esp32c3:CDCOnBoot=cdc,UploadSpeed=115200"
ACLI_BAUD=115200

# Where to install arduino-cli if not already on PATH
ACLI_INSTALL_DIR="$HOME/.local/bin"

# Optional manual port override (blank = let the tool auto-detect)
USER_PORT=""

# ---------------------------------------------------------------------------
#  Colors / logging
# ---------------------------------------------------------------------------
if [[ -t 1 ]]; then
  BOLD=$'\033[1m';   DIM=$'\033[2m'
  RED=$'\033[1;31m'; GREEN=$'\033[1;32m'; YELLOW=$'\033[1;33m'
  BLUE=$'\033[1;34m'; CYAN=$'\033[1;36m'; NC=$'\033[0m'
else
  BOLD=''; DIM=''; RED=''; GREEN=''; YELLOW=''; BLUE=''; CYAN=''; NC=''
fi

log()     { printf '%b\n' "$*"; }
ok()      { log "${GREEN}✓${NC} $*"; }
err()     { log "${RED}✗${NC} $*"; }
info()    { log "${BLUE}→${NC} $*"; }
warn()    { log "${YELLOW}!${NC} $*"; }
section() { log ""; log "${BOLD}${CYAN}=== $* ===${NC}"; }

pause() {
  log ""
  read -r -n 1 -s -p "Press any key to return to the menu..." _ || true
  log ""
}

confirm() {
  local prompt="${1:-Continue?}" ans
  read -r -p "$prompt [y/N] " ans
  [[ "$ans" == y* || "$ans" == Y* ]]
}

has_cmd() { command -v "$1" >/dev/null 2>&1; }

ensure_local_bin_in_path() {
  case ":$PATH:" in
    *":$HOME/.local/bin:"*) ;;
    *) export PATH="$HOME/.local/bin:$PATH" ;;
  esac
}
ensure_local_bin_in_path

# ---------------------------------------------------------------------------
#  USB port helpers
# ---------------------------------------------------------------------------
detect_ports_glob() {
  local found=0 pat p
  shopt -s nullglob
  for pat in \
      /dev/ttyACM* /dev/ttyUSB* \
      /dev/cu.usbmodem* /dev/cu.usbserial* \
      /dev/cu.SLAB_USBtoUART* /dev/cu.wchusbserial*; do
    for p in $pat; do
      [[ -e "$p" ]] || continue
      log "  $p"
      found=1
    done
  done
  shopt -u nullglob
  [[ $found -eq 1 ]]
}

show_ports_generic() {
  section "USB serial ports (glob scan)"
  detect_ports_glob || warn "No USB serial devices detected via /dev/."
  log ""
  if [[ -n "$USER_PORT" ]]; then
    info "Manual port override: ${BOLD}$USER_PORT${NC}"
  else
    info "No manual override — tools will auto-detect."
  fi
}

set_port_interactive() {
  section "Set port manually"
  log "Current port: ${USER_PORT:-<auto>}"
  log "Detected devices:"
  detect_ports_glob || warn "  (none)"
  log ""
  local p
  read -r -p "New port (blank = clear / auto-detect): " p
  USER_PORT="$p"
  if [[ -z "$USER_PORT" ]]; then ok "Port cleared. Tools will auto-detect."
  else                           ok "Port set to: $USER_PORT"
  fi
}

# ===========================================================================
#  PlatformIO actions
# ===========================================================================
pio_check() {
  section "PlatformIO — environment check"

  has_cmd python3 && ok "python3: $(python3 --version 2>&1)" \
                  || err "python3 not found"
  has_cmd pip3    && ok "pip3:    present" \
                  || warn "pip3 not found (needed unless using pipx)"
  has_cmd pipx    && ok "pipx:    $(pipx --version 2>&1)" \
                  || info "pipx not found (optional; preferred installer)"

  if has_cmd pio; then
    ok "platformio (pio): $(pio --version 2>&1)"
  else
    err "platformio (pio) not found — menu option 2 will install it"
  fi

  log ""
  [[ -f "$PIO_DIR/platformio.ini" ]] \
      && ok "platformio.ini: $PIO_DIR/platformio.ini" \
      || err "Missing $PIO_DIR/platformio.ini"

  log ""
  log "USB devices:"
  detect_ports_glob || warn "  (none detected)"
}

pio_install() {
  section "PlatformIO — install / update"

  if has_cmd pio; then
    info "PlatformIO is already installed: $(pio --version 2>&1)"
    if ! confirm "Re-run the installer to update?"; then return 0; fi
  fi

  local os
  os="$(uname -s)"

  # Strategy 1: pipx (preferred everywhere when available)
  if has_cmd pipx; then
    info "Installing/upgrading via pipx..."
    if pipx list 2>/dev/null | grep -qi 'package platformio'; then
      pipx upgrade platformio || { err "pipx upgrade failed"; return 1; }
    else
      pipx install platformio || { err "pipx install failed"; return 1; }
    fi
    ok "Installed via pipx"

  # Strategy 2: Homebrew on macOS — has a `platformio` formula directly
  elif [[ "$os" == "Darwin" ]] && has_cmd brew; then
    info "Installing via Homebrew (brew install platformio)..."
    if brew list platformio >/dev/null 2>&1; then
      brew upgrade platformio || warn "brew upgrade returned non-zero"
    else
      brew install platformio || { err "brew install platformio failed"; return 1; }
    fi
    ok "Installed via Homebrew"

  # Strategy 3: pip3 --user, with explicit PEP 668 handling
  elif has_cmd pip3; then
    info "Installing via 'pip3 install --user'..."
    local logfile rc
    logfile="$(mktemp)"
    pip3 install --user --upgrade platformio >"$logfile" 2>&1
    rc=$?
    cat "$logfile"
    if [[ $rc -eq 0 ]]; then
      ok "Installed via pip3 --user"
      rm -f "$logfile"
    elif grep -q 'externally-managed-environment' "$logfile"; then
      log ""
      warn "pip3 refused because this Python is marked externally managed (PEP 668)."
      if [[ "$os" == "Darwin" ]]; then
        warn "Recommended fix on macOS:"
        log  "    brew install platformio"
        log  "  or:"
        log  "    brew install pipx && pipx install platformio"
      else
        warn "Recommended fix on Debian/Ubuntu:"
        log  "    sudo apt install pipx"
        log  "    pipx install platformio"
      fi
      log ""
      if confirm "Override with '--break-system-packages' (not recommended)?"; then
        pip3 install --user --upgrade --break-system-packages platformio \
          || { err "pip3 install failed"; rm -f "$logfile"; return 1; }
        ok "Installed via pip3 --break-system-packages"
      else
        info "Cancelled. Run one of the suggestions above, then choose this menu option again."
        rm -f "$logfile"
        return 1
      fi
      rm -f "$logfile"
    else
      err "pip3 install failed (see output above)"
      rm -f "$logfile"
      return 1
    fi

  # Strategy 4: nothing useful is installed
  else
    err "No installer available — pipx, brew, and pip3 are all missing."
    if [[ "$os" == "Darwin" ]]; then
      err "Install Homebrew first (https://brew.sh) then re-run this option."
    else
      err "On Debian/Ubuntu: 'sudo apt install pipx' then re-run this option."
    fi
    return 1
  fi

  ensure_local_bin_in_path
  if has_cmd pio; then
    ok "pio on PATH: $(pio --version)"
  else
    warn "pio is installed but not yet on PATH in this shell. Try:"
    log  "    hash -r        # refresh command lookup table"
    log  "    pio --version  # should now resolve"
    log  "  If it still does not, open a new shell or add to ~/.zshrc / ~/.bashrc:"
    log  "    export PATH=\"\$HOME/.local/bin:\$PATH\""
  fi
}

pio_compile() {
  section "PlatformIO — compile"
  has_cmd pio || { err "pio not installed (menu option 2)"; return 1; }
  ( cd "$PIO_DIR" && pio run -e "$PIO_ENV" )
}

pio_upload() {
  section "PlatformIO — upload"
  has_cmd pio || { err "pio not installed (menu option 2)"; return 1; }
  local args=( run -e "$PIO_ENV" -t upload )
  [[ -n "$USER_PORT" ]] && args+=( --upload-port "$USER_PORT" )
  ( cd "$PIO_DIR" && pio "${args[@]}" )
}

pio_monitor() {
  section "PlatformIO — serial monitor"
  has_cmd pio || { err "pio not installed (menu option 2)"; return 1; }
  log "${DIM}(Ctrl+C to exit the monitor.)${NC}"
  local args=( device monitor -b 115200 )
  [[ -n "$USER_PORT" ]] && args+=( -p "$USER_PORT" )
  ( cd "$PIO_DIR" && pio "${args[@]}" )
}

pio_clean() {
  section "PlatformIO — clean build"
  has_cmd pio || { err "pio not installed (menu option 2)"; return 1; }
  confirm "Clean the build directory?" || { info "Cancelled."; return 0; }
  ( cd "$PIO_DIR" && pio run -e "$PIO_ENV" -t clean )
}

pio_show_ports() {
  section "PlatformIO — device list"
  has_cmd pio || { err "pio not installed (menu option 2)"; return 1; }
  pio device list
  log ""
  show_ports_generic
}

pio_do_everything() {
  section "PlatformIO — install → compile → upload → monitor"
  pio_install  || { err "Install step failed";  return 1; }
  pio_compile  || { err "Compile step failed";  return 1; }
  pio_upload   || { err "Upload step failed";   return 1; }
  pio_monitor
}

# ===========================================================================
#  arduino-cli actions
# ===========================================================================
acli_check() {
  section "arduino-cli — environment check"

  if has_cmd arduino-cli; then
    ok "arduino-cli: $(arduino-cli version 2>&1 | head -1)"
  else
    err "arduino-cli not found — menu option 2 will install it"
  fi

  if has_cmd arduino-cli; then
    log ""
    log "Installed cores:"
    arduino-cli core list 2>/dev/null | sed 's/^/    /' \
      || warn "    (core list empty — install option 2)"

    log ""
    log "NimBLE library status:"
    if arduino-cli lib list 2>/dev/null | grep -qi '^NimBLE-Arduino'; then
      arduino-cli lib list 2>/dev/null | grep -i '^NimBLE-Arduino' | sed 's/^/    /'
    else
      warn "    NimBLE-Arduino not installed (option 2 installs it)"
    fi
  fi

  log ""
  [[ -f "$ACLI_SKETCH_FILE" ]] \
      && ok "Sketch: $ACLI_SKETCH_FILE" \
      || err "Missing $ACLI_SKETCH_FILE"

  log ""
  log "USB devices:"
  detect_ports_glob || warn "  (none detected)"
}

acli_install() {
  section "arduino-cli — install / update"

  if ! has_cmd arduino-cli; then
    info "Installing arduino-cli into $ACLI_INSTALL_DIR ..."
    mkdir -p "$ACLI_INSTALL_DIR"
    if ! curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh \
         | BINDIR="$ACLI_INSTALL_DIR" sh; then
      err "arduino-cli install failed"
      return 1
    fi
    ensure_local_bin_in_path
    has_cmd arduino-cli || { err "arduino-cli not on PATH after install"; return 1; }
    ok "arduino-cli installed: $(arduino-cli version 2>&1 | head -1)"
  else
    ok "arduino-cli already installed: $(arduino-cli version 2>&1 | head -1)"
  fi

  info "Updating core index..."
  arduino-cli core update-index || { err "core update-index failed"; return 1; }

  if arduino-cli core list 2>/dev/null | awk '{print $1}' | grep -qx "$ACLI_CORE"; then
    info "ESP32 core present — upgrading..."
    arduino-cli core upgrade "$ACLI_CORE" || warn "core upgrade returned non-zero"
  else
    info "Installing ESP32 core ($ACLI_CORE)..."
    arduino-cli core install "$ACLI_CORE" || { err "core install failed"; return 1; }
  fi

  if arduino-cli lib list 2>/dev/null | grep -qi '^NimBLE-Arduino'; then
    info "NimBLE-Arduino present — upgrading..."
    arduino-cli lib upgrade "NimBLE-Arduino" || warn "lib upgrade returned non-zero"
  else
    info "Installing NimBLE-Arduino library..."
    arduino-cli lib install "NimBLE-Arduino" || { err "lib install failed"; return 1; }
  fi

  ok "arduino-cli environment ready."
}

acli_compile() {
  section "arduino-cli — compile"
  has_cmd arduino-cli || { err "arduino-cli not installed (menu option 2)"; return 1; }
  arduino-cli compile \
    --fqbn "$ACLI_FQBN" \
    --build-path "$ACLI_BUILD_DIR" \
    "$ACLI_SKETCH_DIR"
}

# Resolve port: manual override > first board-list entry > empty
acli_resolve_port() {
  if [[ -n "$USER_PORT" ]]; then
    printf '%s' "$USER_PORT"
    return 0
  fi
  arduino-cli board list 2>/dev/null \
    | awk 'NR>1 && $1 ~ /^\// {print $1; exit}'
}

acli_upload() {
  section "arduino-cli — upload"
  has_cmd arduino-cli || { err "arduino-cli not installed (menu option 2)"; return 1; }
  local port
  port="$(acli_resolve_port)"
  if [[ -z "$port" ]]; then
    err "No port detected. Set one with menu option 8."
    return 1
  fi
  info "Using port: $port"
  arduino-cli upload -p "$port" --fqbn "$ACLI_FQBN" "$ACLI_SKETCH_DIR"
}

acli_monitor() {
  section "arduino-cli — serial monitor"
  has_cmd arduino-cli || { err "arduino-cli not installed (menu option 2)"; return 1; }
  local port
  port="$(acli_resolve_port)"
  if [[ -z "$port" ]]; then
    err "No port detected. Set one with menu option 8."
    return 1
  fi
  log "${DIM}(Ctrl+C to exit the monitor.)${NC}"
  arduino-cli monitor -p "$port" -c "baudrate=$ACLI_BAUD"
}

acli_clean() {
  section "arduino-cli — clean build"
  if [[ ! -d "$ACLI_BUILD_DIR" ]]; then
    info "No build directory to clean ($ACLI_BUILD_DIR)."
    return 0
  fi
  confirm "Remove $ACLI_BUILD_DIR?" || { info "Cancelled."; return 0; }
  rm -rf "$ACLI_BUILD_DIR" && ok "Removed $ACLI_BUILD_DIR"
}

acli_show_ports() {
  section "arduino-cli — board list"
  has_cmd arduino-cli || { err "arduino-cli not installed (menu option 2)"; return 1; }
  arduino-cli board list
  log ""
  show_ports_generic
}

acli_do_everything() {
  section "arduino-cli — install → compile → upload → monitor"
  acli_install || { err "Install step failed"; return 1; }
  acli_compile || { err "Compile step failed"; return 1; }
  acli_upload  || { err "Upload step failed";  return 1; }
  acli_monitor
}

# ===========================================================================
#  Menus
# ===========================================================================
TOOLCHAIN=""   # "pio" or "acli"

print_header() {
  clear || true
  log "${BOLD}${CYAN}╔════════════════════════════════════════════════╗${NC}"
  log "${BOLD}${CYAN}║   Micro:bit Remote — ESP32 firmware launcher   ║${NC}"
  log "${BOLD}${CYAN}╚════════════════════════════════════════════════╝${NC}"
  log "${DIM}  Project : $SCRIPT_DIR${NC}"
  case "$TOOLCHAIN" in
    pio)  log "${DIM}  Toolchain: PlatformIO   |   Port: ${USER_PORT:-<auto>}${NC}" ;;
    acli) log "${DIM}  Toolchain: arduino-cli  |   Port: ${USER_PORT:-<auto>}${NC}" ;;
  esac
  log ""
}

pick_toolchain() {
  while true; do
    print_header
    log "  ${BOLD}${YELLOW}P)${NC} PlatformIO"
    log "  ${BOLD}${YELLOW}A)${NC} arduino-cli"
    log "  ${BOLD}${YELLOW}Q)${NC} Quit"
    log ""
    local choice
    read -r -p "Pick a toolchain: " choice
    case "$choice" in
      [Pp]) TOOLCHAIN="pio";  return 0 ;;
      [Aa]) TOOLCHAIN="acli"; return 0 ;;
      [Qq]) exit 0 ;;
      *) ;;
    esac
  done
}

dispatch() {
  # $1 = action shortname
  #
  # NOTE: do NOT use the `[[ cond ]] && A || B` idiom here — it is not
  # equivalent to if/then/else. When `A` returns non-zero (e.g. compile
  # fails), the `||` triggers `B` and we end up running BOTH branches.
  case "$1" in
    check)
      if [[ "$TOOLCHAIN" == pio ]]; then pio_check;          else acli_check;          fi ;;
    install)
      if [[ "$TOOLCHAIN" == pio ]]; then pio_install;        else acli_install;        fi ;;
    compile)
      if [[ "$TOOLCHAIN" == pio ]]; then pio_compile;        else acli_compile;        fi ;;
    upload)
      if [[ "$TOOLCHAIN" == pio ]]; then pio_upload;         else acli_upload;         fi ;;
    monitor)
      if [[ "$TOOLCHAIN" == pio ]]; then pio_monitor;        else acli_monitor;        fi ;;
    clean)
      if [[ "$TOOLCHAIN" == pio ]]; then pio_clean;          else acli_clean;          fi ;;
    ports)
      if [[ "$TOOLCHAIN" == pio ]]; then pio_show_ports;     else acli_show_ports;     fi ;;
    doall)
      if [[ "$TOOLCHAIN" == pio ]]; then pio_do_everything;  else acli_do_everything;  fi ;;
  esac
}

action_menu() {
  while true; do
    print_header
    log "  ${BOLD}${YELLOW}1)${NC} Check environment"
    log "  ${BOLD}${YELLOW}2)${NC} Install / update toolchain"
    log "  ${BOLD}${YELLOW}3)${NC} Compile firmware"
    log "  ${BOLD}${YELLOW}4)${NC} Upload firmware"
    log "  ${BOLD}${YELLOW}5)${NC} Open serial monitor"
    log "  ${BOLD}${YELLOW}6)${NC} Clean build"
    log "  ${BOLD}${YELLOW}7)${NC} Show detected USB ports"
    log "  ${BOLD}${YELLOW}8)${NC} Set port manually  ${DIM}(current: ${USER_PORT:-<auto>})${NC}"
    log "  ${BOLD}${YELLOW}9)${NC} Do everything (install → compile → upload → monitor)"
    log "  ${BOLD}${YELLOW}0)${NC} Switch toolchain"
    log "  ${BOLD}${YELLOW}Q)${NC} Quit"
    log ""
    local choice
    read -r -p "Choice: " choice
    case "$choice" in
      1) dispatch check;   pause ;;
      2) dispatch install; pause ;;
      3) dispatch compile; pause ;;
      4) dispatch upload;  pause ;;
      5) dispatch monitor; pause ;;
      6) dispatch clean;   pause ;;
      7) dispatch ports;   pause ;;
      8) set_port_interactive; pause ;;
      9) dispatch doall;   pause ;;
      0) pick_toolchain ;;
      [Qq]) exit 0 ;;
      *) ;;
    esac
  done
}

# ---------------------------------------------------------------------------
#  Main
# ---------------------------------------------------------------------------
pick_toolchain
action_menu
