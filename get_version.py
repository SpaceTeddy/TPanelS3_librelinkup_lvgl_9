Import("env")
import os
import subprocess


def get_firmware_version():
    # CI passes version via env var (set in release.yml)
    v = os.environ.get("FIRMWARE_VERSION", "").strip()
    if v:
        return v
    # Local builds: derive from git describe
    try:
        return subprocess.check_output(
            ["git", "describe", "--tags", "--always", "--dirty=-dirty"],
            cwd=env["PROJECT_DIR"],
            stderr=subprocess.DEVNULL,
        ).strip().decode()
    except Exception:
        return "0.0.0-dev"


version = get_firmware_version()
print(f"[get_version] APP_FIRMWARE_VERSION = {version}")
env.Append(CPPDEFINES=[("APP_FIRMWARE_VERSION", f'\\"{version}\\"')])
