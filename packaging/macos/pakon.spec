# PyInstaller spec for Pakon.app. Run through build.sh, which builds the C
# tools and libusb first and passes them in PAKON_BIN_DIR.
#
# The bundle keeps the repo layout under sys._MEIPASS (web/, tools/,
# resources/, firmware/, profiles/, oem/, build/), so the web service finds
# its files the same way it does in a checkout.
import os
from pathlib import Path

from PyInstaller.utils.hooks import collect_dynamic_libs, collect_submodules

HERE = Path(SPECPATH)
REPO = HERE.parent.parent
BIN = Path(os.environ["PAKON_BIN_DIR"])
VERSION = os.environ.get("PAKON_VERSION", "0.1.0")

datas = [
    (str(REPO / "web/static"), "web/static"),
    # tools/pakon_eeprom.py is loaded from its file (web/eeprom.py)
    (str(REPO / "tools/*.py"), "tools"),
    # the scan scripts and the captured firmware load, not the captures
    (str(REPO / "resources/*.pakscan"), "resources"),
    (str(REPO / "resources/*.pakfw"), "resources"),
    (str(REPO / "resources/f135plus/*.pakscan"), "resources/f135plus"),
    (str(REPO / "firmware/*.hex"), "firmware"),
    (str(REPO / "firmware/README.md"), "firmware"),
    (str(REPO / "profiles"), "profiles"),
    (str(REPO / "oem"), "oem"),
    (str(REPO / "LICENSE"), "."),
    (str(REPO / "NOTICE"), "."),
]

binaries = [
    (str(BIN / "pakon_probe"), "build"),
    (str(BIN / "pakon_replay"), "build"),
    (str(BIN / "libusb-1.0.0.dylib"), "."),
] + collect_dynamic_libs("unicorn")   # unicorn/lib/libunicorn.2.dylib, loaded by ctypes

a = Analysis(
    [str(HERE / "pakon_app.py")],
    pathex=[str(REPO), str(REPO / "tools")],
    binaries=binaries,
    datas=datas,
    # tools/oem_sba.py imports unicorn and pefile inside functions
    hiddenimports=(collect_submodules("uvicorn") + collect_submodules("unicorn")
                   + ["pefile", "pakon_eeprom"]),
    excludes=["tkinter", "matplotlib", "IPython", "pytest"],
)
# PyInstaller's PyUSB hook adds the build machine's libusb (Homebrew's, built
# for the running macOS); the app uses its own (pakon_app.py).
a.binaries = [b for b in a.binaries if b[0] != "libusb-1.0.dylib"]
pyz = PYZ(a.pure)
exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name="Pakon",
    console=False,
    target_arch="arm64",
)
coll = COLLECT(exe, a.binaries, a.datas, name="Pakon")
app = BUNDLE(
    coll,
    name="Pakon.app",
    icon=os.environ.get("PAKON_ICON"),
    bundle_identifier="com.pakon.scanner",      # same as the earlier release
    version=VERSION,
    info_plist={
        "CFBundleDisplayName": "Pakon",
        "CFBundleShortVersionString": VERSION,
        "CFBundleVersion": VERSION,
        "LSMinimumSystemVersion": "13.0",
        "LSApplicationCategoryType": "public.app-category.photography",
        "NSHighResolutionCapable": True,
        "NSHumanReadableCopyright": "AGPL-3.0-or-later. Not affiliated with Kodak or Pakon.",
    },
)
