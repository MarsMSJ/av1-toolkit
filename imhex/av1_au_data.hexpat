/**
 * av1_au_data.hexpat  –  ImHex pattern for av1_au_data.bin
 *
 * Drop this file in ImHex's pattern editor (or File → Load Pattern)
 * after opening an au*/av1_au_data.bin.
 *
 * Binary layout
 * ─────────────
 *  FileHeader              (16 bytes, fixed)
 *  Record[]                (variable length, walk to EOF)
 *
 * Each Record is either a Grp0ObuRecord (first tile-group constituent OBUs)
 * or a GrpNRecord (standalone tile_group_obu for groups 1..N-1).
 * Both share the same RecordHeader, distinguished by 'magic'.
 */

#pragma endian little

// ── Magic constants ──────────────────────────────────────────────────────────
// 0xA1100001  →  grp0 record  (seq hdr / frame hdr / first tile group OBUs)
// 0xA1100002  →  grpN record  (subsequent tile_group_obu, N ≥ 1)

// ── OBU type enum ────────────────────────────────────────────────────────────
enum ObuType : u8 {
    SEQUENCE_HEADER    = 1,
    TEMPORAL_DELIMITER = 2,
    FRAME_HEADER       = 3,
    TILE_GROUP         = 4,
    METADATA           = 5,
    FRAME              = 6,
    REDUNDANT_FH       = 7,
    TILE_LIST          = 8,
    PADDING            = 15,
};

// ── File header (16 bytes) ───────────────────────────────────────────────────
struct FileHeader {
    char magic[8];       // "AV1AUDAT"
    u32  au_index;       // which access unit this file describes
    u32  num_groups;     // total number of tile groups in this AU
};

// ── Per-record header (20 bytes) ─────────────────────────────────────────────
struct RecordHeader {
    u32     magic;          // 0xA1100001 = grp0,  0xA1100002 = grpN
    u32     total_size;     // size of entire record incl. this header (bytes)
    u32     au_index;
    u32     grp_index;      // 0 for grp0 OBUs, 1..N-1 for subsequent groups
    ObuType obu_type;       // AV1 OBU type of the payload
    u8      _pad[3];
};

// ── grp0 record: one per constituent OBU of the first group ─────────────────
//    (sequence_header, frame_header, first tile_group, …)
struct Grp0ObuRecord {
    RecordHeader hdr;
    u32          obu_raw_size;
    u8           obu_raw[obu_raw_size];
} [[color("4488FF")]];   // blue

// ── grpN record: one standalone tile_group_obu ───────────────────────────────
struct GrpNRecord {
    RecordHeader hdr;
    u32          obu_raw_size;
    u8           obu_raw[obu_raw_size];
} [[color("FF8844")]];   // orange

// ── Polymorphic record dispatcher ────────────────────────────────────────────
struct Record {
    u32 peek_magic = std::mem::read_unsigned($, 4);
    if (peek_magic == 0xA1100001)
        Grp0ObuRecord rec;
    else
        GrpNRecord    rec;
};

// ── Top-level layout ─────────────────────────────────────────────────────────
FileHeader file_hdr @ 0x0;
Record     records[while(!std::mem::eof())] @ 0x10;
