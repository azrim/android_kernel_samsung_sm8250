#!/bin/bash
set -e

KERNEL_DIR=$(pwd)
DEVICE="${1:-r8q}"
DEVICE2="$2"
DEVICE3="$3"
BUILD_START=$(date +%s)
BUILD_TAG="$(date +%Y%m%d-%H%M)"
ZIP_NAME="Solvege-${DEVICE}-${BUILD_TAG}.zip"

# Load Telegram environment variables if exists
if [ -f "$KERNEL_DIR/.env" ]; then
    . "$KERNEL_DIR/.env"
elif [ -f "$KERNEL_DIR/telegram.env" ]; then
    . "$KERNEL_DIR/telegram.env"
fi
export TG_BOT_TOKEN TG_CHAT_ID

build_kernel() {
    echo "-----------------------------------------------"
    echo "Beginning kernel compilation for $DEVICE..."
    echo "-----------------------------------------------"

    export ARCH=arm64
    mkdir -p out
    rm -f out/arch/arm64/boot/Image out/arch/arm64/boot/dts/dtb out/dtbo.img

    export PATH="${HOME}/.local/bin:${HOME}/toolchains/neutron-clang/bin:$(pwd)/llvm-21/bin:$PATH"
    export LD_LIBRARY_PATH="${HOME}/.local/lib:${LD_LIBRARY_PATH}"
    export BISON_PKGDATADIR="${HOME}/.local/share/bison"
    export M4="${HOME}/.local/bin/m4"
    export HOSTCFLAGS="-idirafter ${HOME}/.local/include"
    export HOSTCXXFLAGS="-idirafter ${HOME}/.local/include"
    export HOSTLDFLAGS="-B${HOME}/.local/lib -L${HOME}/.local/lib"
    export CCACHE_DIR="${HOME}/.cache/ccache"
    export CCACHE_MAXSIZE="50G"
    export CCACHE_SLOPPINESS="file_macro,time_macros,include_file_mtime,include_file_ctime"
    # CCACHE_HARDLINK=1 leaks in from the shell rc and forces hard_link=true
    # in ccache 4.x (CCACHE_NOHARDLINK was removed in 4.0). Hardlinked cache
    # entries into .tmp_<file>.o make `mv -f .tmp_$(@F) $@` fail with "are the
    # same file" on recompiles (seen on signal32.o, fallback_table.o).
    unset CCACHE_HARDLINK
    export CC="ccache clang"
    export CXX="ccache clang++"
    export HOSTCC="ccache clang"
    export HOSTCXX="ccache clang++"

    BUILD_VAR="-j$(nproc) -C $(pwd) O=$(pwd)/out ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- LLVM=1 LLVM_IAS=1"

    CONFIG_FRAGMENTS="arch/arm64/configs/vendor/kona-sec-perf_defconfig arch/arm64/configs/vendor/samsung/$DEVICE.config"
    if [ -f arch/arm64/configs/ksu.config ]; then
        CONFIG_FRAGMENTS="$CONFIG_FRAGMENTS arch/arm64/configs/ksu.config"
    fi
    cat $CONFIG_FRAGMENTS > arch/arm64/configs/temp_defconfig

    cat << 'EOF' >> arch/arm64/configs/temp_defconfig
CONFIG_THINLTO=y
# CONFIG_LTO_NONE is not set
CONFIG_LTO_CLANG=y
CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDUTIL=y
CONFIG_LOCALVERSION="-Solvege"
EOF

    # The concatenated fragments assign some symbols more than once (e.g. AUDIT
    # in both the defconfig and ksu.config, LOCALVERSION/LTO_CLANG here vs the
    # defconfig). kconfig warns "override: reassigning to symbol ..." on every
    # reassignment even when the value is identical, and the later assignment
    # already wins. Drop all but the last assignment of each symbol so the
    # generated .config is unchanged but the override warnings disappear.
    awk '
    {
        line = $0; sym = ""
        if (match(line, /^CONFIG_[A-Za-z0-9_]+=/)) {
            sym = substr(line, 1, RLENGTH - 1)
        } else if (match(line, /^# CONFIG_[A-Za-z0-9_]+ is not set$/)) {
            sym = line; sub(/^# /, "", sym); sub(/ is not set$/, "", sym)
        }
        if (sym != "") last[sym] = NR
        lines[NR] = line; sym_of[NR] = sym
    }
    END {
        for (i = 1; i <= NR; i++) {
            s = sym_of[i]
            if (s == "" || last[s] == i) print lines[i]
        }
    }' arch/arm64/configs/temp_defconfig > arch/arm64/configs/temp_defconfig.dedup
    mv -f arch/arm64/configs/temp_defconfig.dedup arch/arm64/configs/temp_defconfig

    make CC="ccache clang" CXX="ccache clang++" HOSTCC="ccache clang" HOSTCXX="ccache clang++" $BUILD_VAR temp_defconfig
    rm arch/arm64/configs/temp_defconfig
}

build_dtb() {
    echo "-----------------------------------------------"
    echo "Building kernel and dtb..."
    echo "-----------------------------------------------"
    make CC="ccache clang" CXX="ccache clang++" HOSTCC="ccache clang" HOSTCXX="ccache clang++" $BUILD_VAR
    make CC="ccache clang" CXX="ccache clang++" HOSTCC="ccache clang" HOSTCXX="ccache clang++" $BUILD_VAR dtbs

    if [ ! -f "$(pwd)/out/arch/arm64/boot/Image" ]; then
        echo "❌ Error: Kernel Image compilation failed! File out/arch/arm64/boot/Image does not exist."
        exit 1
    fi

    cat "$(pwd)/out/arch/arm64/boot/dts/vendor/qcom/kona.dtb" \
        "$(pwd)/out/arch/arm64/boot/dts/vendor/qcom/kona-v2.dtb" \
        "$(pwd)/out/arch/arm64/boot/dts/vendor/qcom/kona-v2.1.dtb" \
        > "$(pwd)/out/arch/arm64/boot/dts/dtb"
}

build_dtbo() {
    echo "-----------------------------------------------"
    echo "Building dtbo.img..."
    echo "-----------------------------------------------"
    DTBO_FILES=$(find $(pwd)/out/arch/arm64/boot/dts/samsung/$DEVICE -name kona-sec-$DEVICE-*.dtbo)
    $(pwd)/tools/mkdtimg create $(pwd)/out/dtbo.img --page_size=4096 ${DTBO_FILES}
}

prepare_ak3() {
    echo "-----------------------------------------------"
    echo "Packaging AnyKernel3 flashable zip..."
    echo "-----------------------------------------------"
    if [ ! -f "$KERNEL_DIR/out/arch/arm64/boot/Image" ]; then
        echo "❌ Error: Kernel Image not found in out/arch/arm64/boot/Image"
        exit 1
    fi

    cd AnyKernel3/

    cp "$KERNEL_DIR/out/dtbo.img" dtbo.img
    cp "$KERNEL_DIR/out/arch/arm64/boot/Image" Image
    cp "$KERNEL_DIR/out/arch/arm64/boot/dts/dtb" dtb

    sed -i "s/^device\.name1=.*/device.name1=${DEVICE}/" anykernel.sh
    [ -n "$DEVICE2" ] && sed -i "s/^device\.name2=.*/device.name2=${DEVICE2}/" anykernel.sh
    [ -n "$DEVICE3" ] && sed -i "s/^device\.name3=.*/device.name3=${DEVICE3}/" anykernel.sh

    if command -v zip >/dev/null 2>&1; then
        zip -r9 "$KERNEL_DIR/$ZIP_NAME" * -x .git README.md *placeholder
    else
        python3 -c "
import os, zipfile
with zipfile.ZipFile('$KERNEL_DIR/$ZIP_NAME', 'w', zipfile.ZIP_DEFLATED) as z:
    for root, dirs, files in os.walk('.'):
        for f in files:
            if f.startswith('.git') or f == 'README.md' or f.endswith('.placeholder'):
                continue
            path = os.path.join(root, f)
            arcname = os.path.relpath(path, '.')
            z.write(path, arcname)
"
    fi

    echo "==============================================="
    echo "Build Complete! Flashable zip: $ZIP_NAME"
    echo "==============================================="

    cd "$KERNEL_DIR"
}

upload_telegram() {
    BUILD_END=$(date +%s)
    DIFF=$((BUILD_END - BUILD_START))
    DURATION="$((DIFF / 60))m $((DIFF % 60))s"

    if [ -z "$TG_BOT_TOKEN" ] || [ -z "$TG_CHAT_ID" ]; then
        echo "-----------------------------------------------"
        echo "Telegram bot token or chat ID not set. Skipping upload."
        echo "To enable, set TG_BOT_TOKEN & TG_CHAT_ID in telegram.env"
        echo "-----------------------------------------------"
        return 0
    fi

    ZIP_FILE="$KERNEL_DIR/$ZIP_NAME"
    if [ ! -f "$ZIP_FILE" ]; then
        echo "Zip file not found: $ZIP_FILE. Cannot upload."
        return 1
    fi

    # ---- metadata for the caption ------------------------------------------
    case "$DEVICE" in
        r8q|r8qxx|r8qxxx) DEVICE_NAME="Galaxy S20 FE 5G" ;;
        c1q)              DEVICE_NAME="Galaxy Note 20 Ultra" ;;
        c2q)              DEVICE_NAME="Galaxy Note 20" ;;
        f2q)              DEVICE_NAME="Galaxy Z Fold2" ;;
        x1q)              DEVICE_NAME="Galaxy S20" ;;
        y2q)              DEVICE_NAME="Galaxy S20+" ;;
        z3q)              DEVICE_NAME="Galaxy S20 Ultra" ;;
        *)                DEVICE_NAME="$DEVICE" ;;
    esac

    KREL="$(cat "$KERNEL_DIR/out/include/config/kernel.release" 2>/dev/null)"
    [ -n "$KREL" ] || KREL="4.19.325"

    BRANCH="$(git -C "$KERNEL_DIR" rev-parse --abbrev-ref HEAD 2>/dev/null)"
    [ -n "$BRANCH" ] || BRANCH="detached"
    COMMIT_SHA="$(git -C "$KERNEL_DIR" rev-parse HEAD 2>/dev/null)"
    COMMIT_SHORT="$(git -C "$KERNEL_DIR" rev-parse --short HEAD 2>/dev/null)"
    COMMIT_SUBJECT="$(git -C "$KERNEL_DIR" log -1 --pretty=%s 2>/dev/null)"
    REPO_URL="https://github.com/azrim/android_kernel_samsung_sm8250"
    COMMIT_URL="${REPO_URL}/commit/${COMMIT_SHA}"

    COMPILER="$(clang --version 2>/dev/null | head -n1 | sed -e 's/ (.*//')"
    [ -n "$COMPILER" ] || COMPILER="clang"
    CPUS="$(nproc)"

    echo "-----------------------------------------------"
    echo "Uploading $ZIP_NAME to Telegram..."
    echo "-----------------------------------------------"

    # Caption is built in python from the environment so that commit subjects
    # (which may contain quotes / angle brackets) can never break the shell.
    TG_DEVICE="$DEVICE" \
    TG_DEVICE_NAME="$DEVICE_NAME" \
    TG_KREL="$KREL" \
    TG_BRANCH="$BRANCH" \
    TG_COMPILER="$COMPILER" \
    TG_CPUS="$CPUS" \
    TG_DURATION="$DURATION" \
    TG_CLOCK="$(date +'%Y-%m-%d %H:%M %Z')" \
    TG_TAG="$BUILD_TAG" \
    TG_SHA_SHORT="$COMMIT_SHORT" \
    TG_SUBJECT="$COMMIT_SUBJECT" \
    TG_COMMIT_URL="$COMMIT_URL" \
    TG_REPO_URL="$REPO_URL" \
    python3 -c "
