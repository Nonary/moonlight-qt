BUILD_CONFIG="release"
BUILD_JOBS=${BUILD_JOBS:-$(python3 scripts/linux-build-budget.py --jobs)}
[[ "$BUILD_JOBS" =~ ^[1-9][0-9]*$ ]] || { echo 'Invalid BUILD_JOBS' >&2; exit 1; }
export MAKEFLAGS="-j$BUILD_JOBS"

fail()
{
	echo "$1" 1>&2
	exit 1
}

BUILD_ROOT=$PWD/build
SOURCE_ROOT=$PWD
BUILD_FOLDER=$BUILD_ROOT/build-$BUILD_CONFIG
DEPLOY_FOLDER=$BUILD_ROOT/deploy-$BUILD_CONFIG
INSTALLER_FOLDER=$BUILD_ROOT/installer-$BUILD_CONFIG

LINUXDEPLOY=linuxdeploy-$(uname -m).AppImage

if [ -n "$CI_VERSION" ]; then
  VERSION=$CI_VERSION
else
  VERSION=`cat $SOURCE_ROOT/app/version.txt`
fi

command -v qmake6 >/dev/null 2>&1 || fail "Unable to find 'qmake6' in your PATH!"
command -v $LINUXDEPLOY >/dev/null 2>&1 || fail "Unable to find '$LINUXDEPLOY' in your PATH!"

echo Cleaning output directories
rm -rf $BUILD_FOLDER
rm -rf $DEPLOY_FOLDER
rm -rf $INSTALLER_FOLDER
mkdir $BUILD_ROOT
mkdir $BUILD_FOLDER
mkdir $DEPLOY_FOLDER
mkdir $INSTALLER_FOLDER

# Enable LTO for official builds
# Each make worker may link a separate target; keep LTO from spawning another
# full CPU-sized pool inside every link. LTO remains enabled.
export CFLAGS="-flto=1"
export CXXFLAGS="-flto=1"
export LDFLAGS="-flto=1"

echo Configuring the project
pushd $BUILD_FOLDER
# Native Wayland feedback requires Moonlight, SDL and matching Qt Wayland plugins.
# Use host libwayland with host Mesa/EGL: an older bundled libwayland can hide
# symbols required by the host Mesa implementation, breaking EGL even on X11.
# We disable DRM support because linuxdeploy doesn't bundle the appropriate libraries for Qt EGLFS.
compiler_args=()
if [ -n "${CC:-}" ]; then compiler_args+=("QMAKE_CC=$CC"); fi
if [ -n "${CXX:-}" ]; then compiler_args+=("QMAKE_CXX=$CXX"); fi
qmake6 $SOURCE_ROOT/moonlight-qt.pro CONFIG+=disable-libdrm PREFIX=$DEPLOY_FOLDER/usr DEFINES+=APP_IMAGE "${compiler_args[@]}" || fail "Qmake failed!"
popd

echo Compiling Moonlight in $BUILD_CONFIG configuration
pushd $BUILD_FOLDER
make -j"$BUILD_JOBS" $(echo "$BUILD_CONFIG" | tr '[:upper:]' '[:lower:]') || fail "Make failed!"
grep -q -- '-DHAS_WAYLAND' app/Makefile.Release || fail "Native Wayland feedback was not compiled"
popd

echo Deploying to staging directory
pushd $BUILD_FOLDER
make install || fail "Make install failed!"
popd

# appimagetool (bundled in linuxdeploy's appimage plugin) requires .DirIcon to
# be a PNG; since 2026-10-30 an SVG one fails or loudly warns. linuxdeploy
# always picks an SVG over raster icons for the AppDir root, so the AppImage
# ships only the 256x256 PNG (rendered from app/res/moonlight.svg). The
# scalable SVG stays in the regular make install for distribution packages.
rm -f "$DEPLOY_FOLDER/usr/share/icons/hicolor/scalable/apps/moonlight.svg"
install -Dm644 "$SOURCE_ROOT/app/deploy/linux/appimage/moonlight.png" \
  "$DEPLOY_FOLDER/usr/share/icons/hicolor/256x256/apps/moonlight.png" || fail "Icon install failed!"

export QML_SOURCES_PATHS=$SOURCE_ROOT/app/gui
export QMAKE=qmake6
export EXTRA_PLATFORM_PLUGINS='libqwayland-egl.so;libqwayland-generic.so'
export EXTRA_QT_MODULES='waylandclient;waylandcompositor'

echo Creating AppImage
pushd $INSTALLER_FOLDER
host_libraries=(--exclude-library='libwayland-*.so*' --exclude-library='libEGL*.so*' --exclude-library='libGLX_mesa.so*')
# Deploy Qt before creating the image: the Qt plugin may copy dependencies even
# when the main deployer excludes them. Remove only host ABI libraries in this
# known staging directory, then maintain exclusions during final AppDir scanning.
$LINUXDEPLOY --appdir $DEPLOY_FOLDER \
  --library=/usr/local/lib/libSDL3.so.0 \
  "${host_libraries[@]}" --plugin qt || fail "linuxdeploy Qt staging failed!"
find "$DEPLOY_FOLDER/usr/lib" -maxdepth 1 \
  \( -name 'libwayland-*.so*' -o -name 'libEGL*.so*' -o -name 'libGLX_mesa.so*' \) -delete
VERSION=$VERSION $LINUXDEPLOY --appdir $DEPLOY_FOLDER \
  "${host_libraries[@]}" --output appimage || fail "linuxdeploy AppImage creation failed!"
popd

echo Build successful
