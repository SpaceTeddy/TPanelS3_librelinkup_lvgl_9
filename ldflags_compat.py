"""Adds linker flags only if the active toolchain actually understands them.

binutils >= 2.39 (Arduino core 3.x / pioarduino) warns about prebuilt newlib
objects that carry no .note.GNU-stack section:

    libc_a-tolower_l.o: missing .note.GNU-stack section implies executable stack

That object ships inside the toolchain's own libc.a and the warning is
meaningless on the ESP32, so it is suppressed. The suppression flag does not
exist in the ld 2.35 that comes with espressif32 <= 6.x, and an unknown option
is a *hard error* there rather than a no-op -- which would break the core 2.x
build. Hence: probe, do not assume a toolchain.

Must be registered as a `post:` script -- in a `pre:` script $CC is still the
host compiler ("cc") and the probe silently tests the wrong toolchain.
"""

import subprocess

Import("env")  # noqa: F821 - injected by PlatformIO


def linker_supports(option):
    """True if the toolchain's ld lists `option` in its help output."""
    try:
        result = subprocess.run(
            [env.subst("$CC"), "-Wl,--help"],  # noqa: F821
            capture_output=True, text=True, timeout=60,
        )
    except (OSError, subprocess.SubprocessError):
        return False
    return option in result.stdout


# Note: this also hides the warning for our own objects, should one ever
# trigger it. Drop the flag to check.
if linker_supports("--no-warn-execstack"):
    env.Append(LINKFLAGS=["-Wl,--no-warn-execstack"])  # noqa: F821
