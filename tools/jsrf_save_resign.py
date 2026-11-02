#!/usr/bin/env python3
"""Sign a Jet Set Radio Future save for JSRF Unleashed.

    python3 tools/jsrf_save_resign.py <JSRFDATA.SAV or a folder holding saves> [...]

An Xbox save carries a signature in its first 20 bytes, worked out from the
rest of the file with a key that depends on the console -- or the emulator --
that made it. JSRF checks it, and calls a save whose signature it cannot
match damaged. A save made on an Xbox, in xemu or in Cxbx-Reloaded is signed
with their key, not the one JSRF Unleashed's runtime has (sixteen zero bytes:
it has no console of its own), so it is signed again here: an HMAC-SHA1 of
everything after the signature, with that key. The file is changed in place;
a copy of it as it was is kept beside it (JSRFDATA.SAV.orig). A save that is
already right is left alone.

Where to put it first: in the game folder, UDATA/5345000a/ (5345000a is JSRF's
title ID), each save in its own folder as the emulator or the console had it --
for Cxbx-Reloaded, from its EmuDisk/Partition1/UDATA/5345000a/; for xemu, from
the hard-disk image with a tool such as xemu-hdd-image or fatxplorer.
"""
import hashlib
import hmac
import os
import shutil
import sys

KEY = bytes(16)
SIG = 20


def resign(path):
    data = open(path, "rb").read()
    if len(data) <= SIG + 4 or data[SIG:SIG + 4] != b"JSRF":
        print("%s: not a JSRF save (no JSRF after the signature) -- left alone" % path)
        return False
    want = hmac.new(KEY, data[SIG:], hashlib.sha1).digest()
    if data[:SIG] == want:
        print("%s: already signed for JSRF Unleashed" % path)
        return True
    keep = path + ".orig"
    if not os.path.exists(keep):
        shutil.copy2(path, keep)
    with open(path, "r+b") as f:
        f.write(want)
    print("%s: signed (the file as it was: %s)" % (path, os.path.basename(keep)))
    return True


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    n = 0
    for a in argv:
        if os.path.isdir(a):
            for root, _dirs, files in os.walk(a):
                for name in files:
                    if name.upper().endswith(".SAV"):
                        n += resign(os.path.join(root, name))
        else:
            n += resign(a)
    if not n:
        print("no JSRF save found")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
