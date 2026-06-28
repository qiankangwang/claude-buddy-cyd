# PlatformIO pre-build hook.
#
# The vendored XPT2046_Touchscreen library hardcodes its pressure threshold as
# `#define Z_THRESHOLD 300` and exposes no setter, so a light or quick tap (most
# noticeably the triple-tap easter egg) often never registers. We lower it here.
#
# Why a build hook instead of editing the file: the library lives under
# .pio/libdeps (gitignored, re-downloaded on a clean build), so a direct edit
# would not survive. This runs on every build, is idempotent, and self-heals
# after any library reinstall. Bump TOUCH_Z_THRESHOLD back up if phantom touches
# appear; the upstream default is 300 (higher = firmer press required).
import os
import re

Import("env")  # noqa: F821  (injected by PlatformIO/SCons)

TOUCH_Z_THRESHOLD = 220  # upstream default 300

lib_cpp = os.path.join(
    env.subst("$PROJECT_LIBDEPS_DIR"),  # noqa: F821
    env.subst("$PIOENV"),               # noqa: F821
    "XPT2046_Touchscreen",
    "XPT2046_Touchscreen.cpp",
)


def patch():
    if not os.path.isfile(lib_cpp):
        print("[touch-threshold] library not installed yet, skipping:", lib_cpp)
        return
    with open(lib_cpp, "r", encoding="utf-8", errors="ignore") as f:
        src = f.read()
    # Only the bare `Z_THRESHOLD` define; `Z_THRESHOLD_INT` (IRQ wake floor) is
    # left alone -- `\s+` after the name won't match the `_INT` variant.
    new = re.sub(
        r"(#define\s+Z_THRESHOLD\s+)\d+",
        lambda m: m.group(1) + str(TOUCH_Z_THRESHOLD),
        src,
        count=1,
    )
    if new != src:
        with open(lib_cpp, "w", encoding="utf-8") as f:
            f.write(new)
        print("[touch-threshold] set Z_THRESHOLD=%d" % TOUCH_Z_THRESHOLD)
    else:
        print("[touch-threshold] already Z_THRESHOLD=%d" % TOUCH_Z_THRESHOLD)


patch()
