"""
Pakon.app: the web service in its own window.

Runs web.app on a free localhost port and shows it with pywebview (WKWebView);
closing the window quits. PyInstaller freezes this (pakon.spec, build.sh). It
also runs from a checkout, for trying the window without building:

    python packaging/macos/pakon_app.py

In the app the EEPROM archives go to ~/Library/Application Support/Pakon
(PAKON_DATA, web/eeprom.py): the signed bundle is read-only. The log is
~/Library/Logs/Pakon/pakon.log.
"""
import functools
import os
import socket
import sys
import threading
import time
from pathlib import Path

FROZEN = getattr(sys, "frozen", False)
# The bundle mirrors the repo layout: web/, tools/, resources/, build/, ...
ROOT = Path(sys._MEIPASS) if FROZEN else Path(__file__).resolve().parents[2]
if not FROZEN:
    sys.path.insert(0, str(ROOT))

HOME = Path.home()
if FROZEN:
    os.environ.setdefault("PAKON_DATA", str(HOME / "Library/Application Support/Pakon"))
    Path(os.environ["PAKON_DATA"]).mkdir(parents=True, exist_ok=True)
    logs = HOME / "Library/Logs/Pakon"
    logs.mkdir(parents=True, exist_ok=True)
    # a windowed app has no terminal; uvicorn and the tools still print
    sys.stdout = sys.stderr = open(logs / "pakon.log", "a", buffering=1)

# PyUSB looks libusb up with ctypes.util.find_library, which fails on a Mac
# without Homebrew: point its backend at the copy in the bundle.
_libusb = ROOT / "libusb-1.0.0.dylib"
if _libusb.exists():
    import usb.backend.libusb1 as _libusb1
    _libusb1.get_backend = functools.partial(_libusb1.get_backend,
                                             find_library=lambda _name: str(_libusb))

import uvicorn  # noqa: E402
import webview  # noqa: E402

from web.app import app  # noqa: E402


def _free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def main():
    port = _free_port()
    server = uvicorn.Server(uvicorn.Config(app, host="127.0.0.1", port=port,
                                           log_level="info"))
    thread = threading.Thread(target=server.run, daemon=True)
    thread.start()
    deadline = time.monotonic() + 30
    while not server.started and thread.is_alive() and time.monotonic() < deadline:
        time.sleep(0.05)

    webview.settings["ALLOW_DOWNLOADS"] = True       # the frame / zip links
    if server.started:
        webview.create_window("Pakon", f"http://127.0.0.1:{port}/",
                              width=1280, height=880, min_size=(960, 640))
    else:
        webview.create_window("Pakon", html=(
            "<body style='font:15px -apple-system;padding:2em'>"
            "<h2>Pakon could not start its scanner service.</h2>"
            "<p>Details are in ~/Library/Logs/Pakon/pakon.log.</p></body>"))
    webview.start()

    server.should_exit = True                         # window closed: quit
    thread.join(timeout=5)


if __name__ == "__main__":
    main()
