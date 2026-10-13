# This script requires create-dmg to be installed from https://github.com/sindresorhus/create-dmg
BUILD_CONFIG=$1

fail()
{
	echo "$1" 1>&2
	exit 1
}

if [ "$BUILD_CONFIG" != "Debug" ] && [ "$BUILD_CONFIG" != "Release" ]; then
  fail "Invalid build configuration - expected 'Debug' or 'Release'"
fi

BUILD_ROOT=$PWD/build
SOURCE_ROOT=$PWD
BUILD_FOLDER=$BUILD_ROOT/build-$BUILD_CONFIG
INSTALLER_FOLDER=$BUILD_ROOT/installer-$BUILD_CONFIG

if [ -n "$CI_VERSION" ]; then
  VERSION=$CI_VERSION
else
  VERSION=`cat $SOURCE_ROOT/app/version.txt`
fi

if [ -z "$(command -v qmake)" ]; then
  if [ -x "$SOURCE_ROOT/build/vrr-hybrid/qt/6.11.1/macos/bin/qmake" ]; then
    export PATH="$SOURCE_ROOT/build/vrr-hybrid/qt/6.11.1/macos/bin:$PATH"
  elif [ -d "/opt/homebrew/opt/qt/bin" ]; then
    export PATH="/opt/homebrew/opt/qt/bin:$PATH"
  fi
fi

if [ "$SIGNING_PROVIDER_SHORTNAME" == "" ]; then
  SIGNING_PROVIDER_SHORTNAME=$SIGNING_IDENTITY
fi
if [ "$SIGNING_IDENTITY" == "" ]; then
  SIGNING_IDENTITY=$SIGNING_PROVIDER_SHORTNAME
fi

[ "$SIGNING_IDENTITY" == "" ] || git diff-index --quiet HEAD -- || fail "Signed release builds must not have unstaged changes!"

echo Updating dependencies
python3 $SOURCE_ROOT/setup-deps.py

echo Cleaning output directories
rm -rf $BUILD_FOLDER
rm -rf $INSTALLER_FOLDER
mkdir -p $BUILD_ROOT
mkdir -p $BUILD_FOLDER
mkdir -p $INSTALLER_FOLDER

# Enable LTO for official builds
export CFLAGS=-flto=thin
export CXXFLAGS=-flto=thin
export LDFLAGS=-flto=thin

echo Configuring the project
pushd $BUILD_FOLDER
qmake $SOURCE_ROOT/moonlight-qt.pro QMAKE_APPLE_DEVICE_ARCHS="x86_64 arm64" || fail "Qmake failed!"
popd

echo Compiling Moonlight in $BUILD_CONFIG configuration
pushd $BUILD_FOLDER
make -j$(sysctl -n hw.logicalcpu) $(echo "$BUILD_CONFIG" | tr '[:upper:]' '[:lower:]') || fail "Make failed!"
popd

echo Saving dSYM file
pushd $BUILD_FOLDER
dsymutil app/Moonlight.app/Contents/MacOS/Moonlight -o Moonlight-$VERSION.dsym || fail "dSYM creation failed!"
cp -R Moonlight-$VERSION.dsym $INSTALLER_FOLDER || fail "dSYM copy failed!"
popd

echo Creating app bundle
EXTRA_ARGS=
if [ "$BUILD_CONFIG" == "Debug" ]; then EXTRA_ARGS="$EXTRA_ARGS -use-debug-libs"; fi
echo Extra deployment arguments: $EXTRA_ARGS
macdeployqt $BUILD_FOLDER/app/Moonlight.app $EXTRA_ARGS -qmldir=$SOURCE_ROOT/app/gui -appstore-compliant || fail "macdeployqt failed!"

