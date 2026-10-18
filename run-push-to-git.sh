#!/bin/sh
#
# Copyright © 2023 Chee Bin HOH. All rights reserved.
#
# It combines the following steps into one shell script:
# - remove padding space and detect files with tab at start of lines
# - make all to catch build error
# - make clean to remove binary object and executable files
# - clang-format files to be committed (in directory and file extension
#   configured via DIRS_SOURCES file)
# - git add changes
# - git commit changes
# - git push changes

set -eu

buildTest=""
commitMessage=""

while [ "$#" -gt 0 ]; do
  case "$1" in
    -t)
      buildTest="yes"
      shift
      ;;

    -m)
      if [ "$#" -lt 2 ]; then
        echo "Usage: $0 [-t] [-m commit-message]" >&2

        exit 2
      fi

      commitMessage="$2"
      shift 2
      ;;

    *)
      echo "Usage: $0 [-t] [-m commit-message]" >&2

      exit 2
      ;;
  esac
done

rootdir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$rootdir"

. "$rootdir/scripts/run-tab-check-and-clang-format.sh"

# test build, we do not want to check in things that break
if [ "${buildTest}" = "yes" ]; then
  echo "******** run build..."
  if [ ! -d ./Build ]; then
    mkdir ./Build
  fi

  cd ./Build
  cmake ../

  make

  echo "******** run ctest..."
  ctest -L dmn

  # clean up build, we do not want to check in binary
  echo "******** clean up build..."
  make clean

  cd ../
fi

# git activities
echo "******** git add, commit and push..."
echo

git add .
git commit -m "${commitMessage:-"no comment"}"

branch=$(git branch --show-current)
if [ -z "$branch" ]; then
  echo "Error: cannot push from a detached HEAD." >&2

  exit 1
fi

git push --force-with-lease origin "$branch"
