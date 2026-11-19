#!/bin/sh
# Sets $PY to a working Python 3 (python, python3 or the Windows launcher), or exits quietly when there is none.
for c in python python3 "py -3"; do
  if $c -c "import sys; sys.exit(sys.version_info < (3, 8))" >/dev/null 2>&1; then PY=$c; return 0 2>/dev/null || break; fi
done
[ -n "$PY" ] || { echo "versioning hooks: no Python 3 found, skipping" >&2; exit 0; }
