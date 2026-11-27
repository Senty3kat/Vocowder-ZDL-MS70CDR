#!/usr/bin/env python3
"""Build Vocowder.ZDL from vocowder.c + manifest_pedal.json (6-knob vocoder).

Requires the TI C6000 compiler set in paths.py (TI_CGT_C6000 env; edit if yours
differs). Run from the repo root:  python src/custom/vocowder/build.py
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]                     # effects/<name>/build.py -> repo root
sys.path[:0] = [str(ROOT), str(ROOT / "toolchain" / "zdl")]
from paths import TI_ROOT, ZDL_OUT  # noqa: E402

from linker import LinkerConfig, link, params_from_manifest  # noqa: E402
from manifest_params import write_param_header  # noqa: E402
from custom_covers import make_cover  # noqa: E402
from parameter_display import pedal_name  # noqa: E402
from pack_covers import VOCOWDER_KNOBS  # noqa: E402
from screen_image import Canvas  # noqa: E402
sys.path.insert(0, str(HERE.parent / "xduck"))   # knob_icons.py lives in effects/xduck/
sys.path.insert(0, str(HERE))                   # ...and next to this file in a standalone repo
from knob_icons import vocowder_bands_icons  # noqa: E402

KNOB_PIC, KNOB_FRAMES = vocowder_bands_icons()

def onoff_frames(cover_rle: bytes) -> tuple[bytes, int]:
    """1.05 (user, after FrogHM5 0.17): a picture per on/off state: the cover
    (cyber cow) on, the first, plain cow (art "screen_off") off, both RLE,
    padded to one size. Returns the off picture, the size, and the index and
    off value of the first byte where they differ."""
    from screen_image import encode_zoom_rle
    art = json.loads((HERE / "vocowder_art.json").read_text(encoding="utf-8"))
    (_, bx, by), = VOCOWDER_KNOBS
    c = Canvas()
    for y, row in enumerate(art["screen_off"]):
        for x, ch in enumerate(row):
            if ch == "1" and not (bx <= x < bx + 20 and by <= y < by + 15):
                c.px(x, y)
    rle = [cover_rle, encode_zoom_rle(c)]
    n = (max(len(r) for r in rle) + 3) // 4 * 4
    on, off = (r + bytes(n - len(r)) for r in rle)
    # 1.07: only the off picture is stored; the C swaps it with the screen
    # picture. The first byte where the two differ tells which is shown.
    sig = next(i for i in range(n) if on[i] != off[i])
    if os.environ.get("VOCOWDER_PICNEG") == "1":
        return on, n, sig, off[sig]                # 1.11: off = negative of on
    if os.environ.get("VOCOWDER_PICTEST") == "1" or os.environ.get("VOCOWDER_PROBE") == "1":
        return on + off, n, sig, off[sig]          # picture test / probe: both frames
    return off, n, sig, off[sig]


CL6X = TI_ROOT / "bin" / "cl6x"

CFLAGS = [
    "--c99",
    "--opt_level=2",
    "-mv6740",
    "--abi=eabi",
    "--mem_model:data=far",
    f"--include_path={TI_ROOT}/include",
]


def write_em_png(path: Path, version: str) -> None:
    """Effect Manager sidecar, same format as XDuck's: 128x96 RGBA, black ink
    on transparent; the reference art at 128x87, the version underneath."""
    import struct
    import zlib

    w, h = 128, 96
    ink = [[0] * w for _ in range(h)]
    art = json.loads((HERE / "vocowder_art.json").read_text(encoding="utf-8"))
    for y, row in enumerate(art["em"]):
        for x, ch in enumerate(row):
            if ch == "1":
                ink[y][x] = 1
    text = version.upper()
    x0 = w - 1 - (len(text) * 4 - 1)
    for i, ch in enumerate(text):
        for ry, bits in enumerate(Canvas._FONT.get(ch, Canvas._FONT[" "])):
            for rx, b in enumerate(bits):
                if b == "1":
                    ink[90 + ry][x0 + i * 4 + rx] = 1
    raw = b"".join(
        b"\x00" + b"".join(b"\x00\x00\x00\xff" if v else b"\xff\xff\xff\x00" for v in row)
        for row in ink)

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    path.write_bytes(png)


def main() -> None:
    manifest = json.loads((HERE / "manifest_pedal.json").read_text())
    # VOCOWDER_VER / VOCOWDER_TAG: tell test variants apart in Effect Manager
    # (4-character version in the ZDL header, a tag after it on the picture)
    if os.environ.get("VOCOWDER_VER"):
        manifest["fxid_version"] = os.environ["VOCOWDER_VER"][:4]
    em_tag = os.environ.get("VOCOWDER_TAG", "")
    write_param_header(manifest, HERE / "vocowder_params.h", "VOCOWDER")

    src_c = HERE / "vocowder.c"
    out_dir = ZDL_OUT
    out_dir.mkdir(exist_ok=True)

    effect_name = manifest["effect_name"]
    audio_func = manifest["audio_func_name"]
    obj = HERE / f"{effect_name.lower()}.obj"
    out_zdl = out_dir / f"{effect_name}.ZDL"

    cover = make_cover(effect_name, [pedal_name(effect_name, p["name"]) for p in manifest["params"]])
    # VOCOWDER_PICS=0: no on/off pictures -- the safe build while the
    # picture switch freezing the pedal (1.05/1.06) is not pinned down
    pics = os.environ.get("VOCOWDER_PICS", "1") != "0"
    pic_blob, pic_n, pic_sig, pic_sig_off = onoff_frames(cover) if pics else (b"", 0, 0, 0)
    if pics:
        print(f"[vocowder] off picture: {pic_n} bytes RLE, signature byte {pic_sig} = {pic_sig_off}")
    out_dir = Path(os.environ.get("VOCOWDER_OUT", str(out_dir)))
    out_dir.mkdir(exist_ok=True)
    out_zdl = out_dir / f"{effect_name}.ZDL"
    print(f"[vocowder] compiling {src_c.name} -> {obj.name}")
    subprocess.run(
        [
            str(CL6X),
            *CFLAGS,
            f"--define=VOCOWDER_AUDIO_FUNC={audio_func}",
            *([f"--define=VOCOWDER_PIC_WORDS={pic_n // 4}",
               f"--define=VOCOWDER_PIC_SIG_IDX={pic_sig}",
               f"--define=VOCOWDER_PIC_SIG_OFF={pic_sig_off}"] if pics else []),
            *(["--define=VOCOWDER_PICNEG"] if os.environ.get("VOCOWDER_PICNEG") == "1" else []),
            *(["--define=VOCOWDER_PROBE"] if os.environ.get("VOCOWDER_PROBE") == "1" else []),
            # VOCOWDER_NO_SIZE_LABELS=1: labels compiled for speed, as in 1.06
            *(["--define=VOCOWDER_NO_SIZE_LABELS"] if os.environ.get("VOCOWDER_NO_SIZE_LABELS") == "1" else []),
            # VOCOWDER_PICTEST=1: picture switch only, the audio passes dry
            *(["--define=VOCOWDER_PICTEST"] if os.environ.get("VOCOWDER_PICTEST") == "1" else []),
            "-c",
            str(src_c),
            f"--output_file={obj}",
        ],
        check=True,
        cwd=HERE,
    )

    for junk in ("compiler.opt", "linker.cmd"):
        p = HERE / junk
        if p.exists():
            p.unlink()

    cfg = LinkerConfig(
        materialize_init=True,
        effect_name=effect_name,
        screen_image=cover,
        audio_func_name=audio_func,
        gid=manifest["gid"],
        fxid=manifest["fxid"],
        params=params_from_manifest(manifest["params"]),
        obj_path=obj,
        output_path=out_zdl,
        fxid_version=manifest.get("fxid_version", "1.00").encode("ascii"),
        flags_byte=manifest.get("flags_byte", 0x01),
        audio_nop=manifest.get("audio_nop", False),
        dsp_cost=manifest.get("dsp_cost", 20.0),
        knob_positions=VOCOWDER_KNOBS,
        allow_fewer_knob_boxes=True,
        knob_pic=KNOB_PIC,                     # band-count icon
        knob_frames=KNOB_FRAMES,
        extra_const={"Vocowder_Frames": pic_blob} if pics else None,
        pic_symbol="Vocowder_Pic" if pics else None,
        pic_min_size=pic_n,
        use_object_edit_handlers=False,
        synthesize_linesel_edit_handlers=True,
        synth_edit_start_index=2,
        knob3_blob_path="/tmp/__nonexistent__",
    )
    link(cfg)
    write_em_png(out_zdl.with_suffix(".png"), f"V{manifest['fxid_version']}" + (f" {em_tag}" if em_tag else ""))

    print(f"\n[vocowder] done -> {out_zdl}")


if __name__ == "__main__":
    main()
