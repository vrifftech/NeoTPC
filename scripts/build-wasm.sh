#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NEOSHARED_ROOT_VALUE="${NEOSHARED_ROOT:-$ROOT_DIR/../neoshared}"
DEPS_ROOT="${NEO_WASM_DEPS_ROOT:-$ROOT_DIR/../.neo-wasm-deps}"
VCPKG_ROOT_VALUE="${VCPKG_ROOT:-}"
VCPKG_ROOT_EXPLICIT=0
[[ -z "${VCPKG_ROOT:-}" ]] || VCPKG_ROOT_EXPLICIT=1
VCPKG_INSTALLED_DIR=""
forward=()

usage() {
  cat <<'USAGE'
usage: build-wasm.sh [--neoshared-root DIR] [--deps-root DIR] [--vcpkg-root DIR] [--vcpkg-installed-dir DIR] [shared build options]

Build NeoTPC for WebAssembly. By default, the script checks out and bootstraps
vcpkg at the exact builtin-baseline pinned in vcpkg.json under
<deps-root>/vcpkg. A --vcpkg-root checkout must already be at that baseline.
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage; exit 0;;
    --neoshared-root) NEOSHARED_ROOT_VALUE="$2"; shift 2;;
    --deps-root) DEPS_ROOT="$2"; shift 2;;
    --vcpkg-root) VCPKG_ROOT_VALUE="$2"; VCPKG_ROOT_EXPLICIT=1; shift 2;;
    --vcpkg-installed-dir) VCPKG_INSTALLED_DIR="$2"; shift 2;;
    *) forward+=("$1"); shift;;
  esac
done

case "$NEOSHARED_ROOT_VALUE" in /*) ;; *) NEOSHARED_ROOT_VALUE="$ROOT_DIR/$NEOSHARED_ROOT_VALUE";; esac
case "$DEPS_ROOT" in /*) ;; *) DEPS_ROOT="$ROOT_DIR/$DEPS_ROOT";; esac
mkdir -p "$DEPS_ROOT"
DEPS_ROOT="$(cd "$DEPS_ROOT" && pwd)"

[[ -n "$VCPKG_ROOT_VALUE" ]] || VCPKG_ROOT_VALUE="$DEPS_ROOT/vcpkg"
case "$VCPKG_ROOT_VALUE" in /*) ;; *) VCPKG_ROOT_VALUE="$ROOT_DIR/$VCPKG_ROOT_VALUE";; esac
[[ -n "$VCPKG_INSTALLED_DIR" ]] || VCPKG_INSTALLED_DIR="$DEPS_ROOT/vcpkg-installed"
case "$VCPKG_INSTALLED_DIR" in /*) ;; *) VCPKG_INSTALLED_DIR="$ROOT_DIR/$VCPKG_INSTALLED_DIR";; esac

[[ -f "$NEOSHARED_ROOT_VALUE/scripts/build-wasm-app.sh" ]] || {
  echo "neoshared browser-build helper was not found: $NEOSHARED_ROOT_VALUE" >&2
  exit 2
}

for command_name in git python3; do
  command -v "$command_name" >/dev/null 2>&1 || {
    echo "Required command is unavailable: $command_name" >&2
    exit 2
  }
done

VCPKG_BASELINE="$(python3 - "$ROOT_DIR/vcpkg.json" <<'PY'
import json
import sys
from pathlib import Path
path = Path(sys.argv[1])
try:
    value = json.loads(path.read_text(encoding="utf-8"))["builtin-baseline"]
except (OSError, KeyError, json.JSONDecodeError) as exc:
    raise SystemExit(f"Unable to read builtin-baseline from {path}: {exc}")
if not isinstance(value, str) or len(value) != 40 or any(ch not in "0123456789abcdefABCDEF" for ch in value):
    raise SystemExit(f"Invalid builtin-baseline in {path}: {value!r}")
print(value.lower())
PY
)"

prepare_managed_vcpkg() {
  local destination="$1"
  local baseline="$2"
  local parent
  parent="$(dirname "$destination")"
  mkdir -p "$parent"

  if [[ ! -d "$destination/.git" ]]; then
    if [[ -e "$destination" ]]; then
      echo "Managed vcpkg path exists but is not a Git checkout: $destination" >&2
      exit 2
    fi
    mkdir -p "$destination"
    git -C "$destination" init -q
    git -C "$destination" remote add origin https://github.com/microsoft/vcpkg.git
  fi

  local current=""
  current="$(git -C "$destination" rev-parse HEAD 2>/dev/null || true)"
  if [[ "$current" != "$baseline" ]]; then
    echo "Fetching vcpkg baseline $baseline"
    git -C "$destination" fetch --depth 1 origin "$baseline"
    git -C "$destination" checkout --detach --force "$baseline"
  fi
}

if [[ "$VCPKG_ROOT_EXPLICIT" == 0 ]]; then
  prepare_managed_vcpkg "$VCPKG_ROOT_VALUE" "$VCPKG_BASELINE"
else
  [[ -d "$VCPKG_ROOT_VALUE/.git" ]] || {
    echo "--vcpkg-root must name a Git checkout so its pinned revision can be verified: $VCPKG_ROOT_VALUE" >&2
    exit 2
  }
  VCPKG_HEAD="$(git -C "$VCPKG_ROOT_VALUE" rev-parse HEAD 2>/dev/null || true)"
  [[ "$VCPKG_HEAD" == "$VCPKG_BASELINE" ]] || {
    echo "vcpkg checkout does not match NeoTPC's pinned baseline." >&2
    echo "  expected: $VCPKG_BASELINE" >&2
    echo "  actual:   ${VCPKG_HEAD:-unavailable}" >&2
    exit 2
  }
fi

[[ -f "$VCPKG_ROOT_VALUE/scripts/buildsystems/vcpkg.cmake" ]] || {
  echo "vcpkg toolchain file was not found after checkout: $VCPKG_ROOT_VALUE" >&2
  exit 2
}
if [[ ! -x "$VCPKG_ROOT_VALUE/vcpkg" ]]; then
  echo "Bootstrapping vcpkg at $VCPKG_BASELINE"
  bash "$VCPKG_ROOT_VALUE/bootstrap-vcpkg.sh" -disableMetrics
fi
[[ -x "$VCPKG_ROOT_VALUE/vcpkg" ]] || {
  echo "vcpkg bootstrap did not produce an executable: $VCPKG_ROOT_VALUE/vcpkg" >&2
  exit 2
}

mkdir -p "$VCPKG_INSTALLED_DIR"

bash "$NEOSHARED_ROOT_VALUE/scripts/build-wasm-app.sh" \
  --source-root "$ROOT_DIR" \
  --neoshared-root "$NEOSHARED_ROOT_VALUE" \
  --deps-root "$DEPS_ROOT" \
  --app-target "NeoTPC" \
  --app-name "NeoTPC" \
  --slug "neotpc" \
  --option-prefix "NEOTPC" \
  --cli-option "NEOTPC_BUILD_CLI" \
  --icon "resources/tpc.svg" \
  --vcpkg-root "$VCPKG_ROOT_VALUE" \
  --vcpkg-triplet "wasm32-emscripten" \
  --vcpkg-installed-dir "$VCPKG_INSTALLED_DIR" \
  "${forward[@]+"${forward[@]}"}"
