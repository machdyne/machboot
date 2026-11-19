#!/bin/sh
# Publish a machboot release on GitHub from what `make release` gathered:
# a tag v<version> on the current commit, every file as an asset, the zip
# too, and docs/quickstart.md as the description. Needs the GitHub CLI
# (gh auth login once). Add --draft to look before publishing:
#
#   tools/gh-release.sh            publish
#   tools/gh-release.sh --draft    a draft, to check on GitHub first
set -e
VER=$(sed -n 's/.*MB_VERSION_STR[[:space:]]*"\(.*\)".*/\1/p' fw/mb.h)
DIR=build/release/machboot-$VER
[ -d "$DIR" ] || { echo "gh-release: $DIR is missing: run make release first"; exit 1; }
gh release create "v$VER" "$DIR"/* "build/release/machboot-$VER.zip" \
	--title "machboot $VER" --notes-file docs/quickstart.md "$@"
