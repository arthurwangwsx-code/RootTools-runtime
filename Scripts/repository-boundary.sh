#!/usr/bin/env bash

ROOTTOOLS_CANONICAL_REPO="arthurwangwsx-code/RootTools-runtime"

roottools_require_canonical_release_checkout() {
  if [[ "$#" -ne 2 ]]; then
    echo "usage: roottools_require_canonical_release_checkout <checkout> <release-repository>" >&2
    return 64
  fi

  local checkout="$1"
  local release_repo="$2"
  local origin_url
  local branch
  local upstream

  if [[ "$release_repo" != "$ROOTTOOLS_CANONICAL_REPO" ]]; then
    echo "official releases must target $ROOTTOOLS_CANONICAL_REPO, got: $release_repo" >&2
    return 65
  fi

  origin_url="$(git -C "$checkout" remote get-url origin 2>/dev/null || true)"
  case "$origin_url" in
    "https://github.com/$ROOTTOOLS_CANONICAL_REPO"|"https://github.com/$ROOTTOOLS_CANONICAL_REPO.git"|"git@github.com:$ROOTTOOLS_CANONICAL_REPO.git"|"ssh://git@github.com/$ROOTTOOLS_CANONICAL_REPO.git") ;;
    *)
      echo "origin must be the canonical RootTools Runtime repository, got: ${origin_url:-<missing>}" >&2
      return 65
      ;;
  esac

  branch="$(git -C "$checkout" branch --show-current)"
  if [[ "$branch" != "main" ]]; then
    echo "releases must be created from main, got: ${branch:-<detached>}" >&2
    return 65
  fi

  upstream="$(git -C "$checkout" rev-parse --abbrev-ref --symbolic-full-name '@{upstream}' 2>/dev/null || true)"
  if [[ "$upstream" != "origin/main" ]]; then
    echo "main must track origin/main before release, got: ${upstream:-<missing>}" >&2
    return 65
  fi
}
