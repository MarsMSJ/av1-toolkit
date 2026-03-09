#pragma once

#include <cstddef>
#include <cstdint>

// OBU types — mirrors OBU_TYPE enum in av1/common/obu_util.h (libaom)
enum class ObuType : uint8_t {
    RESERVED               = 0,
    SEQUENCE_HEADER        = 1,
    TEMPORAL_DELIMITER     = 2,
    FRAME_HEADER           = 3,
    TILE_GROUP             = 4,
    METADATA               = 5,
    FRAME                  = 6,
    REDUNDANT_FRAME_HEADER = 7,
    TILE_LIST              = 8,
    PADDING                = 15,
};

enum class Av1TileDecodeUnitType {
    SessionStart,   // sequence header + frame header + tile group(s)
    NewFrame,       // frame header + tile group(s)
    TileGroupCont,  // tile group(s) only
    Error
};

class Av1TileDecodeUnit {
public:
    explicit Av1TileDecodeUnit(const uint8_t* data, size_t size);

    Av1TileDecodeUnitType type()     const { return type_; }
    bool                  is_valid() const { return type_ != Av1TileDecodeUnitType::Error; }

private:
    Av1TileDecodeUnitType type_;
};

inline Av1TileDecodeUnit::Av1TileDecodeUnit(const uint8_t* data, size_t size) {
    type_ = Av1TileDecodeUnitType::Error;
    if (!data || size == 0) return;

    bool has_sequence_header = false;
    bool has_frame_header    = false;
    bool has_tile_group      = false;
    size_t pos = 0;

    while (pos < size) {
        // OBU header — mirrors aom_read_obu_header() in av1/common/obu_util.c
        uint8_t b = data[pos++];
        ObuType obu_type = static_cast<ObuType>((b >> 3) & 0xF);
        bool ext      = (b & 4) != 0;
        bool has_size = (b & 2) != 0;

        switch (obu_type) {
            case ObuType::SEQUENCE_HEADER: has_sequence_header = true; break;
            case ObuType::FRAME_HEADER:    has_frame_header    = true; break;
            case ObuType::TILE_GROUP:      has_tile_group      = true; break;
            case ObuType::FRAME:           has_frame_header    = true; has_tile_group = true; break;  // combined OBU
            default: break;
        }

        // Extension header skip — mirrors aom_read_obu_header_extension()
        // in av1/common/obu_util.c
        if (ext && pos < size) pos++;
        if (!has_size || pos >= size) break;

        // LEB128 size — mirrors aom_uleb_decode() in aom/aom_integer.c
        // kMaximumLeb128Size = 8 (aom/aom_integer.h)
        uint64_t obu_size = 0;
        int shift = 0;
        for (int i = 0; i < 8 && pos < size; i++) {
            uint8_t v = data[pos++];
            obu_size |= uint64_t(v & 0x7F) << shift;
            if (!(v & 0x80)) break;
            shift += 7;
        }

        if (obu_size > size - pos) return;  // malformed
        pos += static_cast<size_t>(obu_size);
    }

    if (!has_tile_group)                          return;
    if (has_sequence_header && has_frame_header)  { type_ = Av1TileDecodeUnitType::SessionStart;  return; }
    if (has_frame_header)                         { type_ = Av1TileDecodeUnitType::NewFrame;       return; }
    type_ = Av1TileDecodeUnitType::TileGroupCont;
}