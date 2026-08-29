#!/usr/bin/env bash

roottools_package_version() {
  if [[ "$#" -ne 1 ]]; then
    echo "usage: roottools_package_version <version-file>" >&2
    return 64
  fi

  local version_file="$1"
  local package_version
  package_version="$(tr -d '\r\n' < "$version_file")"
  if [[ ! "$package_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+-[0-9]+$ ]]; then
    echo "invalid RootTools package version in $version_file: $package_version" >&2
    return 65
  fi
  printf '%s' "$package_version"
}

roottools_daemon_version() {
  if [[ "$#" -ne 1 ]]; then
    echo "usage: roottools_daemon_version <package-version>" >&2
    return 64
  fi

  local package_version="$1"
  if [[ ! "$package_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+-[0-9]+$ ]]; then
    echo "invalid RootTools package version: $package_version" >&2
    return 65
  fi
  printf '%s' "${package_version%-*}"
}
