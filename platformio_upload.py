# Allows PlatformIO to upload directly to ElegantOTA
#
# To use:
# - copy this script into the same folder as your platformio.ini
# - set the following for your project in platformio.ini:
#
# extra_scripts = platformio_upload.py
# upload_protocol = custom
# custom_upload_url = <your upload URL>
#
# An example of an upload URL:
#                custom_upload_url = http://192.168.1.123/update
# also possible: custom_upload_url = http://domainname/update

import sys
import time
import requests
import hashlib
from urllib.parse import urlparse
from requests.auth import HTTPDigestAuth

Import("env")

try:
    from requests_toolbelt import MultipartEncoder, MultipartEncoderMonitor
except ImportError:
    env.Execute("$PYTHONEXE -m pip install requests_toolbelt")
    from requests_toolbelt import MultipartEncoder, MultipartEncoderMonitor


def _format_bytes(num: float) -> str:
    for unit in ["B", "KB", "MB", "GB", "TB"]:
        if num < 1024.0:
            return f"{num:,.1f}{unit}"
        num /= 1024.0
    return f"{num:,.1f}PB"


def _format_eta(seconds: float) -> str:
    seconds = int(max(0, seconds))
    m, s = divmod(seconds, 60)
    h, m = divmod(m, 60)
    if h > 0:
        return f"{h:d}:{m:02d}:{s:02d}"
    return f"{m:d}:{s:02d}"


def _progress_bar(pct: float, width: int = 24) -> str:
    filled = int(width * pct / 100.0)
    empty = width - filled
    return "█" * filled + "░" * empty


class OneLineProgressANSI:
    """
    Robust progress display for environments where '\r' doesn't work (PIO output capture).
    Technique:
      - print a full line with newline
      - on next update: move cursor up 1 line and clear it, then print the updated line
    Works in real terminals that interpret ANSI escape codes.
    """
    def __init__(self, total: int, prefix: str = "Uploading", min_interval_s: float = 0.15):
        self.total = max(int(total), 1)
        self.prefix = prefix
        self.min_interval_s = float(min_interval_s)
        self.start = time.time()
        self.last_print = 0.0
        self.printed_once = False

    def update(self, current_bytes: int):
        now = time.time()

        # throttle output (reduces spam + makes some consoles happier)
        if (now - self.last_print) < self.min_interval_s and current_bytes < self.total:
            return

        current_bytes = max(0, min(int(current_bytes), self.total))
        pct = (current_bytes / self.total) * 100.0
        elapsed = max(now - self.start, 1e-6)
        speed = current_bytes / elapsed  # B/s

        remaining_bytes = self.total - current_bytes
        eta_s = remaining_bytes / speed if speed > 1e-6 else 0.0
        eta = _format_eta(eta_s)

        bar = _progress_bar(pct, width=24)

        line = (
            f"{self.prefix}: {pct:6.2f}% "
            f"[{bar}] "
            f"{_format_bytes(speed)}/s "
            f"ETA {eta}"
        )

        if self.printed_once:
            # Cursor up 1 line + clear line
            sys.stdout.write("\x1b[1A\x1b[2K")
        else:
            self.printed_once = True

        sys.stdout.write(line + "\n")
        sys.stdout.flush()
        self.last_print = now


def on_upload(source, target, env):
    firmware_path = str(source[0])

    auth = None
    upload_url_compatibility = env.GetProjectOption("custom_upload_url")
    upload_url = upload_url_compatibility.replace("/update", "")

    print("Starting OTA upload…")

    with open(firmware_path, "rb") as firmware:
        md5 = hashlib.md5(firmware.read()).hexdigest()

        parsed_url = urlparse(upload_url)
        host_ip = parsed_url.netloc

        # Start request
        start_url = f"{upload_url}/ota/start?mode=fr&hash={md5}"

        start_headers = {
            "Host": host_ip,
            "User-Agent": "Mozilla/5.0 (PlatformIO-ElegantOTA)",
            "Accept": "*/*",
            "Accept-Language": "de,en-US;q=0.7,en;q=0.3",
            "Accept-Encoding": "gzip, deflate",
            "Referer": f"{upload_url}/update",
            "Connection": "keep-alive",
        }

        # Check if auth is required
        checkAuthResponse = requests.get(f"{upload_url_compatibility}/update")

        if checkAuthResponse.status_code == 401:
            try:
                username = env.GetProjectOption("custom_username")
                password = env.GetProjectOption("custom_password")
            except Exception:
                username = None
                password = None
                print("No authentication values specified.")
                print(
                    "Please, add some Options in your .ini file like:\n\n"
                    "custom_username=username\ncustom_password=password\n"
                )

            if username is None or password is None:
                print("Authentication required, but no credentials provided.")
                return

            print("Serverconfiguration: authentication needed.")
            auth = HTTPDigestAuth(username, password)
            doUpdateAuth = requests.get(start_url, headers=start_headers, auth=auth)

            if doUpdateAuth.status_code != 200:
                print("authentication faild " + str(doUpdateAuth.status_code))
                return
            print("Authentication successfull")
        else:
            auth = None
            print("Serverconfiguration: autentication not needed.")
            doUpdate = requests.get(start_url, headers=start_headers)

            if doUpdate.status_code != 200:
                print("start-request faild " + str(doUpdate.status_code))
                return

        # Prepare upload
        firmware.seek(0)
        encoder = MultipartEncoder(
            fields={
                "MD5": md5,
                "firmware": ("firmware", firmware, "application/octet-stream"),
            }
        )

        progress = OneLineProgressANSI(total=encoder.len, prefix="Uploading", min_interval_s=0.15)

        def _cb(monitor):
            progress.update(monitor.bytes_read)

        monitor = MultipartEncoderMonitor(encoder, _cb)

        post_headers = {
            "Host": host_ip,
            "User-Agent": "Mozilla/5.0 (PlatformIO-ElegantOTA)",
            "Accept": "*/*",
            "Accept-Language": "de,en-US;q=0.7,en;q=0.3",
            "Accept-Encoding": "gzip, deflate",
            "Referer": f"{upload_url}/update",
            "Connection": "keep-alive",
            "Content-Type": monitor.content_type,
            "Content-Length": str(monitor.len),
            "Origin": f"{upload_url}",
        }

        response = requests.post(
            f"{upload_url}/ota/upload",
            data=monitor,
            headers=post_headers,
            auth=auth,
        )

        # Force final update (100%)
        progress.update(encoder.len)
        time.sleep(0.1)

        if response.status_code != 200:
            print("\nUpload faild.\nServer response: " + response.text)
        else:
            print("\nUpload successful\nServer response: " + response.text)


# Only take over the upload when the project actually selected this protocol.
# The script used to replace UPLOADCMD unconditionally, so a serial upload
# (upload_protocol = esptool) still ended up in on_upload() and died on the
# missing custom_upload_url -- which made it look like enabling extra_scripts
# broke the build, when in fact only `-t upload` was affected.
if env.GetProjectOption("upload_protocol", "") == "custom":
    env.Replace(UPLOADCMD=on_upload)
