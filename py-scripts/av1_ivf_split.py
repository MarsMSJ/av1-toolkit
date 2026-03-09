#!/usr/bin/env python3
"""
av1_ivf_split.py  –  Split an AV1 IVF into per-AU / per-tile-group bins.

Output layout
─────────────
  auN/
    av1_auN_grp0.bin   – sequence_header_obu(s) + frame_header_obu + first tile_group_obu  (raw)
    av1_auN_grp1.bin   – second tile_group_obu                                              (raw)
    …
    av1_auN_grpM.bin   – last  tile_group_obu                                               (raw)
    av1_au_data.bin    – ImHex-friendly container (see C++ structs below)

Usage:  python av1_ivf_split.py <input.ivf> [--max-au N]
"""

import struct, sys, os, argparse
from pathlib import Path

# ── OBU type constants ──────────────────────────────────────────────────────
OBU_SEQUENCE_HEADER  = 1
OBU_TEMPORAL_DELIM   = 2
OBU_FRAME_HEADER     = 3
OBU_TILE_GROUP       = 4
OBU_METADATA         = 5
OBU_FRAME            = 6   # combined frame_header + tile_group
OBU_REDUNDANT_FH     = 7
OBU_TILE_LIST        = 8
OBU_PADDING          = 15

OBU_NAMES = {
    1:"SEQUENCE_HEADER", 2:"TEMPORAL_DELIMITER", 3:"FRAME_HEADER",
    4:"TILE_GROUP", 5:"METADATA", 6:"FRAME", 7:"REDUNDANT_FH",
    8:"TILE_LIST", 15:"PADDING",
}

# ── LEB128 helper ───────────────────────────────────────────────────────────
def read_leb128(data: bytes, pos: int):
    value, shift = 0, 0
    while pos < len(data):
        b = data[pos]; pos += 1
        value |= (b & 0x7F) << shift
        shift += 7
        if not (b & 0x80):
            break
    return value, pos

# ── IVF reader ──────────────────────────────────────────────────────────────
IVF_FILE_HDR = 32
IVF_FRAME_HDR = 12

def parse_ivf(path: str):
    """Yield (pts, frame_bytes) for every IVF frame."""
    with open(path, "rb") as f:
        hdr = f.read(IVF_FILE_HDR)
    if hdr[:4] != b'DKIF':
        raise ValueError("Not a valid IVF file")
    with open(path, "rb") as f:
        f.seek(IVF_FILE_HDR)
        while True:
            fhdr = f.read(IVF_FRAME_HDR)
            if len(fhdr) < IVF_FRAME_HDR:
                break
            size = struct.unpack_from("<I", fhdr, 0)[0]
            pts  = struct.unpack_from("<Q", fhdr, 4)[0]
            data = f.read(size)
            if len(data) < size:
                break
            yield pts, data

# ── OBU parser ──────────────────────────────────────────────────────────────
def parse_obus(data: bytes):
    """
    Yield (obu_type, has_size_field, header_bytes, payload_bytes, raw_bytes)
    raw_bytes = header_bytes + size_field_bytes + payload_bytes  (the complete OBU on the wire)
    """
    pos = 0
    while pos < len(data):
        start = pos
        b = data[pos]; pos += 1
        forbidden      = (b >> 7) & 1
        obu_type       = (b >> 3) & 0x0F
        extension_flag = (b >> 2) & 1
        has_size_field = (b >> 1) & 1
        reserved       = b & 1

        header_end = pos
        if extension_flag:
            pos += 1          # extension byte
            header_end = pos

        header_bytes = data[start:header_end]

        if has_size_field:
            size_start = pos
            obu_size, pos = read_leb128(data, pos)
            size_bytes = data[size_start:pos]
        else:
            obu_size   = len(data) - pos
            size_bytes = b''

        payload_bytes = data[pos:pos+obu_size]
        pos += obu_size

        raw = data[start:pos]
        yield obu_type, has_size_field, header_bytes, payload_bytes, raw

# ── ImHex data builder ──────────────────────────────────────────────────────
"""
C++ structs for ImHex  (paste into the Pattern Editor):

// ─── common header every record starts with ───────────────────────────────
struct RecordHeader {
    u32 magic;          // 0xAV10_XXXX where XXXX = record kind
    u32 total_size;     // total bytes of this record incl. this header
    u32 au_index;
    u32 grp_index;      // 0xFFFFFFFF for grp0 (multi-OBU record)
    u8  obu_type;
    u8  _pad[3];
};

// ─── grp0: sequence_header(s) + frame_header + first tile_group ───────────
//     One record per OBU inside grp0.
struct Grp0ObuRecord {
    RecordHeader hdr;   // grp_index == 0, magic == 0xAV100001
    u32          obu_raw_size;
    u8           obu_raw[obu_raw_size];
};

// ─── grpN (N>=1): single tile_group_obu ──────────────────────────────────
struct GrpNRecord {
    RecordHeader hdr;   // magic == 0xAV100002
    u32          obu_raw_size;
    u8           obu_raw[obu_raw_size];
};

// Top-level layout of av1_au_data.bin:
//   FileHeader   (16 bytes)
//   RecordHeader/Grp0ObuRecord/GrpNRecord  …  (variable count)

struct FileHeader {
    char magic[8];      // "AV1AUDAT"
    u32  au_index;
    u32  num_groups;
};
"""

