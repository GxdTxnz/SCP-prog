#!/bin/bash
# cборка бинаря scpanel (glibc 2.17+)
set -euo pipefail

NC_VER=6.5
NC_SHA256=136d91bc269a9a5785e5f9e980bc76ab57428f604ce3e5a5a90cebc767971cc6
MAX_GLIBC=2.17

SRC=/src/scpanel.cpp
OUT=/out/scpanel
WORK=$(mktemp -d)
NC=$WORK/ncurses

cd "$WORK"
curl -fsSL -o ncurses.tar.gz "https://ftp.gnu.org/gnu/ncurses/ncurses-$NC_VER.tar.gz"
echo "$NC_SHA256  ncurses.tar.gz" | sha256sum -c --quiet
tar xzf ncurses.tar.gz
cd "ncurses-$NC_VER"

# Файлы terminfo (описания терминалов)
# Debian/Ubuntu/Astra - /lib/terminfo + /usr/share/terminfo
# RHEL/Fedora/ALT - /usr/share/terminfo
./configure --quiet \
  --prefix="$NC" \
  --enable-widec \
  --without-shared --with-normal \
  --without-debug --without-cxx --without-cxx-binding --without-ada \
  --without-progs --without-tests --without-manpages --without-gpm \
  --disable-db-install --disable-termcap \
  --with-terminfo-dirs=/etc/terminfo:/lib/terminfo:/usr/share/terminfo:/usr/lib/terminfo \
  --with-default-terminfo-dir=/usr/share/terminfo
make -s -j"$(nproc)" >/dev/null
make -s install >/dev/null

g++ -std=c++17 -O2 -Wall -Wextra -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=600 \
  -I"$NC/include/ncursesw" -I"$NC/include" \
  -o "$OUT" "$SRC" "$NC/lib/libncursesw.a" \
  -static-libstdc++ -static-libgcc
strip "$OUT"

need=$(objdump -T "$OUT" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1)
echo "Нужна glibc: $need (максимум можно $MAX_GLIBC)"

if [ "$(printf '%s\n%s\n' "$need" "$MAX_GLIBC" | sort -V | tail -1)" != "$MAX_GLIBC" ]; then
  echo "Ошибка: бинарь требует glibc $need, на старых системах не запустится" >&2
  exit 1
fi

echo "Зависимости:"
readelf -d "$OUT" | grep NEEDED
rm -rf "$WORK"
