#!/usr/bin/env bash
set -eu
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec python3 "$project_dir/tools/record_input_queries.py" "$@"
