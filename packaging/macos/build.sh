#!/bin/bash
# Build Pakon.app and Pakon.dmg (Apple silicon, macOS 13+) with PyInstaller.
#
#   packaging/macos/build.sh [VERSION]
#
# Signing: uses the first "Developer ID Application" identity in the keychain
# (or PAKON_SIGN_IDENTITY); without one it signs ad hoc, which runs only on
# this Mac. Notarization runs when PAKON_NOTARY_PROFILE names a profile made
# with `xcrun notarytool store-credentials`. See packaging/macos/README.md.
#
# Output: dist/macos/Pakon.app and dist/macos/Pakon.dmg
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
VERSION="${1:-0.1.0}"
CACHE="$HERE/.cache"
OUT="$REPO/dist/macos"
APP="$OUT/Pakon.app"
DEPLOY=13.0
export MACOSX_DEPLOYMENT_TARGET=$DEPLOY

# Python from python-build-standalone: built for old macOS, unlike Homebrew's,
# and the frozen app inherits its libpython.
PY_URL="https://github.com/astral-sh/python-build-standalone/releases/download/20260924/cpython-3.12.14%2B20260924-aarch64-apple-darwin-install_only.tar.gz"
PY_SHA=9763f43db2481a6af36af82ec40302aab7a73632f880129d07a6e81aec846277
USB_URL="https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.tar.bz2"
USB_SHA=fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf

step() { printf '\n==> %s\n' "$*"; }

fetch() {  # url sha256 dest
    if [ ! -f "$3" ] || ! echo "$2  $3" | shasum -a 256 -c --status; then
        curl -fL --retry 3 -o "$3.part" "$1"
        echo "$2  $3.part" | shasum -a 256 -c --status \
            || { echo "checksum mismatch: $1" >&2; rm -f "$3.part"; exit 1; }
        mv "$3.part" "$3"
    fi
}

machos() {  # every Mach-O file under $1, one per line
    find "$1" -type f -print0 | xargs -0 file -F '|' \
        | grep -v 'for architecture' | grep 'Mach-O' | cut -d'|' -f1
}

[ "$(uname -m)" = arm64 ] || { echo "build on Apple silicon" >&2; exit 1; }
[[ "$VERSION" =~ ^[0-9]+(\.[0-9]+){1,2}$ ]] \
    || { echo "version must look like 1.2 or 1.2.3, got '$VERSION'" >&2; exit 1; }
IDENTITY="${PAKON_SIGN_IDENTITY:-$(security find-identity -v -p codesigning \
    | sed -n 's/.*"\(Developer ID Application: .*\)"/\1/p' | head -1)}"
if [ -n "${PAKON_NOTARY_PROFILE:-}" ] && [ -z "$IDENTITY" ]; then
    echo "notarization needs a Developer ID Application certificate in the keychain" \
         "(see packaging/macos/README.md)" >&2
    exit 1
fi
mkdir -p "$CACHE" "$OUT"
rm -rf "$APP" "$OUT/Pakon.dmg"

# --- libusb, built for $DEPLOY (Homebrew's targets the running macOS) -------
step "libusb"
USB_PREFIX="$CACHE/libusb-$DEPLOY"
if [ ! -f "$USB_PREFIX/lib/libusb-1.0.0.dylib" ]; then
    fetch "$USB_URL" "$USB_SHA" "$CACHE/libusb.tar.bz2"
    rm -rf "$CACHE/libusb-src" && mkdir -p "$CACHE/libusb-src"
    tar -xjf "$CACHE/libusb.tar.bz2" -C "$CACHE/libusb-src" --strip-components 1
    (cd "$CACHE/libusb-src" \
        && ./configure --prefix="$USB_PREFIX" --disable-static --quiet \
        && make -s -j"$(sysctl -n hw.ncpu)" && make -s install)
fi

# --- C tools, linked against that libusb by rpath --------------------------
step "pakon_probe, pakon_replay"
BIN="$CACHE/bin"
rm -rf "$BIN" && mkdir -p "$BIN"
PKG_CONFIG_PATH="$USB_PREFIX/lib/pkgconfig" cmake -S "$REPO" -B "$CACHE/cbuild" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=$DEPLOY \
    -DCMAKE_OSX_ARCHITECTURES=arm64 >/dev/null
cmake --build "$CACHE/cbuild" --target pakon_probe pakon_replay -j >/dev/null
cp "$USB_PREFIX/lib/libusb-1.0.0.dylib" "$BIN/"
chmod u+w "$BIN/libusb-1.0.0.dylib"
install_name_tool -id @rpath/libusb-1.0.0.dylib "$BIN/libusb-1.0.0.dylib"
for t in pakon_probe pakon_replay; do
    cp "$CACHE/cbuild/$t" "$BIN/"
    install_name_tool -change "$USB_PREFIX/lib/libusb-1.0.0.dylib" \
        @rpath/libusb-1.0.0.dylib "$BIN/$t"
    install_name_tool -add_rpath @loader_path/.. "$BIN/$t"   # build/ -> bundle root
done

# --- build Python with wheels for $DEPLOY ----------------------------------
step "Python"
PYROOT="$CACHE/python"
if [ ! -x "$PYROOT/bin/python3" ]; then
    fetch "$PY_URL" "$PY_SHA" "$CACHE/python.tar.gz"
    tar -xzf "$CACHE/python.tar.gz" -C "$CACHE"            # -> .cache/python
