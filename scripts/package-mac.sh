#!/bin/bash
# Package only: does not install or restart audio on the build machine.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
BUILD_DIR="${BUILD_DIR:-build}"
OUT="$ROOT/artifacts"
VERSION_STRING="$(tr -d '[:space:]' < "$ROOT/VERSION.txt")"
if [[ ! "$VERSION_STRING" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)-build\.([0-9]+)$ ]]; then
  echo "Invalid VERSION.txt value: $VERSION_STRING" >&2
  exit 1
fi
RELEASE_VERSION="${BASH_REMATCH[1]}.${BASH_REMATCH[2]}.${BASH_REMATCH[3]}"
APP_ICON="ManagerApp/AES67Manager.app/Contents/Resources/AppIcon.icns"
if [ ! -s "$APP_ICON" ] || [ "$(dd if="$APP_ICON" bs=1 count=4 2>/dev/null)" != "icns" ]; then
  echo "Missing or invalid app icon: $APP_ICON (build the manager first)" >&2
  exit 1
fi
mkdir -p "$OUT"
STAGE="$(mktemp -d "$OUT/package.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
PAYLOAD="$STAGE/payload"
APP_STAGING="$STAGE/AES67Manager.app"
mkdir -p "$PAYLOAD/Library/Audio/Plug-Ins/HAL" "$PAYLOAD/Applications" \
  "$PAYLOAD/Library/Application Support/AES67Driver/Documentation" "$STAGE/scripts" "$STAGE/resources"
for arch in x86_64 arm64; do
  lipo "$BUILD_DIR/AES67Driver.driver/Contents/MacOS/AES67Driver" -verify_arch "$arch"
  lipo ManagerApp/AES67Manager.app/Contents/MacOS/AES67Manager -verify_arch "$arch"
  lipo "$BUILD_DIR/Tools/AES67TestReceiver" -verify_arch "$arch"
  lipo "$BUILD_DIR/Tools/AES67Discover" -verify_arch "$arch"
  lipo "$BUILD_DIR/Tools/AES67ClockService" -verify_arch "$arch"
done
codesign --force --sign - "$BUILD_DIR/AES67Driver.driver"
codesign --verify --strict "$BUILD_DIR/AES67Driver.driver"
cp -RX ManagerApp/AES67Manager.app "$APP_STAGING"
cp -X "$BUILD_DIR/Tools/AES67Discover" "$APP_STAGING/Contents/Resources/AES67Discover"
codesign --force --sign - "$APP_STAGING/Contents/Resources/AES67Discover"
cp -X "$BUILD_DIR/Tools/AES67TestReceiver" "$APP_STAGING/Contents/Resources/AES67TestReceiver"
codesign --force --sign - "$APP_STAGING/Contents/Resources/AES67TestReceiver"
codesign --force --sign - --options runtime "$APP_STAGING"
codesign --verify --strict "$APP_STAGING"
cp -RX "$BUILD_DIR/AES67Driver.driver" "$PAYLOAD/Library/Audio/Plug-Ins/HAL/"
cp -RX "$APP_STAGING" "$PAYLOAD/Applications/"
mkdir -p "$PAYLOAD/Library/LaunchDaemons"
cp -X "$BUILD_DIR/Tools/AES67ClockService" "$PAYLOAD/Library/Application Support/AES67Driver/"
codesign --force --sign - "$PAYLOAD/Library/Application Support/AES67Driver/AES67ClockService"
cp -X Installer/org.communityaudio.aes67.clock.plist "$PAYLOAD/Library/LaunchDaemons/"
chmod 644 "$PAYLOAD/Library/LaunchDaemons/org.communityaudio.aes67.clock.plist"
cp -X LICENSE "$PAYLOAD/Library/Application Support/AES67Driver/Documentation/LICENSE-GPL3.txt"
cp -X external/libASPL/LICENSE* "$PAYLOAD/Library/Application Support/AES67Driver/Documentation/"
cp -X Docs/Getting-Started.md Docs/Testing.md Docs/Troubleshooting.md Docs/UPSTREAM-NOTICE.md \
  "$PAYLOAD/Library/Application Support/AES67Driver/Documentation/"
