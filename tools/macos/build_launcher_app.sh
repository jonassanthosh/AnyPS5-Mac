#!/bin/sh
set -eu
repo=$(cd "$(dirname "$0")/../.." && pwd)
out="$repo/build-launcher"
app="$out/AnyPS5.app"
swift build -c release --package-path "$repo/tools/macos/launcher" --scratch-path "$out/swift"
rm -rf "$app"
mkdir -p "$app/Contents/MacOS"
cp "$out/swift/release/AnyPS5" "$app/Contents/MacOS/AnyPS5"
cat > "$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>AnyPS5</string>
    <key>CFBundleIdentifier</key>
    <string>org.anyps5.launcher</string>
    <key>CFBundleName</key>
    <string>AnyPS5</string>
    <key>CFBundleDisplayName</key>
    <string>AnyPS5</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>0.1</string>
    <key>CFBundleVersion</key>
    <string>1</string>
    <key>LSMinimumSystemVersion</key>
    <string>14.0</string>
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>LSApplicationCategoryType</key>
    <string>public.app-category.games</string>
    <key>AnyPS5Repository</key>
    <string>$repo</string>
</dict>
</plist>
EOF
codesign --force --sign - "$app"
echo "$app"
