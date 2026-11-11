#!/usr/bin/env bash
# Re-extract every cached Qt dependency deb over the local prefix.
# Repairs files/symlinks that were truncated to zero bytes; intact files
# are simply overwritten with identical content.
#
# Two constraints drive the design:
#  1. The workspace fakeowner mount cannot let tar (dpkg-deb) replace an
#     existing regular file with a symlink. Every path that any deb
#     declares as a symlink must therefore be deleted BEFORE extraction.
#  2. qtbase5-dev-tools ships real binaries at usr/lib/qt5/bin, while
#     qtbase5-dev ships same-named symlinks under the multiarch path.
#     The tools deb is extracted to an isolated dir and its binaries are
#     copied into the real bin directory only.
set -euo pipefail

DEB_DIR=/workspace/.localdeb/debs
PREFIX=/workspace/.localdeb/prefix
TOOLS_DEB="$DEB_DIR/qtbase5-dev-tools_5.15.8+dfsg-11+deb12u3_arm64.deb"
DEV_DEB="$DEB_DIR/qtbase5-dev_5.15.8+dfsg-11+deb12u3_arm64.deb"

# Phase 1: delete every path that any deb declares as a symlink, whatever
# it currently is. On this fakeowner mount, extracting a symlink entry over
# an existing path (even an existing symlink) truncates it to a zero-byte
# regular file, so the path must be absent before extraction.
for deb in "$DEB_DIR"/*.deb; do
    dpkg-deb -c "$deb" | awk '/ -> / { print $6 }'
done | sort -u | while read -r entry; do
    rel=${entry#./}
    path="$PREFIX/$rel"
    if [ -e "$path" ] || [ -L "$path" ]; then
        rm -f "$path"
    fi
done

# Phase 2: extract every deb; the tools deb never touches the multiarch
# symlink directory.
count=0
for deb in "$DEB_DIR"/*.deb; do
    if [ "$deb" = "$TOOLS_DEB" ]; then
        tmp=$(mktemp -d)
        dpkg-deb -x "$deb" "$tmp"
        cp -a "$tmp/usr/lib/qt5/bin/." "$PREFIX/usr/lib/qt5/bin/"
        rm -rf "$tmp"
    else
        dpkg-deb -x "$deb" "$PREFIX"
    fi
    count=$((count + 1))
done
echo "re-extracted $count packages into $PREFIX"

# Phase 3: force-rebuild the multiarch qt5/bin symlinks exactly as
# qtbase5-dev declares (defensive; phase 1+2 should already create them).
while IFS=$'\t' read -r target linkname; do
    linkpath="$PREFIX/$target"
    rm -f "$linkpath"
    ln -s "$linkname" "$linkpath"
done < <(dpkg-deb -c "$DEV_DEB" \
    | awk '/\/usr\/lib\/aarch64-linux-gnu\/qt5\/bin\/.* -> / {
            target=$6; sub(/^\.\//, "", target);
            linkname=$8; gsub(/^[ \t]+/, "", linkname);
            print target "\t" linkname }')

"$PREFIX/usr/lib/aarch64-linux-gnu/qt5/bin/moc" --version
remaining=$(find "$PREFIX" -type f -size 0 \
    ! -path '*/python3.11/*' \
    ! -name '.placeholder' \
    ! -path '*/X11/locale/*' | wc -l)
echo "unexplained zero-byte files remaining: $remaining"
find "$PREFIX" -type f -size 0 \
    ! -path '*/python3.11/*' \
    ! -name '.placeholder' \
    ! -path '*/X11/locale/*' || true
