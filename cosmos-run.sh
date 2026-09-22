#!/usr/bin/env bash
# Run a command against the locally-built COSMOS 4.5.3 (arm64 macOS, system Ruby 2.6).
#
#   ./cosmos-run.sh ruby bin/cosmos demo ~/cosmos_demo
#   ./cosmos-run.sh rake build VERSION=4.5.3
#   cd ~/cosmos_demo && /path/to/cosmos-run.sh ruby my_script.rb
#
# CI=1 is required on every invocation: it makes the Gemfile skip the DART/Rails
# stack and makes cosmos.gemspec skip the qtbindings gem. GUI tools work via the
# locally built Qt 6 binding in ext/cosmos/ext/qt6 -- see ./cosmos-demo.sh.
set -euo pipefail
COSMOS_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export CI=1
export BUNDLE_GEMFILE="$COSMOS_ROOT/Gemfile"
export RUBYLIB="$COSMOS_ROOT/lib${RUBYLIB:+:$RUBYLIB}"
exec bundle exec "$@"
