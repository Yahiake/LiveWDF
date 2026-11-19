#!/bin/sh
# Prints the version after the highest vX.Y.Z tag in HEAD's history: `patch` for a nightly, `minor` for a release.
# Before the first tag it counts from the version in CMakeLists.txt.
set -e
last=$(git tag --merged HEAD --list 'v[0-9]*' --sort=-v:refname | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' | head -n 1)
[ -n "$last" ] || last=$(sed -n 's/^set(LIVEWDF_VERSION \([0-9.]*\) .*/\1/p' CMakeLists.txt)
IFS=. read -r major minor patch <<EOV
${last#v}
EOV
case $1 in
patch) echo "$major.$minor.$((patch + 1))" ;;
minor) echo "$major.$((minor + 1)).0" ;;
*) echo "usage: $0 patch|minor" >&2; exit 2 ;;
esac
