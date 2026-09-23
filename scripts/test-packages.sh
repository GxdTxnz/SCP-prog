#!/bin/bash
# проверка пакетов в менеджере пакетов на разных ОС в docker'е
set -u

TIMEOUT=${TIMEOUT:-300}
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
DEB=$(ls dist/scpanel_*.deb 2>/dev/null | head -1)
RPM=$(ls dist/scpanel-*.rpm 2>/dev/null | head -1)
[ -n "$DEB" ] && [ -n "$RPM" ] || { echo "Нет пакетов в dist/, сначала make packages" >&2; exit 1; }

IMAGES=("$@")
[ ${#IMAGES[@]} -gt 0 ] || IMAGES=(
  debian:10
  debian:13
  ubuntu:18.04
  ubuntu:24.04
  registry.astralinux.ru/library/astra/ubi17
  centos:7
  almalinux:8
  almalinux:9
  almalinux:10
  fedora:latest
  alt:p9
  alt:p10
)

PREP_DEBIAN10='sed -i -e "s|deb.debian.org|archive.debian.org|g" -e "s|security.debian.org|archive.debian.org|g" -e "/buster-updates/d" /etc/apt/sources.list'
PREP_CENTOS7='sed -i -e "s|^mirrorlist|#mirrorlist|" -e "s|^#baseurl=http://mirror.centos.org|baseurl=http://vault.centos.org|" /etc/yum.repos.d/CentOS-*.repo'

fail=0

for img in "${IMAGES[@]}"; do
  prep=true
  case "$img" in
    debian:10) prep=$PREP_DEBIAN10 ;;
    centos:7) prep=$PREP_CENTOS7 ;;
  esac

  case "$img" in
    alt:*)
      pkg=$RPM
      install='apt-get update -qq && apt-get install -y -qq /pkg/p.rpm'
      remove='apt-get remove -y -qq scpanel' ;;
    centos:7)
      pkg=$RPM
      install='yum install -y -q /pkg/p.rpm'
      remove='yum remove -y -q scpanel' ;;
    almalinux:* | fedora:* | rockylinux:*)
      pkg=$RPM
      install='dnf install -y -q /pkg/p.rpm'
      remove='dnf remove -y -q scpanel' ;;
    *)
      pkg=$DEB
      install='apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq /pkg/p.deb'
      remove='apt-get remove -y -qq scpanel' ;;
  esac

  ext=${pkg##*.}
  printf '%-45s %-4s проверяю...\n' "$img" "$ext"
  name=scpanel-test-$$
  out=$(timeout "$TIMEOUT" $DOCKER run --rm $SEC --name "$name" -v "$PWD/$pkg:/pkg/p.$ext:ro" "$(fq "$img")" sh -c "
    ($prep) >/dev/null 2>&1
    ($install) >/tmp/log 2>&1 || { cat /tmp/log; exit 1; }
    command -v ssh >/dev/null || { echo 'не поставился ssh'; exit 1; }
    scpanel a b c 2>&1 | grep -q Usage || { echo 'scpanel не запускается'; exit 1; }
    [ \"\$(readlink /usr/bin/scpanel)\" = /usr/libexec/scpanel/scpanel ] || { echo '/usr/bin/scpanel не ссылка на /usr/libexec/scpanel/scpanel'; ls -l /usr/bin/scpanel; exit 1; }
    ($remove) >/tmp/log 2>&1 || { cat /tmp/log; exit 1; }
    [ ! -e /usr/bin/scpanel ] && [ ! -L /usr/bin/scpanel ] || { echo 'после удаления осталась /usr/bin/scpanel'; exit 1; }
    [ ! -e /usr/libexec/scpanel ] || { echo 'после удаления осталась /usr/libexec/scpanel'; ls -la /usr/libexec/scpanel; exit 1; }
  " 2>&1)
  rc=$?
  $DOCKER rm -f "$name" >/dev/null 2>&1

  if [ $rc -eq 0 ]; then
    printf '%-45s %-4s OK         \n' "$img" "$ext"
  elif [ $rc -eq 124 ]; then
    printf '%-45s %-4s НЕ УСПЕЛ за %s с\n' "$img" "$ext" "$TIMEOUT"
    fail=1
  else
    printf '%-45s %-4s ОШИБКА     \n' "$img" "$ext"
    printf '%s\n' "$out" | tail -8 | sed 's/^/    /'
    fail=1
  fi
done

exit $fail
