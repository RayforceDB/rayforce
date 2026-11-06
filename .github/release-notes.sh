#!/usr/bin/env bash
# Compatible interface: release-notes.sh X.Y.Z <artifact-sha> [source-sha] [--allow-direct-merge]
# Optional source pin also accepted as RELEASE_SOURCE_SHA. Requires full history,
# tags, and gh read access for historical release provenance (never moving dev).
set -euo pipefail
exec python3 "$(dirname "${BASH_SOURCE[0]}")/release_notes.py" "$@"
