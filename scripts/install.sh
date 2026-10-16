#!/usr/bin/env bash
# TaskTree — one-shot installer for Ubuntu/Debian. Installs build dependencies, builds the
# overlay app and the `tt` CLI from source, and puts BOTH `tt` (store) and `tasktree`
# (app lifecycle) on your PATH so they work out of the box. Idempotent — safe to re-run.
#
#   ./scripts/install.sh            # deps + PROD build (optimized) + install
#   ./scripts/install.sh --dev      # dev build (build/, dev features on) instead
#   ./scripts/install.sh --no-deps  # skip apt (dependencies already present)
set -euo pipefail

REPO="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
BINDIR="$HOME/.local/bin"
PRESET=prod
BUILDDIR="$REPO/build-prod"
DO_DEPS=1

for a in "$@"; do
  case "$a" in
    --no-deps) DO_DEPS=0 ;;
    --dev)     PRESET=dev; BUILDDIR="$REPO/build" ;;
    -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
    *) echo "unknown option: $a (try --help)" >&2; exit 1 ;;
  esac
done

say() { printf '\033[1;36m==>\033[0m %s\n' "$*"; }

# 1. System dependencies (Ubuntu/Debian). X11 + GL for the overlay, GLib/GIO for the tray,
#    OpenSSL for the cloud LLM, python3 for the ttd SessionStart hook.
if [[ "$DO_DEPS" == 1 ]]; then
  if command -v apt-get >/dev/null 2>&1; then
    say "Installing build dependencies (sudo apt-get)…"
    sudo apt-get update -qq
    sudo apt-get install -y --no-install-recommends \
      build-essential ninja-build git pkg-config python3 python3-pip \
      xorg-dev libgl1-mesa-dev libssl-dev libglib2.0-dev cmake
  else
    say "No apt-get here — install equivalents of: build-essential ninja git pkg-config \
python3 xorg-dev libgl1-mesa-dev libssl-dev libglib2.0-dev cmake(>=3.24), then rerun --no-deps."
  fi
fi

# 2. CMake >= 3.24 is required (Ubuntu 22.04 ships 3.22). If apt's is too old, install a
#    userspace copy with pip — it lands in ~/.local/bin, which we ensure is on PATH below.
need_cmake=1
if command -v cmake >/dev/null 2>&1; then
  ver="$(cmake --version | head -1 | grep -oE '[0-9]+(\.[0-9]+)+' | head -1)"
  IFS=. read -r cmajor cminor _ <<<"$ver"
  if (( cmajor > 3 || (cmajor == 3 && cminor >= 24) )); then need_cmake=0; fi
fi
if [[ "$need_cmake" == 1 ]]; then
  say "cmake >= 3.24 not found — installing a userspace copy via pip…"
  python3 -m pip install --user --upgrade cmake
  export PATH="$HOME/.local/bin:$PATH"
fi

# 3. Build the overlay + the tt CLI. The first build fetches deps (GLFW, NanoVG, json,
#    toml++, httplib, SQLite) via CMake FetchContent, so it needs network.
say "Configuring ($PRESET) and building — first build fetches deps and needs network…"
cmake --preset "$PRESET"
cmake --build "$BUILDDIR" -j

[[ -x "$BUILDDIR/tt" ]]       || { echo "build did not produce $BUILDDIR/tt" >&2; exit 1; }
[[ -x "$BUILDDIR/tasktree" ]] || { echo "build did not produce $BUILDDIR/tasktree" >&2; exit 1; }

# 4. Put both commands on PATH. Symlinks (not copies), so a later rebuild is picked up with
#    no reinstall; `tasktree` points at the repo-tracked launcher script.
mkdir -p "$BINDIR"
ln -sfn "$BUILDDIR/tt"            "$BINDIR/tt"
ln -sfn "$REPO/scripts/tasktree"  "$BINDIR/tasktree"
say "Linked $BINDIR/tt        -> $BUILDDIR/tt"
say "Linked $BINDIR/tasktree  -> $REPO/scripts/tasktree"

# 5. Ensure ~/.local/bin is on PATH for future shells.
if ! printf '%s' ":${PATH}:" | grep -q ":${BINDIR}:"; then
  rc="$HOME/.bashrc"; [[ "${SHELL:-}" == *zsh ]] && rc="$HOME/.zshrc"
  if ! grep -qs '\.local/bin' "$rc" 2>/dev/null; then
    printf '\n# added by TaskTree install.sh\nexport PATH="$HOME/.local/bin:$PATH"\n' >> "$rc"
    say "Added ~/.local/bin to PATH in $rc — open a new shell, or: source $rc"
  fi
fi

# 6. Private env file (mode 600): the Groq key plus TASKTREE_BIN, so `tasktree` launches
#    exactly the build this run produced. Created if absent (migrating a key from the old
#    ~/.init-scripts/tasktree.sh); TASKTREE_BIN is refreshed every run. Never enters the repo.
cfg="${XDG_CONFIG_HOME:-$HOME/.config}/tasktree/env"
mkdir -p "$(dirname "$cfg")"
if [[ ! -f "$cfg" ]]; then
  key="${GROQ_API_KEY:-}"
  if [[ -z "$key" && -f "$HOME/.init-scripts/tasktree.sh" ]]; then
    key="$(grep -oE 'GROQ_API_KEY="[^"]*"' "$HOME/.init-scripts/tasktree.sh" 2>/dev/null \
           | head -1 | sed -E 's/.*="([^"]*)"/\1/')"
  fi
  ( umask 077; printf '# TaskTree local secrets + settings (mode 600). Not tracked by git.\nGROQ_API_KEY="%s"\n' \
      "$key" > "$cfg" )
  [[ -n "$key" ]] && say "Wrote your Groq key to $cfg (private, 600)." \
                  || say "Created $cfg — add GROQ_API_KEY=… to enable LLM auto-classification (optional)."
fi
# Pin the launcher to the build we just installed (idempotent upsert; keeps the key line).
umask 077; tmp="$(mktemp)"
grep -v '^TASKTREE_BIN=' "$cfg" > "$tmp" 2>/dev/null || true
printf 'TASKTREE_BIN="%s"\n' "$BUILDDIR/tasktree" >> "$tmp"
cat "$tmp" > "$cfg"; rm -f "$tmp"; chmod 600 "$cfg"
say "Pinned tasktree -> $BUILDDIR/tasktree (in $cfg; override with TASKTREE_BIN)."

# 7. Flag the old shell alias, which would shadow the new PATH command.
if grep -qs 'alias tasktree=' "$HOME/.zshrc" 2>/dev/null; then
  say "Note: ~/.zshrc has 'alias tasktree=…' that shadows the new command. Remove it with:"
  echo "        sed -i.bak '/alias tasktree=/d' ~/.zshrc   # keeps a .bak"
fi

say "Done ($PRESET build).  tt --help   ·   tasktree status   ·   tasktree start"
