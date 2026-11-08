/* ------------------------------------------------------------------------
 * libfif v3.5 / fif v2.5 - FIF2 fractal image format
 *
 * libfif and fif:  Oskar Behzadpour
 * FIF2 format:     Rezar Behzad
 * Web:             www.FractalImageCompression.com
 * Contact:         oskar@fractalimagecompression.com
 *
 * Copyright (c) 2026 Oskar Behzadpour
 * ---------------------------------------------------------------------- */

#pragma once
/* FIF2 container, format version 3.3 - the one place that knows the layout.
 *
 *   [header, 144 bytes] [chunks] [thumbnail] [FIC data] [raw data]
 *
 * Every section after the header is optional: its size is in the header, and
 * a size of zero means the section is not there.  Offsets are not stored; each
 * section starts where the previous one ends.  Any section may be stored
 * compressed, and a compressed one also encrypted, which the two section bit
 * fields of the header say per section; a reader unpacks such a section before it
 * reads anything in it, and every CRC in the header is over the unpacked
 * bytes.  libfif packs as u64 unpacked size + zlib stream and cannot
 * decrypt.  website/md/fif2-spec.md is the normative description. */
#include "fif_internal.h"
#include <cstring>
#include <string>
#include <vector>

namespace fif {

constexpr uint8_t  kVerMajor = 3;
constexpr uint8_t  kVerMinor = 3;
/* The header has one size.  Nothing in it is variable - the source name, the
 * one field that was, is an ORGN chunk - so the chunks always start at 144
 * and the header CRC always sits in the last four bytes. */
constexpr size_t   kHeaderSize = 144;
/* The 16 bytes above the header CRC, held open for a later section of
 * correction data.  A writer zeroes them, a reader ignores them. */
constexpr size_t   kHeaderReserved = 16;
/* The two method bytes of the header (spec 3).  One of each per file: they
 * say how every section the bit fields mark is packed and locked.  The
 * meaning of a value belongs to the file version, so a later minor version
 * may add methods; a reader that does not know one cannot read the sections
 * concerned, and the rest of the file stays readable. */
enum : uint8_t { COMPRESS_NONE = 0, COMPRESS_DEFLATE = 1 };
enum : uint8_t { ENCRYPT_NONE = 0, ENCRYPT_BLOWFISH = 1 };

enum : uint8_t { COLOR_GRAY = 0, COLOR_RGB = 1, COLOR_RGBA = 2, COLOR_YCBCR = 3 };
enum : uint8_t { CODING_PLANAR = 1 };
/* The sections after the header, in file order.  One bit per section in each
 * of the header's two bit fields, compressed and encrypted: FIF_SECTION_* in
 * fif.h.  Bits 4 to 7 of both are free for sections a later version adds. */
enum { SEC_CHUNKS = 0, SEC_THUMBS = 1, SEC_FIC = 2, SEC_RAW = 3, SEC_COUNT = 4 };
constexpr uint8_t section_bit(int s) { return (uint8_t)(1u << s); }
/* The header counts the thumbnails in one byte. */
constexpr size_t kMaxThumbnails = 255;
/* The most a compressed section may unpack to.  The unpacked size is declared
 * in the file, so it is attacker-controlled and bounded before allocating. */
constexpr uint64_t kMaxUnpacked = (uint64_t)1 << 30;

/* Packs `in` into the stored form of a compressed section.  False when this
 * build has no zlib. */
bool zlib_pack(const uint8_t *in, size_t n, std::vector<uint8_t> &out);
/* Unpacks a compressed section.  `complete` is false for a section the
 * buffer cuts short; then whatever inflates is returned.  False on a damaged
 * stream, a size over kMaxUnpacked, a mismatch with the declared size, or a
 * build without zlib. */
bool zlib_unpack(const uint8_t *in, size_t avail, bool complete, std::vector<uint8_t> &out,
                 uint64_t *declared = nullptr);
bool have_zlib();

uint32_t crc32(const uint8_t *d, size_t n, uint32_t crc = 0);

struct Buf {
    std::vector<uint8_t> v;
    void u8(uint8_t x)   { v.push_back(x); }
    void u16(uint16_t x) { v.push_back(x & 0xFF); v.push_back(x >> 8); }
    void u32(uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((x >> (8 * i)) & 0xFF); }
    void u64(uint64_t x) { for (int i = 0; i < 8; ++i) v.push_back((x >> (8 * i)) & 0xFF); }
    void f64(double f)   { uint64_t t; std::memcpy(&t, &f, 8); u64(t); }
    /* LEB128.  Part lengths span four orders of magnitude between a flat tile
     * and a busy one, and a fixed 32-bit field spends three bytes on nothing. */
    void var(uint64_t x) {
        while (x >= 0x80) { v.push_back((uint8_t)(x | 0x80)); x >>= 7; }
        v.push_back((uint8_t)x);
    }
    void raw(const void *p, size_t n) {
        const uint8_t *b = (const uint8_t *)p;
        v.insert(v.end(), b, b + n);
    }
    size_t size() const { return v.size(); }
};

struct Rd {
    const uint8_t *p, *e;
    bool bad = false;
    bool need(size_t n) { if ((size_t)(e - p) < n) { bad = true; return false; } return true; }
    uint8_t  u8()  { if (!need(1)) return 0; return *p++; }
    uint16_t u16() { if (!need(2)) return 0; uint16_t x = (uint16_t)(p[0] | (p[1] << 8)); p += 2; return x; }
    uint32_t u32() { if (!need(4)) return 0; uint32_t x = 0; for (int i = 0; i < 4; ++i) x |= (uint32_t)p[i] << (8*i); p += 4; return x; }
    uint64_t u64() { if (!need(8)) return 0; uint64_t x = 0; for (int i = 0; i < 8; ++i) x |= (uint64_t)p[i] << (8*i); p += 8; return x; }
    float    f32() { uint32_t t = u32(); float f; std::memcpy(&f, &t, 4); return f; }
    double   f64() { uint64_t t = u64(); double f; std::memcpy(&f, &t, 8); return f; }
    /* LEB128, capped at ten groups so a crafted run of 0x80 bytes cannot walk
     * off the end or overflow the accumulator. */
    uint64_t var() {
        uint64_t x = 0;
        for (int i = 0; i < 10; ++i) {
            if (!need(1)) return 0;
            const uint8_t b = *p++;
            x |= (uint64_t)(b & 0x7F) << (7 * i);
            if (!(b & 0x80)) return x;
        }
        bad = true;
        return 0;
    }
    size_t left() const { return (size_t)(e - p); }
};

/* One section as declared in the header and as present in the buffer.  A
 * truncated file has `avail < size`; that is not an error by itself. */
struct Section {
    const uint8_t *p = nullptr;
    uint64_t size = 0;       /* declared */
    size_t avail = 0;        /* bytes actually present */
    uint32_t crc = 0;
    bool complete() const { return avail == size; }
    bool crc_ok() const { return complete() && crc32(p, avail) == crc; }
};

/* The parsed header, plus the decoder recommendations found in DECO chunks. */
struct Container {
    uint8_t major = 0, minor = 0;
    uint64_t file_size = 0;
    /* The header's two section bit fields, indexed by section_bit(). */
    uint8_t compressed = 0, encrypted = 0;
    /* How the marked sections are packed and locked, for the whole file.
     * COMPRESS_* and ENCRYPT_*; a value this build does not know costs it the
     * sections concerned, not the file. */
    uint8_t compress_method = COMPRESS_NONE, encrypt_method = ENCRYPT_NONE;
    /* Where the detail layers start, relative to the start of the unpacked
     * FIC section; everything before it is the directory and the base
     * layers. */
    uint64_t detail_offset = 0;
    char remarks[4] = {0, 0, 0, 0};
    /* channels == nplanes: the colour model decides both. */
    int width = 0, height = 0, channels = 0, nplanes = 0, cs = 0;
    uint8_t color_model = 0;
    CodecParams p;
    /* Not stored: they follow from the geometry (spec 6.2).  The plane padded
     * to whole max_block units, the tile edge actually used, and the tile
     * grid over the padded plane, in raster order. */
    int pw = 0, ph = 0, ts = 0, tiles_x = 0, tiles_y = 0;
    uint32_t tile_count = 0;
    uint32_t source_width = 0, source_height = 0;
    double source_scale = 1.0;
    /* From the ORGN source_name chunk; points into `chunks.p`, so into the
     * stream - or into store[SEC_CHUNKS] when the section is compressed. */
    const char *source_name = nullptr;
    size_t source_name_len = 0;
    uint8_t thumb_count = 0;
    uint32_t chunk_count = 0;
    /* The reserved block above the header CRC, kept so that a rewrite of the
     * header (fif_unpack) does not drop what another writer put there. */
    uint8_t reserved[kHeaderReserved] = {0};
    /* The sections as stored in the file, indexed by SEC_*. */
    Section stored[SEC_COUNT];
    /* The views every walker uses: the stored bytes, or their unpacked copy
     * in store[].  A compressed view is filled by open_section; until then,
     * and when it cannot be unpacked, it is empty.  Each carries the header's
     * CRC, which is over the unpacked bytes. */
    Section chunks, thumb, fic, raw;
    std::vector<uint8_t> store[SEC_COUNT];
    /* Per section: 1 = the view holds the content, 0 = not yet opened,
     * -1 = it cannot be read (encrypted, no zlib in this build, damaged). */
    int opened[SEC_COUNT] = {0, 0, 0, 0};
    bool truncated = false;
    /* Why parse_container refused the stream; null when it did not. */
    const char *why = nullptr;

