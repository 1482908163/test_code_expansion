#!/usr/bin/env bash
# Compatibility entry point; all algorithm ablations use one runner.
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/run_experiments.sh" "$@"
