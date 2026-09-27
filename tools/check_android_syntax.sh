#!/usr/bin/env bash
#
# 用「實際出貨的 NDK clang」對所有原生 C 來源做語法檢查（-fsyntax-only，秒級）。
#
# 為什麼需要這一關：
#   1. Windows 開發機跑不了 epoll 整合測試，Linux VM 只有 GCC，而 **GCC 與 Clang 對
#      C11 atomics 的寬鬆度不同** —— `atomic_load_explicit(&plain_int, ...)` 在 GCC
#      放行、在 Clang（NDK 26）直接是 error：
#        "address argument to atomic operation must be a pointer to _Atomic type"
#      這種錯只有真正建置 Android target 才會現形，而 assembleRelease 要等好幾分鐘。
#   2. 編輯器/工具在改檔時偶爾會吃掉換行，把 `// 註解` 與下一行的函式簽名黏成一行，
#      於是函式體變成檔案層級語句，錯誤訊息卻指向毫不相關的行號（曾實際發生）。
#      -fsyntax-only 幾秒就能抓到。
#
# 用法：bash tools/check_android_syntax.sh
# 離開碼：0 = 乾淨；非 0 = 有 error（warning 不阻擋，只列出）。

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

# NDK 版本取自 app/build.gradle 的 ndkVersion；位置可由 ANDROID_NDK_HOME 覆蓋。
NDK_VERSION="$(sed -n 's/.*ndkVersion[[:space:]]*"\([^"]*\)".*/\1/p' "${ROOT_DIR}/app/build.gradle" | head -1)"
SDK_DIR="$(sed -n 's/^sdk\.dir=//p' "${ROOT_DIR}/local.properties" | tr -d '\r' | sed 's|\\\\|/|g; s|\\|/|g')"

NDK_ROOT="${ANDROID_NDK_HOME:-}"
if [ -z "${NDK_ROOT}" ]; then
    if [ -z "${SDK_DIR}" ] || [ ! -d "${SDK_DIR}/ndk/${NDK_VERSION}" ]; then
        echo "找不到 NDK（ndkVersion=${NDK_VERSION}）。請設定 ANDROID_NDK_HOME。" >&2
        exit 2
    fi
    NDK_ROOT="${SDK_DIR}/ndk/${NDK_VERSION}"
fi

CC="${NDK_ROOT}/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe"
[ -x "${CC}" ] || CC="${NDK_ROOT}/toolchains/llvm/prebuilt/linux-x86_64/bin/clang"
[ -x "${CC}" ] || CC="${NDK_ROOT}/toolchains/llvm/prebuilt/darwin-x86_64/bin/clang"
if [ ! -x "${CC}" ]; then
    echo "找不到 NDK clang：${CC}" >&2
    exit 2
fi

# minSdk 取自 app/build.gradle，決定 __ANDROID_API__
MIN_SDK="$(sed -n 's/.*minSdk[[:space:]]*\([0-9]*\).*/\1/p' "${ROOT_DIR}/app/build.gradle" | head -1)"
[ -n "${MIN_SDK}" ] || MIN_SDK=26

CPP_DIR="${ROOT_DIR}/app/src/main/cpp"
SOURCES=(
    "${CPP_DIR}/simple-socks5.c"
    "${CPP_DIR}/udp_dns.c"
    "${CPP_DIR}/udp_conn.c"
    "${CPP_DIR}/socks5_protocol.c"
    "${CPP_DIR}/conn_state.c"
    "${CPP_DIR}/conn_forward.c"
    "${CPP_DIR}/ghost_purge.c"
    "${CPP_DIR}/jni_bridge.c"
)

echo "NDK   : ${NDK_ROOT}"
echo "clang : ${CC}"
echo "target: aarch64-linux-android${MIN_SDK}"
echo

OUT="$("${CC}" \
    --target="aarch64-linux-android${MIN_SDK}" \
    --sysroot="${NDK_ROOT}/toolchains/llvm/prebuilt/windows-x86_64/sysroot" \
    -std=c11 -Wall -Wextra -fsyntax-only \
    -I "${CPP_DIR}" \
    "${SOURCES[@]}" 2>&1)"
RC=$?

# sysroot 路徑在非 Windows 主機上不同，失敗時退回不指定（clang 會用內建預設）
if [ ${RC} -ne 0 ] && echo "${OUT}" | grep -q "sysroot"; then
    OUT="$("${CC}" --target="aarch64-linux-android${MIN_SDK}" \
        -std=c11 -Wall -Wextra -fsyntax-only -I "${CPP_DIR}" "${SOURCES[@]}" 2>&1)"
    RC=$?
fi

if echo "${OUT}" | grep -q "error:"; then
    echo "${OUT}"
    echo
    echo "RESULT: FAIL（有 error）"
    exit 1
fi

# warning 只列出，不阻擋
if [ -n "${OUT}" ]; then
    echo "--- warnings ---"
    echo "${OUT}"
    echo
fi
echo "RESULT: PASS（無 error）"