# Apple bundle versions remain numeric; nightly identity and source are explicit.
if [[ "$VERSION" =~ ^6\.[12]\.0-daily\.([1-9][0-9]*)$ ]]; then
  NIGHTLY_SEQUENCE="${BASH_REMATCH[1]}"
  NIGHTLY_SOURCE=$(git rev-parse HEAD) || fail "Could not resolve nightly source"
  PLIST="$BUILD_FOLDER/app/Moonlight.app/Contents/Info.plist"
  /usr/libexec/PlistBuddy -c "Set :CFBundleVersion 6.2.$NIGHTLY_SEQUENCE" "$PLIST" || fail "Could not set nightly bundle version"
  /usr/libexec/PlistBuddy -c "Add :MoonlightNightlyVersion string $VERSION" "$PLIST" || fail "Could not set nightly identity"
  /usr/libexec/PlistBuddy -c "Add :MoonlightSourceCommit string $NIGHTLY_SOURCE" "$PLIST" || fail "Could not set nightly source"
fi

echo Removing dSYM files from app bundle
find $BUILD_FOLDER/app/Moonlight.app/ -name '*.dSYM' | xargs rm -rf

if [ "$SIGNING_IDENTITY" != "" ]; then
  echo Signing app bundle
  codesign --force --deep --options runtime --timestamp --sign "$SIGNING_IDENTITY" $BUILD_FOLDER/app/Moonlight.app || fail "Signing failed!"
else
  # Seal the app and its non-Qt dependencies after all metadata and resource
  # edits. Deployment-time ad hoc signatures may be stale or incomplete. This
  # is not a Developer ID signature and does not make the nightly notarized.
  echo Ad-hoc signing completed app bundle
  codesign --force --deep --sign - "$BUILD_FOLDER/app/Moonlight.app" || fail "Ad-hoc signing failed!"
fi

codesign --verify --deep --strict "$BUILD_FOLDER/app/Moonlight.app" || fail "App bundle signature verification failed!"

echo Creating DMG
DMG_CREATED=0
if command -v create-dmg >/dev/null 2>&1; then
  # Test if create-dmg is sindresorhus/create-dmg (accepts <app> <destination>)
  if create-dmg --help 2>&1 | grep -q "<app>"; then
    if [ "$SIGNING_IDENTITY" != "" ]; then
      create-dmg "$BUILD_FOLDER/app/Moonlight.app" "$INSTALLER_FOLDER" --identity="$SIGNING_IDENTITY" --no-version-in-filename && DMG_CREATED=1
    else
      # Keep unsigned builds unverified even if this machine has a signing identity.
      create-dmg "$BUILD_FOLDER/app/Moonlight.app" "$INSTALLER_FOLDER" --no-version-in-filename --no-code-sign
      case $? in
        0|2) DMG_CREATED=1 ;;
        *) DMG_CREATED=0 ;;
      esac
    fi
  fi
fi

if [ "$DMG_CREATED" -eq 0 ]; then
  echo "Staging DMG contents with Applications symlink..."
  DMG_STAGING="$BUILD_FOLDER/dmg-staging"
  rm -rf "$DMG_STAGING" "$INSTALLER_FOLDER/Moonlight.dmg"
  mkdir -p "$DMG_STAGING"
  ditto "$BUILD_FOLDER/app/Moonlight.app" "$DMG_STAGING/Moonlight.app"
  ln -s /Applications "$DMG_STAGING/Applications"

  if command -v diskutil >/dev/null 2>&1 && diskutil help image create from >/dev/null 2>&1; then
    diskutil image create from --format UDZO --volname "Moonlight" "$DMG_STAGING" "$INSTALLER_FOLDER/Moonlight.dmg" || fail "diskutil image create failed!"
  else
    hdiutil create -volname "Moonlight" -srcfolder "$DMG_STAGING" -ov -format UDZO "$INSTALLER_FOLDER/Moonlight.dmg" || fail "hdiutil create failed!"
  fi
  rm -rf "$DMG_STAGING"
fi

if [ "$NOTARY_KEYCHAIN_PROFILE" != "" ]; then
  echo Uploading to App Notary service
  xcrun notarytool submit --keychain-profile "$NOTARY_KEYCHAIN_PROFILE" --wait $INSTALLER_FOLDER/Moonlight.dmg || fail "Notary submission failed"

  echo Stapling notary ticket to DMG
  xcrun stapler staple -v $INSTALLER_FOLDER/Moonlight.dmg || fail "Notary ticket stapling failed!"
fi

mv $INSTALLER_FOLDER/Moonlight.dmg $INSTALLER_FOLDER/Moonlight-$VERSION.dmg
echo Build successful
