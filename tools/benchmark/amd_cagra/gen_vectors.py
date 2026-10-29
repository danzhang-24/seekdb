#!/usr/bin/env python3
"""Generate a deterministic float32 vector dataset as TSV (id, vector).

The official Hidden250K set is not published, so this produces a stand-in with
the same shape and scale (250000 x 256). It exists so the baseline and the
candidate build are compared on byte-identical input; it is not a reproduction
of the official dataset.
"""
import argparse
import struct
import sys


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, required=True)
    ap.add_argument("--dim", type=int, required=True)
    ap.add_argument("--seed", type=int, required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    # xorshift64* PRNG: deterministic and fast enough in pure Python for the
    # sizes we need, and identical across machines.
    state = args.seed & 0xFFFFFFFFFFFFFFFF
    rows = args.rows
    dim = args.dim

    with open(args.out, "w", encoding="utf-8", newline="\n") as out:
        buf = []
        for i in range(rows):
            vals = []
            for _ in range(dim):
                state ^= (state >> 12) & 0xFFFFFFFFFFFFFFFF
                state ^= (state << 25) & 0xFFFFFFFFFFFFFFFF
                state ^= (state >> 27) & 0xFFFFFFFFFFFFFFFF
                x = (state * 0x2545F4914F6CDD1D) & 0xFFFFFFFFFFFFFFFF
                # map to [-1, 1) with 24 bits of mantissa kept
                vals.append((x >> 40) / 8388608.0 - 1.0)
            buf.append("%d\t[%s]" % (i, ",".join("%.6f" % v for v in vals)))
            if len(buf) >= 2000:
                out.write("\n".join(buf) + "\n")
                buf = []
        if buf:
            out.write("\n".join(buf) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
