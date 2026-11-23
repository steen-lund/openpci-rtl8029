#!/bin/sh
# Sets the version and release date everywhere they appear.
#
#   scripts/bump-version.sh [<major.minor>] [<dd.mm.yyyy>]     (or: make bump VERSION=... DATE=...)
#
# Without a version the current one is kept (to refresh the dates); without a date, today.
# Updates the version numbers and $VER string in src/ne2000/rev.h, the guide's $VER line,
# the Version: field of the Aminet readme (src/dist/openpci-rtl8029.readme),
# the end year of the "... Steen Lund" copyright lines, and the version's heading in
# src/ne2000/release_notes, which it adds (with a TODO line to replace) if it is missing.
#
# Copyright (c) 2026 Steen Lund
# SPDX-License-Identifier: MIT (see licenses/MIT.txt)

set -e
cd "$(dirname "$0")/.."

REV=src/ne2000/rev.h
GUIDE=src/dist/openpci-rtl8029.guide
README=src/dist/openpci-rtl8029.readme
NOTES=src/ne2000/release_notes

version=$1
date=${2:-$(date +%d.%m.%Y)}

if [ -z "$version" ]; then
    major=$(sed -n 's/^#define DEV_VERSION[[:space:]]*\([0-9]*\).*/\1/p' "$REV")
    minor=$(sed -n 's/^#define DEV_REVISION[[:space:]]*\([0-9]*\).*/\1/p' "$REV")
    version="$major.$minor"
fi

case "$version" in
    *[!0-9.]* | .* | *. | *.*.* | "") echo "Version must look like 2.1, not '$version'" >&2; exit 1 ;;
    *.*) ;;
    *) echo "Version must look like 2.1, not '$version'" >&2; exit 1 ;;
esac
case "$date" in
    [0-3][0-9].[01][0-9].[12][0-9][0-9][0-9]) ;;
    *) echo "Date must look like 05.01.2027 (dd.mm.yyyy), not '$date'" >&2; exit 1 ;;
esac

major=${version%.*}
minor=${version#*.}
year=${date##*.}
export major minor version date year

# rev.h: version numbers, and version, date and copyright end year in the $VER string
perl -pi -e '
    s/^(#define DEV_VERSION\s+)\d+/${1}$ENV{major}/;
    s/^(#define DEV_REVISION\s+)\d+/${1}$ENV{minor}/;
    s/(\$VER: openpci-rtl8029\.device )\d+\.\d+ \([\d.]+\)/${1}$ENV{version} ($ENV{date})/;
' "$REV"

# The guide's own version string, and the Version: field of the Aminet readme
perl -pi -e 's/(\$VER: openpci-rtl8029\.guide )\d+\.\d+ \([\d.]+\)/${1}$ENV{version} ($ENV{date})/' "$GUIDE"
perl -pi -e 's/^(Version:\s+)\d+\.\d+\s*$/${1}$ENV{version}\n/' "$README"

# Copyright lines of this project (never Matay's): move the end year up to this year
for f in $(git ls-files); do
    case "$f" in licenses/matay-*) continue ;; esac
    grep -Iq 'Steen Lund' "$f" 2>/dev/null || continue
    perl -pi -e '
        s/\b(20\d\d)(-|\xe2\x80\x93)(20\d\d)( Steen Lund)/$3 < $ENV{year} ? "$1$2$ENV{year}$4" : "$1$2$3$4"/ge;
        s/(Copyright \([cC]\) )(20\d\d)( Steen Lund)/$2 < $ENV{year} ? "$1$2-$ENV{year}$3" : "$1$2$3"/ge;
    ' "$f"
done

# release_notes: date the version's heading, or add one
if grep -q "^$version (" "$NOTES"; then
    perl -pi -e 's/^\Q$ENV{version}\E \([\d.]+\)/$ENV{version} ($ENV{date})/' "$NOTES"
else
    # The file may not end in a newline
    [ -n "$(tail -c 1 "$NOTES")" ] && echo >> "$NOTES"
    printf '\n%s (%s)\n\n  TODO: describe the changes in this release.\n' "$version" "$date" >> "$NOTES"
    echo "Added a $version section to $NOTES: replace its TODO line before releasing."
fi

echo "Version $version, dated $date. Changed files:"
git status --short
