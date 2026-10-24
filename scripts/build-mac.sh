#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
# Use the same compiler and SDK paths as ManagerApp/build.sh.
XCODE_DEVELOPER=/Applications/Xcode.app/Contents/Developer
if [ -z "${DEVELOPER_DIR:-}" ] && [ -x "$XCODE_DEVELOPER/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang++" ]; then
  BUILD_CXX="$XCODE_DEVELOPER/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang++"
  BUILD_SDK="$XCODE_DEVELOPER/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk"
else
  BUILD_CXX="$(xcrun -f clang++)"
  BUILD_SDK="$(xcrun --sdk macosx --show-sdk-path)"
fi
BUILD_DIR="${BUILD_DIR:-build}"
ASPL_REV=633e0f70203edd87d320fc5a3cae901e1363aac5
if [ ! -f external/libASPL/include/aspl/Plugin.hpp ]; then
  git clone https://github.com/gavv/libASPL.git external/libASPL
  git -C external/libASPL checkout "$ASPL_REV"
fi
if [ -d external/libASPL/.git ]; then
  ACTUAL_REV="$(git -C external/libASPL rev-parse HEAD)"
else
  ACTUAL_REV="$(cat external/libASPL/UPSTREAM-COMMIT)"
fi
if [ "$ACTUAL_REV" != "$ASPL_REV" ]; then
  echo "libASPL revision differs from pinned version $ASPL_REV" >&2; exit 1
fi
CMAKE_BIN="${CMAKE_BIN:-$PWD/.build-tools/bin/cmake}"
if [ ! -x "$CMAKE_BIN" ]; then CMAKE_BIN="$(command -v cmake)"; fi
"$CMAKE_BIN" -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER="$BUILD_CXX" \
  -DCMAKE_OBJCXX_COMPILER="$BUILD_CXX" \
  -DCMAKE_OSX_SYSROOT="$BUILD_SDK" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 '-DCMAKE_OSX_ARCHITECTURES=x86_64;arm64' \
  -DBUILD_MANAGER=ON -DBUILD_TESTS=ON -DBUILD_EXAMPLES=OFF
"$CMAKE_BIN" --build "$BUILD_DIR" -j 6
"$(dirname "$CMAKE_BIN")/ctest" --test-dir "$BUILD_DIR" --output-on-failure --timeout 45
codesign --force --sign - "$BUILD_DIR/AES67Driver.driver"
for arch in x86_64 arm64; do
  lipo "$BUILD_DIR/AES67Driver.driver/Contents/MacOS/AES67Driver" -verify_arch "$arch"
  lipo ManagerApp/AES67Manager.app/Contents/MacOS/AES67Manager -verify_arch "$arch"
done
printf '%s\n' 'Build and local tests complete. X32, Logic and macOS 11 hardware testing still required.'
