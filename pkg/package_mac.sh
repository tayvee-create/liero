#!/bin/bash
# Builds a self-contained Liero.app (SDL libraries bundled inside) and a zip to
# share. Needs: brew install cmake sdl2 sdl2_image dylibbundler
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
DIST="$ROOT/dist"
APP="$DIST/Liero.app"

cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 >/dev/null
cmake --build "$BUILD" -j8 --target openliero

rm -rf "$DIST"
mkdir -p "$DIST"
cp -R "$BUILD/openliero.app" "$APP"

# Copy Homebrew dylibs into the bundle and point the executable at them
dylibbundler -of -b -cd \
	-x "$APP/Contents/MacOS/openliero" \
	-d "$APP/Contents/Frameworks/" \
	-p @executable_path/../Frameworks/ >/dev/null

# sdl2-compat dlopens SDL3 at runtime from @loader_path/libSDL3.dylib, so bundle it under that name
SDL3="$(brew --prefix sdl3)/lib/libSDL3.0.dylib"
cp "$SDL3" "$APP/Contents/Frameworks/libSDL3.dylib"
chmod u+w "$APP/Contents/Frameworks/libSDL3.dylib"
install_name_tool -id @loader_path/libSDL3.dylib "$APP/Contents/Frameworks/libSDL3.dylib" 2>/dev/null

# dylibbundler can add the same LC_RPATH several times, which dyld refuses to load
for bin in "$APP/Contents/MacOS/openliero" "$APP"/Contents/Frameworks/*.dylib; do
	otool -l "$bin" | awk '/LC_RPATH/{getline; getline; print $2}' | sort | uniq -c | while read -r count rpath; do
		for ((i = 1; i < count; i++)); do
			install_name_tool -delete_rpath "$rpath" "$bin" 2>/dev/null
		done
	done
done

# Ad-hoc signature (no Apple developer account); required to run on Apple Silicon
codesign --force --deep --sign - "$APP"
codesign --verify --deep "$APP"

# Nothing should still point at Homebrew
if otool -L "$APP/Contents/MacOS/openliero" "$APP"/Contents/Frameworks/*.dylib | grep -q /opt/homebrew; then
	echo "warning: bundle still references Homebrew libraries" >&2
fi

(cd "$DIST" && ditto -c -k --keepParent Liero.app Liero.zip)
echo "Built $APP and $DIST/Liero.zip"
