#!/bin/bash
# проверка бинарника scpanel на разных ОС в docer'е

set -u

DOCKER=${DOCKER:-$(command -v docker >/dev/null 2>&1 && echo docker || echo podman)}
SEC="--security-opt label=disable"

fq() {
  case "${1%%/*}" in
    *.*) echo "$1" ;;
    "$1") echo "docker.io/library/$1" ;;
    *) echo "docker.io/$1" ;;
  esac
}

cd "$(dirname "$0")/.."
BIN=dist/scpanel
[ -x "$BIN" ] || { echo "Нет $BIN, сначала make portable" >&2; exit 1; }

IMAGES=("$@")
[ ${#IMAGES[@]} -gt 0 ] || IMAGES=(
  centos:7
  almalinux:8
  almalinux:9
  almalinux:10
  fedora:latest
  debian:10
  debian:13
  ubuntu:18.04
  ubuntu:24.04
  alt:p9
  alt:p10
  registry.astralinux.ru/library/astra/ubi17
)

fail=0

for img in "${IMAGES[@]}"; do
  if ! $DOCKER pull -q "$(fq "$img")" >/dev/null 2>&1; then
    printf '%-45s НЕ СКАЧАЛСЯ ОБРАЗ\n' "$img"
    fail=1
    continue
  fi

  glibc=$($DOCKER run --rm "$(fq "$img")" sh -c 'ldd --version 2>&1 | head -1 | grep -o "[0-9]*\.[0-9]*$"')
  log=$(mktemp)
  script -qfc "$DOCKER run --rm -it --init $SEC -e TERM=xterm-256color -e LANG=C.UTF-8 \
    -v '$PWD/$BIN:/scpanel:ro' '$(fq "$img")' timeout --foreground 2 /scpanel" "$log" \
    </dev/null >/dev/null 2>&1
  out=$(tail -n +2 "$log")
  rm -f "$log"

  if printf '%s' "$out" | grep -q 'user@host'; then
    printf '%-45s glibc %-5s OK\n' "$img" "$glibc"
  else
    printf '%-45s glibc %-5s ОШИБКА\n' "$img" "$glibc"
    printf '%s\n' "$out" | head -5 | sed 's/^/    /'
    fail=1
  fi
done

exit $fail
