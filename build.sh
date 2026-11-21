#!/usr/bin/env bash
set -euo pipefail
WS="$GITHUB_WORKSPACE"
cd "$WS"

echo "=== [1/6] toolchain ==="
clang --version | head -1
llvm-objcopy --version | head -1

echo "=== [2/6] fetch GKI android15-6.6.77_r00 ==="
curl -fL --retry 3 -o kernel.tar.gz "https://android.googlesource.com/kernel/common/+archive/android15-6.6.77_r00.tar.gz"
mkdir -p kernel
tar xzf kernel.tar.gz -C kernel

echo "=== [3/6] prepare kernel with EXACT device .config ==="
cp device.config kernel/.config
cd kernel
# device uname: 6.6.77-android15-8-2755199-abogkiS9380ZCS9BZB2-4k
sed -i 's/^CONFIG_LOCALVERSION=.*/CONFIG_LOCALVERSION="-android15-8-2755199-abogkiS9380ZCS9BZB2-4k"/' .config
sed -i 's/^CONFIG_MODULE_SIG=y/CONFIG_MODULE_SIG=n/' .config
sed -i '/^CONFIG_LOCALVERSION_AUTO=/d' .config
make ARCH=arm64 LLVM=1 olddefconfig
make ARCH=arm64 LLVM=1 -j"$(nproc)" modules_prepare
echo "--- utsrelease.h ---"
cat include/generated/utsrelease.h
echo "--- relevant config ---"
grep -E '^CONFIG_(LOCALVERSION|MODVERSIONS|MODULE_SIG|CFI_CLANG)' .config || true

echo "=== [4/6] fetch DFRoot v4.7 source ==="
cd "$WS"
curl -fL --retry 3 -o dfroot.tar.gz "https://github.com/diabl0w/DFRoot/archive/refs/tags/v4.7.tar.gz"
tar xzf dfroot.tar.gz
mv DFRoot-4.7 dfroot

echo "=== [5/6] build dfroot.ko against 6.6.77 ==="
cd "$WS/dfroot/lkm"
make KDIR="$WS/kernel" ARCH=arm64 LLVM=1 OBJCOPY=llvm-objcopy KBUILD_MODPOST_WARN=1 -j"$(nproc)"
llvm-objcopy --strip-unneeded \
  -R .comment -R .note.gnu.build-id -R .note.gnu.property \
  -R .note.Linux -R .note.GNU-stack \
  -R .BTF -R .BTF.base -R .llvm_addrsig \
  -R .hyp.text -R .hyp.bss -R .hyp.rodata -R .hyp.event_ids \
  -R .hyp.patchable_function_entries -R .hyp.data dfroot.ko || true
# keep the "modversions" vermagic flag but drop CRC table: device has different CRCs
llvm-objcopy --remove-section=__versions dfroot.ko 2>/dev/null || true
echo "--- built module ---"
ls -l dfroot.ko
strings dfroot.ko | grep -E 'vermagic|^name=' || true
mkdir -p "$WS/dfroot/app/src/main/jni/ko"
cp dfroot.ko "$WS/dfroot/app/src/main/jni/ko/dfroot-android15-6.6.ko"
for k in android12-5.10 android13-5.10 android13-5.15 android14-5.15 android14-6.1 android16-6.12 android17-6.18; do
  : > "$WS/dfroot/app/src/main/jni/ko/dfroot-$k.ko"
done
ls -l "$WS/dfroot/app/src/main/jni/ko"

echo "=== [6/6] build signed APK ==="
cd "$WS/dfroot"
chmod +x gradlew
echo "sdk.dir=$ANDROID_HOME" > local.properties
keytool -genkeypair -keystore keystore.jks -storetype JKS \
  -storepass df-ksu -keypass df-ksu -alias df-ksu \
  -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=DFRoot" -noprompt
printf 'KEYSTORE_FILE=../keystore.jks\nKEYSTORE_PASSWORD=df-ksu\nKEY_ALIAS=df-ksu\nKEY_PASSWORD=df-ksu\n' > signing.properties
./gradlew :app:assembleRelease --no-daemon --stacktrace
mkdir -p "$WS/out"
cp app/build/outputs/apk/release/DFRoot_*.apk "$WS/out/DFRoot-4.7-S9380-ZB2.apk"
ls -lh "$WS/out"
echo "BUILD OK"