MAGIC_FILE    = b'AV1AUDAT'
MAGIC_GRP0    = 0xA1100001   # first group record
MAGIC_GRPN    = 0xA1100002   # subsequent tile group records

def build_record(magic: int, au_idx: int, grp_idx: int, obu_type: int, raw: bytes) -> bytes:
    # RecordHeader: magic(4) + total_size(4) + au_index(4) + grp_index(4) + obu_type(1) + pad(3)
    HDR_SIZE = 4+4+4+4+1+3
    obu_raw_size_field = struct.pack("<I", len(raw))
    total = HDR_SIZE + 4 + len(raw)
    hdr = struct.pack("<IIIIBBBB",
        magic, total, au_idx,
        grp_idx & 0xFFFFFFFF,
        obu_type, 0, 0, 0)
    return hdr + obu_raw_size_field + raw

def build_au_data_bin(au_idx: int, num_groups: int, grp_records: list) -> bytes:
    # FileHeader
    fh = MAGIC_FILE + struct.pack("<II", au_idx, num_groups)
    return fh + b''.join(grp_records)

# ── Main ────────────────────────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser(description="Split AV1 IVF into per-AU tile-group bins")
    ap.add_argument("ivf", help="Input .ivf file")
    ap.add_argument("--max-au", type=int, default=None, help="Stop after N access units")
    ap.add_argument("--out-dir", default=".", help="Root output directory (default: .)")
    args = ap.parse_args()

    out_root = Path(args.out_dir)
    out_root.mkdir(parents=True, exist_ok=True)

    for au_idx, (pts, frame_data) in enumerate(parse_ivf(args.ivf)):
        if args.max_au is not None and au_idx >= args.max_au:
            break

        au_dir = out_root / f"au{au_idx}"
        au_dir.mkdir(exist_ok=True)

        # Collect OBUs
        obus = list(parse_obus(frame_data))

        # Split into groups:
        #   grp0 = all non-tile-group OBUs leading up to + including the first TILE_GROUP / FRAME
        #   grpN = each subsequent TILE_GROUP / FRAME OBU
        grp0_raws   = []   # list of raw OBU bytes for grp0
        grp0_types  = []
        tile_groups = []   # list of (obu_type, raw) for grp1..grpN

        first_tg_seen = False
        for obu_type, _, hdr_b, pay_b, raw in obus:
            is_tg = obu_type in (OBU_TILE_GROUP, OBU_FRAME)
            if not first_tg_seen:
                grp0_raws.append(raw)
                grp0_types.append(obu_type)
                if is_tg:
                    first_tg_seen = True
            else:
                if is_tg or obu_type not in (OBU_TEMPORAL_DELIM,):
                    tile_groups.append((obu_type, raw))

        num_groups = 1 + len(tile_groups)

        # ── Write grp0.bin ──────────────────────────────────────────────
        grp0_bin = b''.join(grp0_raws)
        (au_dir / f"av1_au{au_idx}_grp0.bin").write_bytes(grp0_bin)

        # ── Write grpN.bin for N>=1 ─────────────────────────────────────
        for g_idx, (obu_type, raw) in enumerate(tile_groups, 1):
            (au_dir / f"av1_au{au_idx}_grp{g_idx}.bin").write_bytes(raw)

        # ── Build av1_au_data.bin ───────────────────────────────────────
        data_records = []

        # grp0: one record per OBU
        for obu_type, raw in zip(grp0_types, grp0_raws):
            rec = build_record(MAGIC_GRP0, au_idx, 0, obu_type, raw)
            data_records.append(rec)

        # grpN: one record each
        for g_idx, (obu_type, raw) in enumerate(tile_groups, 1):
            rec = build_record(MAGIC_GRPN, au_idx, g_idx, obu_type, raw)
            data_records.append(rec)

        au_data = build_au_data_bin(au_idx, num_groups, data_records)
        (au_dir / "av1_au_data.bin").write_bytes(au_data)

        # ── Console summary ─────────────────────────────────────────────
        print(f"AU {au_idx:4d}  pts={pts:8d}  groups={num_groups}  "
              f"grp0_obus={len(grp0_raws)}  frame_sz={len(frame_data)}")

    print("Done.")

if __name__ == "__main__":
    main()