cp -X scripts/uninstall-mac.command "$OUT/Deinstallieren.command"
cp -X Docs/Getting-Started.md "$OUT/GETTING-STARTED.md"
cp -X Docs/Testing.md "$OUT/TESTING.md"
cp -X Docs/Troubleshooting.md "$OUT/TROUBLESHOOTING.md"
cp -X Installer/scripts/preinstall Installer/scripts/postinstall "$STAGE/scripts/"
chmod 755 "$STAGE/scripts/"* "$OUT/Deinstallieren.command"
python3 scripts/check-package-quarantine.py --clean-staging "$PAYLOAD" "$STAGE/scripts"
/usr/bin/plutil -lint "$PAYLOAD/Library/LaunchDaemons/org.communityaudio.aes67.clock.plist"
codesign --verify --strict --deep "$PAYLOAD/Applications/AES67Manager.app"
codesign --verify --strict "$PAYLOAD/Library/Audio/Plug-Ins/HAL/AES67Driver.driver"
codesign --verify --strict "$PAYLOAD/Library/Application Support/AES67Driver/AES67ClockService"
pkgbuild --root "$PAYLOAD" --component-plist Installer/components.plist --identifier org.communityaudio.aes67.x32 \
  --version "$RELEASE_VERSION" --scripts "$STAGE/scripts" --install-location / "$STAGE/component.pkg"
sed "s/@VERSION@/$RELEASE_VERSION/g" Installer/Distribution.xml > "$STAGE/Distribution.xml"
cp -X LICENSE "$STAGE/resources/license.txt"
cp -X Installer/welcome.html Installer/readme.html Installer/conclusion.html "$STAGE/resources/"
productbuild --distribution "$STAGE/Distribution.xml" --package-path "$STAGE" \
  --resources "$STAGE/resources" \
  "$OUT/AES67-Mac-$RELEASE_VERSION-universal.pkg"
# Inspect the final archive too: checking the pre-pkgbuild tree alone is insufficient.
pkgutil --expand-full "$OUT/AES67-Mac-$RELEASE_VERSION-universal.pkg" "$STAGE/verified"
python3 scripts/check-package-quarantine.py "$STAGE/verified"
# Build a privacy-clean source archive from an explicit allowlist. Keep all test
# sources, but exclude local plans/logs, package docs copied to artifacts, old
# private project notes, and all local build/dependency metadata.
if [ -d external/libASPL/.git ]; then
  git -C external/libASPL rev-parse HEAD > external/libASPL/UPSTREAM-COMMIT
fi
test -s external/libASPL/UPSTREAM-COMMIT
COPYFILE_DISABLE=1 tar --uid 0 --gid 0 --uname root --gname wheel -czf "$OUT/AES67-Mac-$RELEASE_VERSION-source.tar.gz" \
  --exclude='.git' --exclude='*/.git' --exclude='*/.git/**' \
  --exclude='.DS_Store' --exclude='*/.DS_Store' --exclude='._*' --exclude='*/._*' \
  --exclude='*.log' --exclude='*/logs/*' --exclude='build' --exclude='build-*' \
  --exclude='artifacts' --exclude='.build-tools' --exclude='graphify-out' \
  --exclude='*.app' --exclude='*.driver' --exclude='*.pkg' --exclude='*.o' \
  --exclude='*.a' --exclude='*.dSYM' --exclude='compile_commands.json' \
  --exclude='.build' --exclude='output' --exclude='payload' \
  --exclude='Makefile' --exclude='CTestTestfile.cmake' --exclude='CTestCostData.txt' \
  --exclude='Installer/README.md' --exclude='ManagerApp/.build' \
  README.md CONTRIBUTING.md SECURITY.md LICENSE VERSION.txt .gitignore CMakeLists.txt .github \
  Driver NetworkEngine Shared Tests Tools Examples ManagerApp scripts \
  Docs/Getting-Started.md Docs/Testing.md Docs/Troubleshooting.md Docs/UPSTREAM-NOTICE.md \
  Installer/Distribution.xml Installer/component.plist Installer/components.plist \
  Installer/conclusion.html Installer/readme.html Installer/welcome.html \
  Installer/org.communityaudio.aes67.clock.plist Installer/build-installer.sh \
  Installer/scripts Installer/uninstall.sh external/libASPL
(cd "$OUT" && shasum -a 256 "AES67-Mac-$RELEASE_VERSION-universal.pkg" "AES67-Mac-$RELEASE_VERSION-source.tar.gz" > SHA256SUMS.txt)
printf '%s\n' "$OUT/AES67-Mac-$RELEASE_VERSION-universal.pkg" \
  'Local experimental package, ad-hoc signed binaries, no Apple notarization. Configuration changes restart CoreAudio only.'
