#!/bin/bash
set -e

OBS_VERSION="32.0.4"

echo "Installing Linux Dependencies for OBS Plugin..."

sudo apt-get update
sudo apt-get install -y cmake pkg-config build-essential \
  libcurl4-openssl-dev libavcodec-dev libavdevice-dev libavfilter-dev \
  libavformat-dev libavutil-dev libswresample-dev libswscale-dev \
  libjansson-dev libx11-xcb-dev libgles2-mesa-dev libwayland-dev \
  libpulse-dev qt6-base-dev libqt6svg6-dev qt6-base-private-dev \
  extra-cmake-modules libxkbcommon-dev libsimde-dev libmbedtls-dev libvlc-dev libpci-dev uuid-dev libxss-dev uthash-dev libdrm-dev libvulkan-dev libasound2-dev

# Clone OBS
echo "Cloning OBS Studio Source (v$OBS_VERSION)..."
git clone --depth 1 --branch $OBS_VERSION --recurse-submodules --shallow-submodules https://github.com/obsproject/obs-studio.git /tmp/obs-studio-src

# Build OBS Studio core to get libraries for linking
echo "Building OBS Studio core..."
mkdir -p /tmp/obs-studio-build
cd /tmp/obs-studio-build

cmake -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_BROWSER=OFF \
  -DENABLE_PLUGINS=OFF \
  -DENABLE_UI=OFF \
  -DENABLE_SCRIPTING=OFF \
  -DQT_VERSION=6 \
  -DCMAKE_INSTALL_PREFIX="/tmp/obs-studio-installed" \
  /tmp/obs-studio-src

# Only build the frontend API and core to avoid UI dependency issues
cmake --build . --config Release --parallel 4 -t obs-frontend-api

# Manually copy the compiled libraries to the expected directory
mkdir -p /tmp/obs-studio-installed/lib
find . -name "libobs.so*" -exec cp -a {} /tmp/obs-studio-installed/lib/ \;
find . -name "libobs-frontend-api.so*" -exec cp -a {} /tmp/obs-studio-installed/lib/ \;

# Copy obsconfig.h to the source directory so the plugin can find it
find . -name "obsconfig.h" -exec cp -a {} /tmp/obs-studio-src/libobs/ \; || true

echo "OBS Installed contents:"
ls -la /tmp/obs-studio-installed/lib/

echo "Linux Dependencies installation completed successfully."
