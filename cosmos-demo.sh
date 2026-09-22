#!/usr/bin/env bash
# Launch a COSMOS 4.5.3 GUI tool, backed by the Qt 6 binding in
# ext/cosmos/ext/qt6 (built from this checkout, not qtbindings/Qt 4.8).
#
#   ./cosmos-demo.sh                 # Launcher (the tool menu)
#   ./cosmos-demo.sh CmdTlmServer    # a specific tool
#   ./cosmos-demo.sh --list          # show available tools
#
# Environment:
#   COSMOS_DEMO         project directory (default: ~/cosmos_demo)
#   QT_QPA_PLATFORM     set to "offscreen" to run with no window (headless test)
#
# CI=1 is required on every invocation: it makes the Gemfile skip the
# DART/Rails stack and makes cosmos.gemspec skip the (unavailable) qtbindings
# gem. Qt 6 comes from the locally built ext/cosmos/ext/qt6 extension instead.
set -euo pipefail

COSMOS_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEMO="${COSMOS_DEMO:-$HOME/cosmos_demo}"
TOOL="${1:-Launcher}"

if [ ! -d "$DEMO" ]; then
  echo "COSMOS project not found: $DEMO" >&2
  echo "Create one with: $COSMOS_ROOT/cosmos-run.sh ruby bin/cosmos demo $DEMO" >&2
  exit 1
fi

if [ "$TOOL" = "--list" ]; then
  echo "Tools available in $DEMO/tools:"
  ls "$DEMO/tools" | grep -vE '\.bat$|\.rb$|^mac$' | sed 's/^/  /'
  exit 0
fi

if [ ! -f "$DEMO/tools/$TOOL" ]; then
  echo "No such tool: $TOOL  (try: $0 --list)" >&2
  exit 1
fi

if [ ! -f "$COSMOS_ROOT/lib/cosmos/ext/qt6.bundle" ]; then
  echo "Qt 6 extension not built. Build it with:" >&2
  echo "  cd $COSMOS_ROOT/ext/cosmos/ext/qt6 && ruby extconf.rb && make \\" >&2
  echo "    && rm -f $COSMOS_ROOT/lib/cosmos/ext/qt6.bundle \\" >&2
  echo "    && cp qt6.bundle $COSMOS_ROOT/lib/cosmos/ext/ \\" >&2
  echo "    && codesign -f -s - $COSMOS_ROOT/lib/cosmos/ext/qt6.bundle" >&2
  echo "  (rm+codesign matter: overwriting a mapped .bundle in place invalidates" >&2
  echo "   its signature and macOS then SIGKILLs ruby at load with no message.)" >&2
  exit 1
fi

export CI=1
export BUNDLE_GEMFILE="$COSMOS_ROOT/Gemfile"
export RUBYLIB="$COSMOS_ROOT/lib${RUBYLIB:+:$RUBYLIB}"

cd "$DEMO"
exec bundle exec ruby "tools/$TOOL"
