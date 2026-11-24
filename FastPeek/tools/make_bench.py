"""Regenerates the synthetic photo folder used for the --bench numbers in README.

    python tools/make_bench.py _bench2 15000 126

Creates <outdir> with `dirs` sub directories holding `count` 900x900 PNGs in
total.  Every image has different content, so the thumbnail cache cannot hit on
one decoded frame.  Expect roughly 10 minutes for 15000 files (~600 MB).
"""
import os
import sys

import numpy as np
from PIL import Image


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "_bench2"
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 15000
    dirs = int(sys.argv[3]) if len(sys.argv) > 3 else 126
    size = 900

    folders = []
    for d in range(dirs):
        p = os.path.join(out, "2020年", "2020-%02d 活动" % (d % 12 + 1), "批次%03d" % d)
        os.makedirs(p, exist_ok=True)
        folders.append(p)

    yy, xx = np.mgrid[0:size, 0:size]
    for i in range(count):
        off = (i * 7) & 0xFF
        v = (xx + yy + off) & 0xFF
        arr = np.stack([v, (v * 3) & 0xFF, 255 - v], axis=-1).astype(np.uint8)

        bx = (i * 13) % (size - 120)
        arr[60:180, bx:bx + 120] = 250

        name = "IMG_20%05d.png" % i
        Image.fromarray(arr).save(os.path.join(folders[i % dirs], name),
                                  "PNG", compress_level=1)
        if (i + 1) % 500 == 0:
            print("%d/%d" % (i + 1, count), flush=True)

    print("done: %s (%d files, %d dirs)" % (out, count, dirs))


if __name__ == "__main__":
    main()