fi
VENV="$CACHE/venv"
STAMP="$VENV/.requirements"
if ! cmp -s "$HERE/requirements.txt" "$STAMP" \
        || ! cmp -s "$REPO/web/requirements.txt" "$STAMP.web"; then
    rm -rf "$VENV"
    "$PYROOT/bin/python3" -m venv "$VENV"
    SITE="$("$VENV/bin/python" -c 'import sysconfig; print(sysconfig.get_path("purelib"))')"
    # pywebview's proxy_tools ships as source only; --platform needs wheels
    rm -rf "$CACHE/wheels"
    "$VENV/bin/python" -m pip wheel --quiet --disable-pip-version-check \
        --no-deps proxy_tools -w "$CACHE/wheels"
    # --platform picks wheels built for $DEPLOY, not for the macOS doing the build
    (cd "$HERE" && "$VENV/bin/python" -m pip install --quiet \
        --disable-pip-version-check --no-cache-dir --upgrade \
        --find-links "$CACHE/wheels" \
        --target "$SITE" --platform "macosx_${DEPLOY/./_}_arm64" \
        --python-version 3.12 --implementation cp --only-binary=:all: \
        -r requirements.txt)
    cp "$HERE/requirements.txt" "$STAMP"
    cp "$REPO/web/requirements.txt" "$STAMP.web"
fi
PY="$VENV/bin/python"

# --- icon ----------------------------------------------------------------------
rm -rf "$CACHE/AppIcon.iconset"
"$PY" -B "$HERE/make_icon.py" "$CACHE/AppIcon.iconset"
iconutil -c icns "$CACHE/AppIcon.iconset" -o "$CACHE/AppIcon.icns"

# --- freeze --------------------------------------------------------------------
step "PyInstaller"
PAKON_BIN_DIR="$BIN" PAKON_VERSION="$VERSION" PAKON_ICON="$CACHE/AppIcon.icns" \
    "$PY" -m PyInstaller --noconfirm --clean --log-level WARN \
    --distpath "$OUT" --workpath "$CACHE/pyinstaller" "$HERE/pakon.spec"
rm -rf "$OUT/Pakon"                                  # the bare COLLECT folder

# --- every binary must run on $DEPLOY ---------------------------------------
step "check minimum macOS"
bad=$(machos "$APP" | while read -r f; do
        m=$(otool -l "$f" | awk '/LC_BUILD_VERSION/{b=1} b&&/minos/{print $2; exit}')
        if [ -n "$m" ] && [ "$(printf '%s\n%s\n' "$m" "$DEPLOY" | sort -V | tail -1)" != "$DEPLOY" ]; then
            echo "$m ${f#"$APP"/}"
        fi
    done)
[ -z "$bad" ] || { echo "needs newer macOS than $DEPLOY:"; echo "$bad"; exit 1; }

# --- sign --------------------------------------------------------------------
step "sign"
if [ -n "$IDENTITY" ]; then
    SIGN=(codesign --force --options runtime --timestamp --sign "$IDENTITY")
    echo "identity: $IDENTITY"
else
    SIGN=(codesign --force --options runtime --sign -)
    echo "no Developer ID Application identity: signing ad hoc (runs on this Mac only)"
fi
# every Mach-O inside, then the app (its executable gets the entitlements).
# pakon_probe and pakon_replay are executables of their own: they need the
# entitlements too, or an ad hoc build cannot load the bundle's libusb.
machos "$APP" | grep -v "/Contents/MacOS/Pakon$" | while read -r f; do
    case "$f" in
        */build/pakon_probe|*/build/pakon_replay) ent=(--entitlements "$HERE/entitlements.plist") ;;
        *) ent=() ;;
    esac
    "${SIGN[@]}" ${ent[@]+"${ent[@]}"} "$f" 2>&1 | { grep -v "replacing existing signature" || true; }
    [ "${PIPESTATUS[0]}" -eq 0 ] || { echo "signing failed: $f" >&2; exit 1; }
done
"${SIGN[@]}" --entitlements "$HERE/entitlements.plist" "$APP"
codesign --verify --deep --strict "$APP"

# --- dmg ---------------------------------------------------------------------
step "dmg"
STAGE="$CACHE/dmg"
rm -rf "$STAGE" && mkdir -p "$STAGE"
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
hdiutil create -quiet -volname Pakon -srcfolder "$STAGE" -fs HFS+ \
    -format UDZO -ov "$OUT/Pakon.dmg"
rm -rf "$STAGE"
if [ -n "$IDENTITY" ]; then
    codesign --force --timestamp --sign "$IDENTITY" "$OUT/Pakon.dmg"
fi

if [ -n "${PAKON_NOTARY_PROFILE:-}" ]; then
    [ -n "$IDENTITY" ] || { echo "notarization needs a Developer ID identity" >&2; exit 1; }
    step "notarize"
    xcrun notarytool submit "$OUT/Pakon.dmg" --keychain-profile "$PAKON_NOTARY_PROFILE" --wait
    xcrun stapler staple "$OUT/Pakon.dmg"
    spctl --assess --type open --context context:primary-signature -v "$OUT/Pakon.dmg"
fi

step "done"
du -sh "$APP" "$OUT/Pakon.dmg"
