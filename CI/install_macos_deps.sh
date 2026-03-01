#!/bin/bash
set -e

MAC_DEPS_ARCH=$1
MAC_QT6_ARCH=$2
MAC_ARCH=$3

if [ -z "$MAC_DEPS_ARCH" ] || [ -z "$MAC_QT6_ARCH" ] || [ -z "$MAC_ARCH" ]; then
    echo "Usage: $0 <mac_deps_arch> <mac_qt6_arch> <mac_arch>"
    echo "Example: $0 x86_64 universal Intel"
    exit 1
fi

OBS_VERSION="32.0.4"
DEPS_VERSION="2025-08-23"

echo "Installing macOS Dependencies for OBS Plugin (Arch: $MAC_ARCH, Deps: $MAC_DEPS_ARCH, Qt6: $MAC_QT6_ARCH)..."

# 1. macOS Dependencies (obs-deps)
echo "Downloading obs-deps..."
wget https://github.com/obsproject/obs-deps/releases/download/$DEPS_VERSION/macos-deps-$DEPS_VERSION-$MAC_DEPS_ARCH.tar.xz
mkdir -p /tmp/obs-deps
tar -xf macos-deps-$DEPS_VERSION-$MAC_DEPS_ARCH.tar.xz -C /tmp/obs-deps

# 2. macOS Qt6
echo "Downloading Qt6..."
wget https://github.com/obsproject/obs-deps/releases/download/$DEPS_VERSION/macos-deps-qt6-$DEPS_VERSION-$MAC_QT6_ARCH.tar.xz
mkdir -p /tmp/qt6
tar -xf macos-deps-qt6-$DEPS_VERSION-$MAC_QT6_ARCH.tar.xz -C /tmp/qt6

# 3. OBS Studio Headers & macOS App
echo "Cloning OBS Studio Source (v$OBS_VERSION)..."
git clone --depth 1 --branch $OBS_VERSION https://github.com/obsproject/obs-studio.git /tmp/obs-studio-src

echo "Downloading OBS Studio App..."
wget https://github.com/obsproject/obs-studio/releases/download/$OBS_VERSION/OBS-Studio-$OBS_VERSION-macOS-$MAC_ARCH.dmg
hdiutil attach OBS-Studio-$OBS_VERSION-macOS-$MAC_ARCH.dmg
cp -R /Volumes/OBS*/OBS.app /tmp/OBS.app
hdiutil detach /Volumes/OBS*

# Install compile tools
echo "Installing build tools..."
brew update
brew install cmake pkg-config

echo "macOS Dependencies installation completed successfully."
