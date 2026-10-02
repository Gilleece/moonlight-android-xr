#!/usr/bin/env python3
#
# Writes the QR code the session's Ko-fi sheet shows, once, as a black on
# white PNG: version 3 (29 by 29 modules), medium error correction, 8 px a
# module and the four module quiet zone the standard asks for, 296 px square.
#
#   python3 -m venv /tmp/qr && /tmp/qr/bin/pip install qrcode pillow
#   /tmp/qr/bin/python tools/make_qr.py
#
# It needs the qrcode package (BSD licence) and Pillow (HPND), installed in a
# throwaway virtual environment as above. Neither is an app dependency and
# nothing of them ships: only the PNG this writes goes into the APK.

import os
import sys

import qrcode

URL = "https://ko-fi.com/moonlightxr"
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, "..", "app", "src", "main", "assets", "images",
                                    "kofi_qr.png"))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else OUT
    qr = qrcode.QRCode(version=3, error_correction=qrcode.constants.ERROR_CORRECT_M,
                       box_size=8, border=4)
    qr.add_data(URL)
    # Held to version 3, so a change to the address that will not fit says so
    # rather than quietly growing the code
    qr.make(fit=False)
    # One bit a pixel: the modules are all there is to it
    image = qr.make_image(fill_color="black", back_color="white").get_image().convert("1")
    image.save(out, optimize=True)
    print("%s: %d modules, %dx%d px" % (out, qr.modules_count, image.size[0], image.size[1]))


if __name__ == "__main__":
    main()