import os, sys, html, requests

token = os.environ.get('TG_BOT_TOKEN', '')
chat_id = os.environ.get('TG_CHAT_ID', '')
zip_file = '$ZIP_FILE'

def e(key):
    return html.escape(os.environ.get(key, ''))

caption = (
    '<b>🔨 Solvege Kernel</b> — build complete\n'
    '\n'
    '<b>Compiler:</b> ' + e('TG_COMPILER') + ' · ThinLTO\n'
    '<b>Device:</b> ' + e('TG_DEVICE_NAME') + ' (' + e('TG_DEVICE') + ')\n'
    '<b>Kernel:</b> Solvege <code>' + e('TG_KREL') + '</code>\n'
    '<b>Version:</b> <code>' + e('TG_TAG') + '</code>\n'
    '<b>Branch:</b> <code>' + e('TG_BRANCH') + '</code>\n'
    '<b>CPUs:</b> <code>' + e('TG_CPUS') + '</code> · <b>Build time:</b> <code>' + e('TG_DURATION') + '</code>\n'
    '<b>Clocked at:</b> ' + e('TG_CLOCK') + '\n'
    '\n'
    '<b>Latest commit:</b>\n'
    '<a href=\"' + e('TG_COMMIT_URL') + '\">' + e('TG_SHA_SHORT') + '</a> ' + e('TG_SUBJECT') + '\n'
    '\n'
    '<a href=\"' + e('TG_REPO_URL') + '\">' + e('TG_REPO_URL') + '</a>'
)

if not token or not chat_id:
    print('⚠️ Notice: TG_BOT_TOKEN or TG_CHAT_ID is not configured. Skipping Telegram upload.')
    sys.exit(0)

url = f'https://api.telegram.org/bot{token}/sendDocument'
try:
    with open(zip_file, 'rb') as f:
        r = requests.post(url, data={'chat_id': chat_id, 'caption': caption, 'parse_mode': 'HTML'}, files={'document': f}, timeout=600)
        res = r.json()
        if res.get('ok'):
            print('✅ Upload to Telegram successful!')
        else:
            print('❌ Upload failed:', res.get('description', res))
except Exception as exc:
    print('❌ Upload error:', str(exc))
"
}

build_kernel
build_dtb
build_dtbo
prepare_ak3
upload_telegram
