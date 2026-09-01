#!/usr/bin/env bash

ROOTTOOLS_CANONICAL_SOURCE_REPO="arthurwangwsx-code/RootTools-runtime"
ROOTTOOLS_PRIVATE_RELEASE_REPO="arthurwangwsx-code/RootTools-runtime-releases"
# Compatibility alias for host tooling written before source and binary
# repositories were separated.
ROOTTOOLS_CANONICAL_REPO="$ROOTTOOLS_CANONICAL_SOURCE_REPO"

roottools_require_canonical_source_checkout() {
  if [[ "$#" -ne 1 ]]; then
    echo "usage: roottools_require_canonical_source_checkout <checkout>" >&2
    return 64
  fi

  local checkout="$1"
  local origin_url
  local branch
  local upstream

  origin_url="$(git -C "$checkout" remote get-url origin 2>/dev/null || true)"
  case "$origin_url" in
    "https://github.com/$ROOTTOOLS_CANONICAL_SOURCE_REPO"|"https://github.com/$ROOTTOOLS_CANONICAL_SOURCE_REPO.git"|"git@github.com:$ROOTTOOLS_CANONICAL_SOURCE_REPO.git"|"ssh://git@github.com/$ROOTTOOLS_CANONICAL_SOURCE_REPO.git") ;;
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

roottools_require_release_target() {
  if [[ "$#" -ne 3 ]]; then
    echo "usage: roottools_require_release_target <repository> <visibility> <draft:0|1>" >&2
    return 64
  fi
  local release_repo="$1"
  local visibility
  visibility="$(printf '%s' "$2" | tr '[:lower:]' '[:upper:]')"
  local draft="$3"
  [[ "$draft" == "0" || "$draft" == "1" ]] || { echo "draft flag must be 0 or 1" >&2; return 64; }

  if [[ "$release_repo" == "$ROOTTOOLS_CANONICAL_SOURCE_REPO" ]]; then
    if [[ "$visibility" != "PUBLIC" ]]; then
      echo "canonical source repository visibility changed unexpectedly: $visibility" >&2
      return 65
    fi
    if [[ "$draft" != "1" ]]; then
      echo "personalized binaries in the public source repository must remain draft-only" >&2
      return 65
    fi
    return 0
  fi

  if [[ "$release_repo" == "$ROOTTOOLS_PRIVATE_RELEASE_REPO" ]]; then
    if [[ "$visibility" != "PRIVATE" ]]; then
      echo "binary release repository must be private, got: $visibility" >&2
      return 65
    fi
    return 0
  fi

  echo "binary releases may target only $ROOTTOOLS_PRIVATE_RELEASE_REPO or a draft in $ROOTTOOLS_CANONICAL_SOURCE_REPO, got: $release_repo" >&2
  return 65
}

roottools_require_canonical_release_checkout() {
  if [[ "$#" -ne 2 ]]; then
    echo "usage: roottools_require_canonical_release_checkout <checkout> <release-repository>" >&2
    return 64
  fi
  roottools_require_canonical_source_checkout "$1" || return
  if [[ "$2" != "$ROOTTOOLS_CANONICAL_SOURCE_REPO" && "$2" != "$ROOTTOOLS_PRIVATE_RELEASE_REPO" ]]; then
    echo "unsupported RootTools release repository: $2" >&2
    return 65
  fi
}
