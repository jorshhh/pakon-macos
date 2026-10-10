# Pakon.app (macOS)

A double-clickable app for people who don't use the command line. It runs the
web service from `web/` and shows it in its own window with
[pywebview](https://pywebview.flowrl.com) (WKWebView); closing the window quits.
PyInstaller freezes it with its own Python, the C tools, libusb and the
firmware, so the Mac needs nothing installed. Apple silicon, macOS 13 or later.

| File                 | What it is |
|----------------------|------------|
| `pakon_app.py`       | Entry point: web service on a free localhost port + the window |
| `pakon.spec`         | PyInstaller spec: what goes in the bundle |
| `build.sh`           | Builds libusb and the C tools for macOS 13, freezes, signs, makes the `.dmg`, notarizes |
| `requirements.txt`   | Build requirements (the web service's, plus pywebview and PyInstaller) |
| `entitlements.plist` | Hardened-runtime exceptions for Python, unicorn and the bundled libraries |
| `make_icon.py`       | Draws the app icon |

## Build

```sh
packaging/macos/build.sh 0.2.0        # version number
```

Output goes to `dist/macos/`: `Pakon.app` and `Pakon.dmg`. Downloads and
intermediate files are cached in `packaging/macos/.cache/` (both git-ignored).
Needs Xcode's command-line tools, CMake and pkg-config. Python and libusb are
downloaded at pinned versions and checked against their SHA-256.

`build.sh` fails if any binary in the bundle needs a newer macOS than 13: wheels
are fetched for macOS 13, and libusb is built from source because Homebrew's
targets the running macOS.

To try the window without building: `python packaging/macos/pakon_app.py`
(with `web/requirements.txt` and `pywebview` installed).

## Signing and notarization

`build.sh` signs with the first **Developer ID Application** certificate in the
keychain (or `PAKON_SIGN_IDENTITY`). Without one it signs ad hoc: the app runs
on the Mac that built it, but other Macs will refuse it.

Notarizing needs a stored App Store Connect login, once per Mac:

```sh
xcrun notarytool store-credentials pakon-notary \
    --apple-id YOU@EXAMPLE.COM --team-id T928P2TAKQ
```

Then:

```sh
PAKON_NOTARY_PROFILE=pakon-notary packaging/macos/build.sh 0.2.0
```

This submits the `.dmg`, waits for Apple, staples the ticket and checks it with
`spctl`. Upload `dist/macos/Pakon.dmg` to a GitHub release under that exact
name: the website's Download button points at
`releases/latest/download/Pakon.dmg`.

## Where the app keeps things

- EEPROM archives: `~/Library/Application Support/Pakon/eeprom/` (the app
  bundle is read-only). Keep a copy off the machine, as with `backups/eeprom/`.
- Log: `~/Library/Logs/Pakon/pakon.log`.
- Scans: wherever the operator picks in the UI (home folder by default).
