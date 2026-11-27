"""Encode two 128x64 screens as Zoom RLE streams with the SAME token layout.

Vocowder 1.12: the pedal froze when the effect swapped its screen picture for
a different one (1.05..1.10). The likely cause is a race: the firmware decodes
the RLE picture in a task that the audio callback interrupts, and if the
picture is replaced mid-decode, the rest of the stream is read from the new
picture at the old position. Two streams with different layouts then misparse
(runs land on literals, run counts overshoot the 1024-byte screen). FrogHM5's
off picture is the runtime negative of the on picture, which keeps the layout,
so its mixes always decode.

Here both pictures get one layout: a run wherever BOTH are constant, a literal
elsewhere. Where the two disagree (one has equal neighbours, the other not),
the second picture is nudged: an equal neighbour is copied, or one pixel is
flipped. Then any mix new[:k] + old[k:] decodes to exactly 1024 bytes.

Zoom RLE: bytes are 8-pixel columns (bit0 = top) of 8 row-blocks x 128
columns; a byte followed by the same byte is a run "v v n" of n + 2 bytes
(n <= 255), anything else is a literal.
"""

from __future__ import annotations


def raw_from_rows(rows) -> list[int]:
    raw = []
    for yb in range(8):
        for x in range(128):
            v = 0
            for z in range(8):
                if rows[yb * 8 + z][x]:
                    v |= 1 << z
            raw.append(v)
    return raw


def decode(stream: bytes) -> list[int]:
    out: list[int] = []
    k = 0
    while len(out) < 1024:
        c = stream[k]
        if k + 2 < len(stream) and stream[k + 1] == c:
            out += [c] * (stream[k + 2] + 2)
            k += 3
        else:
            out.append(c)
            k += 1
    return out[:1024]


def encode_pair(a: list[int], b: list[int]) -> tuple[bytes, bytes, list[int], int]:
    """Returns (stream_a, stream_b, nudged b, number of nudged bytes)."""
    b = list(b)
    nudged = 0
    for p in range(1023):
        eq_a = a[p] == a[p + 1]
        eq_b = b[p] == b[p + 1]
        if eq_a and not eq_b:
            b[p + 1] = b[p]
            nudged += 1
        elif eq_b and not eq_a:
            b[p + 1] ^= 0x80          # bottom pixel of the 8-row block
            nudged += 1
    sa, sb = bytearray(), bytearray()
    p = 0
    while p < 1024:
        if p + 1 < 1024 and a[p] == a[p + 1]:
            n = 1
            while p + n < 1024 and n < 257 and a[p + n] == a[p] and b[p + n] == b[p]:
                n += 1
            sa += bytes([a[p], a[p], n - 2])
            sb += bytes([b[p], b[p], n - 2])
            p += n
        else:
            sa.append(a[p])
            sb.append(b[p])
            p += 1
    while len(sa) % 4:
        sa.append(0)
        sb.append(0)
    return bytes(sa), bytes(sb), b, nudged


def check_mixes(old: bytes, new: bytes) -> None:
    """Every state a decoder can meet while `old` is overwritten by `new`."""
    assert len(old) == len(new)
    for k in range(len(old) + 1):
        mix = new[:k] + old[k:]
        out = 0
        i = 0
        while out < 1024:
            c = mix[i]
            if i + 2 < len(mix) and mix[i + 1] == c:
                out += mix[i + 2] + 2
                i += 3
            else:
                out += 1
                i += 1
        assert out == 1024, (k, out)
