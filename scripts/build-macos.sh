#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NEOSHARED_ROOT_VALUE=""
VCPKG_ROOT_VALUE="${VCPKG_ROOT:-$ROOT_DIR/../vcpkg}"
VCPKG_TRIPLET_VALUE="${VCPKG_DEFAULT_TRIPLET:-}"
FORWARD=()
SHOW_HELP=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --neoshared-root) NEOSHARED_ROOT_VALUE="$2"; shift 2;;
    --vcpkg-root) VCPKG_ROOT_VALUE="$2"; shift 2;;
    --vcpkg-triplet) VCPKG_TRIPLET_VALUE="$2"; shift 2;;
    -h|--help) SHOW_HELP=1; FORWARD+=("$1"); shift;;
    *) FORWARD+=("$1"); shift;;
  esac
done

if [[ -z "$NEOSHARED_ROOT_VALUE" ]]; then
  NEOSHARED_ROOT_VALUE="$("${CMAKE:-cmake}" -P "$ROOT_DIR/cmake/NeoSharedSource.cmake")"
fi
case "$NEOSHARED_ROOT_VALUE" in /*|[A-Za-z]:/*) ;; *) NEOSHARED_ROOT_VALUE="$ROOT_DIR/$NEOSHARED_ROOT_VALUE";; esac
case "$VCPKG_ROOT_VALUE" in /*) ;; *) VCPKG_ROOT_VALUE="$ROOT_DIR/$VCPKG_ROOT_VALUE";; esac

[[ -f "$NEOSHARED_ROOT_VALUE/scripts/build-macos-app.sh" ]] || {
  echo "neoshared macOS helper was not found under: $NEOSHARED_ROOT_VALUE" >&2
  echo "The automatically selected NeoShared checkout is incomplete." >&2
  exit 2
}
if [[ "$SHOW_HELP" == 0 ]]; then
  [[ -f "$VCPKG_ROOT_VALUE/scripts/buildsystems/vcpkg.cmake" ]] || {
    echo "NeoTPC requires the pinned vcpkg checkout under: $VCPKG_ROOT_VALUE" >&2
    echo "Check out the vcpkg baseline pinned in vcpkg.json or pass --vcpkg-root." >&2
    exit 2
  }
fi

vcpkg_args=(--vcpkg-root "$VCPKG_ROOT_VALUE")
[[ -z "$VCPKG_TRIPLET_VALUE" ]] || vcpkg_args+=(--vcpkg-triplet "$VCPKG_TRIPLET_VALUE")

bash "$NEOSHARED_ROOT_VALUE/scripts/build-macos-app.sh" \
  --source-root "$ROOT_DIR" \
  --app-name "NeoTPC" \
  --neoshared-root "$NEOSHARED_ROOT_VALUE" \
  "${vcpkg_args[@]}" \
  "${FORWARD[@]+"${FORWARD[@]}"}"
