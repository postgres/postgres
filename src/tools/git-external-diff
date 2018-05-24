#!/bin/bash

# This script is used to produce git context diffs

# Supplied parameters:
# $1   $2       $3       $4       $5       $6       $7
# path old-file old-hash old-mode new-file new-hash new-mode
# 'path' is the git-tree-relative path of the file being diff'ed

=comment

This info is copied from the old wiki page on Working with git:

Context diffs with Git

Copy git-external-diff into libexec/git-core/ of your git installation
and configure git to use that wrapper with:

    git config [--global] diff.external git-external-diff

--global makes the configuration global for your user - otherwise it is
just configured for the current repository.

For every command which displays diffs in some way you can use the
parameter "--[no-]-ext-diff" to enable respectively disable using the
external diff command.

For the git diff command --ext-diff is enabled by default - for any
other command like git log -p or git format-patch it is not!

This method should work on all platforms supported by git.

If you do not want to configure the external wrapper permanently or you
want to overwrite it you can also invoke git like:

    export GIT_EXTERNAL_DIFF=git-external-diff
    git diff --[no-]ext-diff

Alternatively, configure a git alias in ~/.gitconfig or .git/config:

    [alias]
        cdiff = !GIT_EXTERNAL_DIFF=git-context-diff git diff
=cut

old_hash="$3"
new_hash=$(git hash-object "$5")

# no change?
[ "$old_hash" = "$new_hash" ] && exit 0

[ "$DIFF_OPTS" = "" ] && DIFF_OPTS='-pcd'

echo "diff --git a/$1 b/$1"
echo "new file mode $7"
echo "index ${old_hash:0:7}..${new_hash:0:7}"

diff --label a/"$1" --label b/"$1" $DIFF_OPTS "$2" "$5"

exit 0
