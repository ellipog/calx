#!/bin/sh
# Vercel's ignored build step (vercel.json's ignoreCommand), run in site/: exits 0 to skip the
# build when nothing the site is made from has changed since the branch's last deployment, and 1
# to build. The site is made from this folder, docs/, the README's numbers, assets/logo/'s art,
# tools/install/'s scripts, which it serves, and tools/vscode/'s grammar, which colors its code.
# When there's nothing to compare with, as on a branch's first push, or when the last deployment's
# commit is older than Vercel's shallow clone holds, it builds.
prev="$VERCEL_GIT_PREVIOUS_SHA"
if [ -z "$prev" ] || ! git cat-file -e "$prev^{commit}" 2> /dev/null; then
    echo "No earlier deployment to compare with: building."
    exit 1
fi
if git diff --quiet "$prev" HEAD -- . ../docs ../README.md ../assets/logo ../tools/install ../tools/vscode/syntaxes; then
    echo "Nothing the site is made from has changed since $prev: skipping the build."
    exit 0
fi
echo "The site's files have changed since $prev: building."
exit 1
