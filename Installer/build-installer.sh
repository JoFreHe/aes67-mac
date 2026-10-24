#!/bin/bash
set -euo pipefail
exec "$(dirname "$0")/../scripts/package-mac.sh" "$@"