    Container() = default;
    /* Not copyable: once parsed, its views may point into its own store. */
    Container(const Container &) = delete;
    Container &operator=(const Container &) = delete;

    bool packed(int s) const { return (compressed & section_bit(s)) != 0; }
    bool locked(int s) const { return (encrypted & section_bit(s)) != 0; }
    Section &view(int s) { return s == SEC_CHUNKS ? chunks : s == SEC_THUMBS ? thumb : s == SEC_FIC ? fic : raw; }
    const Section &view(int s) const { return const_cast<Container *>(this)->view(s); }

    /* From the ENCO chunks; -1 when the file does not carry them. */
    int quality = -1, effort = -1;
    /* From DECO chunks; 0 / negative = the stream has no opinion. */
    int rec_deblock = -1;
    int rec_iterations = 0;
    float rec_epsilon = 0.0f;
};

/* Parses and validates the header, locates every section, unpacks the chunk
 * section and reads the DECO recommendations.  False means the file is not a
 * stream this reader can decode; a short file is still true, with the missing
 * sections short. */
bool parse_container(const uint8_t *data, size_t size, Container &out);
/* Makes the view of section `s` hold its content, unpacking it if it is
 * stored compressed.  FIF_OK, or FIF_ERR_ENCRYPTED / FIF_ERR_UNSUPPORTED (no
 * zlib) / FIF_ERR_FORMAT (the packed stream is damaged). */
fif_status open_section(Container &c, int s);
/* The reason parse_container rejects `data`, as a short English phrase, or
 * null when it accepts it.  For diagnostics only. */
const char *container_problem(const uint8_t *data, size_t size);

/* Walks the chunk block.  Returns false at the end of the block or at the
 * first chunk that does not fit what is present. */
struct ChunkView {
    char type[4];
    uint8_t value_type;
    const char *name; size_t name_len;
    const uint8_t *data; uint64_t size;
    bool crc_ok;
    bool is(const char *t, const char *n) const {
        return std::memcmp(type, t, 4) == 0 && name_len == std::strlen(n) &&
               std::memcmp(name, n, name_len) == 0;
    }
};
bool next_chunk(Rd &r, ChunkView &out);

/* Reads a numeric chunk of any stored width as the requested kind.  False when
 * the value type or width does not fit. */
bool chunk_uint(const ChunkView &c, uint64_t *v);
bool chunk_float(const ChunkView &c, double *v);

/* One thumbnail as listed in the thumbnail directory.  `data` points at the
 * stored file; `avail < size` when the buffer ends inside it. */
struct ThumbView {
    char format[4];
    uint32_t width, height;         /* 0 = not stated */
    const uint8_t *data;
    uint64_t size;
    size_t avail;
    uint32_t crc;
    bool complete() const { return avail == size; }
    bool crc_ok() const { return complete() && crc32(data, avail) == crc; }
};
/* Fixed part of one directory entry.  A longer entry carries fields from a
 * later minor version, which this reader skips. */
constexpr size_t kThumbEntry = 26;
/* The number of thumbnails the directory lists, or -1 when the directory is
 * missing or malformed. */
int thumb_count(const Section &s);
/* The index-th thumbnail.  False past the last one or on a malformed
 * directory. */
bool thumb_entry(const Section &s, int index, ThumbView &out);

/* Appends one chunk to a chunk block. */
void write_chunk(Buf &b, const char type[4], uint8_t value_type, const char *name,
                 size_t name_len, const void *data, uint64_t size);

/* Everything the writer needs besides the coded tiles. */
struct ContainerInput {
    int width = 0, height = 0, nplanes = 0;
    CodecParams p;
    char remarks[4] = {0, 0, 0, 0};
    /* FIF_SECTION_* bits: the sections to store zlib-compressed. */
    uint8_t compress = 0;
    uint64_t detail_offset = 0;       /* see Container */
    uint32_t source_width = 0, source_height = 0;
    double source_scale = 1.0;
    std::vector<uint8_t> chunks;      /* complete chunk block */
    uint32_t chunk_count = 0;
    struct Thumb {
        char format[4];
        uint32_t width, height;
        std::vector<uint8_t> data;    /* the image file itself */
    };
    std::vector<Thumb> thumbs;        /* in the order they are stored */
};
/* False when compression is asked of a build without zlib, or when there are
 * more than kMaxThumbnails thumbnails. */
bool write_container(const ContainerInput &in, const std::vector<uint8_t> &fic,
                     std::vector<uint8_t> &out);

/* Writes the kHeaderSize-byte header for `c`, with these section sizes as stored and
 * these CRCs of the unpacked sections, and a fresh header CRC. */
void write_header(const Container &c, const uint64_t size[SEC_COUNT],
                  const uint32_t crc[SEC_COUNT], std::vector<uint8_t> &out);

/* In-memory JPEG, used for thumbnails.  Unsupported without libjpeg. */
fif_status jpeg_encode_mem(const fif_image &img, int quality, std::vector<uint8_t> &out);
fif_status jpeg_decode_mem(const uint8_t *d, size_t n, fif_image *out);

/* Width and height from a JPEG's frame header, without decoding it.  False
 * when the data is not a JPEG or has no frame header. */
bool jpeg_dimensions(const uint8_t *d, size_t n, int *w, int *h);

} /* namespace fif */
