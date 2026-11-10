/* ------------------------------------------------------------------------
 * libfif v3.5 / fif v2.5 - FIF2 fractal image format
 *
 * libfif and fif:  Oskar Behzadpour
 * FIF2 format:     Rezar Behzad
 * Web:             www.FractalImageCompression.com
 * Contact:         oskar@fractalimagecompression.com
 *
 * Copyright (c) 2026 Oskar Behzadpour
 * SPDX-License-Identifier: MIT
 * ---------------------------------------------------------------------- */

/* FIF2 container 3.1: writing and parsing the header and the sections, and the
 * public accessors for chunks, thumbnails and raw data.  The coded tiles
 * themselves (the FIC section) are the business of encode/serialize/decode;
 * this file only knows where they start and how long they are. */
#include "format.h"
#include "codec.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#ifdef FIF_HAVE_ZLIB
#  include <zlib.h>
#endif

namespace fif {
namespace {

struct Crc32Table {
    uint32_t t[256];
    Crc32Table() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
    }
};
const Crc32Table kCrc32;

/* A section is [offset, offset + declared) of which only the part inside the
 * buffer is present.  Checked term by term so no sum of attacker-controlled
 * sizes can wrap back under the buffer length. */
bool place(const uint8_t *data, size_t size, uint64_t &off, uint64_t declared,
           uint32_t crc, Section &s) {
    s.size = declared;
    s.crc = crc;
    if (declared == 0) { s.p = nullptr; s.avail = 0; return true; }
    if (off > size) { s.p = data + size; s.avail = 0; }
    else {
        s.p = data + off;
        s.avail = (size_t)std::min<uint64_t>(declared, size - off);
    }
    if (declared > UINT64_MAX - off) return false;
    off += declared;
    return true;
}

} /* namespace */

uint32_t crc32(const uint8_t *d, size_t n, uint32_t crc) {
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = kCrc32.t[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ----------------------------------------------------------------- chunks */

void write_chunk(Buf &b, const char type[4], uint8_t value_type, const char *name,
                 size_t name_len, const void *data, uint64_t size) {
    const size_t start = b.size();
    b.raw(type, 4);
    b.u8(value_type);
    b.u16((uint16_t)name_len);
    if (name_len) b.raw(name, name_len);
    b.u64(size);
    if (size) b.raw(data, (size_t)size);
    /* Over the whole entry, type and name included: a flipped bit in the name
     * turns one value into another just as surely as one in the data. */
    b.u32(crc32(b.v.data() + start, b.size() - start));
}

bool next_chunk(Rd &r, ChunkView &c) {
    if (r.left() == 0) return false;
    const uint8_t *start = r.p;
    if (!r.need(4 + 1 + 2)) return false;
    std::memcpy(c.type, r.p, 4); r.p += 4;
    c.value_type = r.u8();
    c.name_len = r.u16();
    if (!r.need(c.name_len)) return false;
    c.name = (const char *)r.p; r.p += c.name_len;
    c.size = r.u64();
    if (r.bad) return false;
    /* Bound the length before adding to it: a value near 2^64 would otherwise
     * wrap `size + 4` to something need() waves through. */
    if (c.size > r.left() || !r.need((size_t)c.size + 4)) return false;
    c.data = r.p; r.p += c.size;
    const uint32_t stored = r.u32();
    c.crc_ok = crc32(start, (size_t)(r.p - 4 - start)) == stored;
    return true;
}

bool chunk_uint(const ChunkView &c, uint64_t *v) {
    if (c.value_type != FIF_VALUE_UINT && c.value_type != FIF_VALUE_INT) return false;
    if (c.size != 1 && c.size != 2 && c.size != 4 && c.size != 8) return false;
    uint64_t x = 0;
    for (uint64_t i = 0; i < c.size; ++i) x |= (uint64_t)c.data[i] << (8 * i);
    if (c.value_type == FIF_VALUE_INT && (c.data[c.size - 1] & 0x80)) return false;
    *v = x;
    return true;
}

bool chunk_float(const ChunkView &c, double *v) {
    if (c.value_type == FIF_VALUE_FLOAT) {
        Rd r{c.data, c.data + c.size};
        if (c.size == 4) { *v = r.f32(); return true; }
        if (c.size == 8) { *v = r.f64(); return true; }
        return false;
    }
    uint64_t u;
    if (!chunk_uint(c, &u)) return false;
    *v = (double)u;
    return true;
}

/* ------------------------------------------------------------------- zlib */

#ifdef FIF_HAVE_ZLIB
bool have_zlib() { return true; }

bool zlib_pack(const uint8_t *in, size_t n, std::vector<uint8_t> &out) {
    uLongf cap = compressBound((uLong)n);
    out.assign(8 + (size_t)cap, 0);
    for (int i = 0; i < 8; ++i) out[(size_t)i] = (uint8_t)((uint64_t)n >> (8 * i));
    if (compress2(out.data() + 8, &cap, in, (uLong)n, Z_BEST_COMPRESSION) != Z_OK) return false;
    out.resize(8 + (size_t)cap);
    return true;
}

bool zlib_unpack(const uint8_t *in, size_t avail, bool complete, std::vector<uint8_t> &out,
                 uint64_t *declared) {
    out.clear();
    if (declared) *declared = 0;
    if (avail < 8) return !complete;
    uint64_t want = 0;
    for (int i = 0; i < 8; ++i) want |= (uint64_t)in[i] << (8 * i);
    if (want > kMaxUnpacked) return false;
    if (declared) *declared = want;
    out.resize((size_t)want);
    z_stream z{};
    if (inflateInit(&z) != Z_OK) return false;
    z.next_in = const_cast<Bytef *>(in + 8);
    z.avail_in = (uInt)(avail - 8);
    z.next_out = out.data();
    z.avail_out = (uInt)want;
    const int r = inflate(&z, Z_FINISH);
    const size_t got = (size_t)z.total_out;
    inflateEnd(&z);
    if (r == Z_STREAM_END) {
        out.resize(got);
        return got == want;
    }
    /* A cut-off section yields what arrived; a complete one that does not
     * inflate to its declared size is damaged. */
    if (!complete && (r == Z_BUF_ERROR || r == Z_OK)) { out.resize(got); return true; }
    out.clear();
    return false;
}
#else
bool have_zlib() { return false; }
bool zlib_pack(const uint8_t *, size_t, std::vector<uint8_t> &) { return false; }
bool zlib_unpack(const uint8_t *, size_t, bool, std::vector<uint8_t> &out, uint64_t *declared) {
    out.clear();
    if (declared) *declared = 0;
    return false;
}
#endif

/* ----------------------------------------------------------------- header */

void write_header(const Container &c, const uint64_t size[SEC_COUNT],
                  const uint32_t crc[SEC_COUNT], std::vector<uint8_t> &out) {
    const CodecParams &p = c.p;
    const bool grey = c.nplanes == 1;
    Buf h;
    h.raw("FIF2", 4);
    h.u8(kVerMajor);
    h.u8(kVerMinor);
    h.u8(c.compressed);
    h.u8(c.encrypted);
    h.u64(kHeaderSize + size[SEC_CHUNKS] + size[SEC_THUMBS] + size[SEC_FIC] + size[SEC_RAW]);
    h.u32((uint32_t)c.width);
    h.u32((uint32_t)c.height);
    h.u8(8);                                         /* bits per channel */
    h.u8(grey ? COLOR_GRAY : COLOR_YCBCR);
    /* Both describe the colour planes, so a grey file has neither. */
    h.u8(grey ? 0 : (uint8_t)p.subsampling);
    h.u8(grey ? 0 : (uint8_t)p.chroma_mode);
    h.u8((uint8_t)p.dc_bits);
    h.u8((uint8_t)p.s_bits);
    h.u8((uint8_t)p.domain_shift);
    h.u8((uint8_t)(p.planar_mode ? CODING_PLANAR : 0));
    h.u16((uint16_t)p.min_block);
    h.u16((uint16_t)p.max_block);
    h.u32((uint32_t)p.tile_size);
    h.u64(size[SEC_FIC] ? c.detail_offset : 0);
    h.u32(c.source_width);
    h.u32(c.source_height);
    h.f64(c.source_scale);
    h.u8(c.thumb_count);
    h.u8(c.compress_method);
    h.u8(c.encrypt_method);
    h.u8(0);                                         /* reserved, keeps the u32s aligned */
    h.raw(c.remarks, 4);
    h.u32(c.chunk_count);
    h.u32(size[SEC_CHUNKS] ? crc[SEC_CHUNKS] : 0);
    h.u64(size[SEC_CHUNKS]);
    h.u64(size[SEC_THUMBS]);
    h.u64(size[SEC_FIC]);
    h.u64(size[SEC_RAW]);
    h.u32(size[SEC_THUMBS] ? crc[SEC_THUMBS] : 0);
    h.u32(size[SEC_FIC] ? crc[SEC_FIC] : 0);
    h.u32(size[SEC_RAW] ? crc[SEC_RAW] : 0);
    h.raw(c.reserved, kHeaderReserved);
    h.u32(crc32(h.v.data(), h.size()));
    out.insert(out.end(), h.v.begin(), h.v.end());
}

bool write_container(const ContainerInput &in, const std::vector<uint8_t> &fic,
                     std::vector<uint8_t> &out) {
    /* Thumbnail section: count, the directory, then the files in directory
     * order.  Empty when there are none. */
    std::vector<uint8_t> thumb;
    if (in.thumbs.size() > kMaxThumbnails) return false;
    if (!in.thumbs.empty()) {
        Buf t;
        t.u32((uint32_t)in.thumbs.size());
        for (const auto &e : in.thumbs) {
            t.u16((uint16_t)kThumbEntry);
            t.raw(e.format, 4);
            t.u32(e.width);
            t.u32(e.height);
            t.u64(e.data.size());
            t.u32(crc32(e.data.data(), e.data.size()));
        }
        for (const auto &e : in.thumbs) t.raw(e.data.data(), e.data.size());
        thumb = std::move(t.v);
    }

    Container c;
    c.width = in.width; c.height = in.height; c.nplanes = in.nplanes;
    c.p = in.p;
    c.detail_offset = in.detail_offset;
    c.source_width = in.source_width; c.source_height = in.source_height;
    c.source_scale = in.source_scale;
    c.thumb_count = (uint8_t)in.thumbs.size();
    std::memcpy(c.remarks, in.remarks, 4);
    c.chunk_count = in.chunk_count;

    /* The CRCs are over the sections as a reader sees them once unpacked, so
     * they are taken before packing.  A section is packed only when asked
     * and present: the bit on an empty section would promise a stream that
     * is not there. */
    const std::vector<uint8_t> *plain[SEC_COUNT] = {&in.chunks, &thumb, &fic, nullptr};
    std::vector<uint8_t> packed[SEC_COUNT];
    const std::vector<uint8_t> *body[SEC_COUNT] = {};
    uint64_t size[SEC_COUNT] = {};
    uint32_t crc[SEC_COUNT] = {};
    for (int s = 0; s < SEC_COUNT; ++s) {
        body[s] = plain[s];
        if (!plain[s] || plain[s]->empty()) continue;
        crc[s] = crc32(plain[s]->data(), plain[s]->size());
        if (in.compress & section_bit(s)) {
            if (!zlib_pack(plain[s]->data(), plain[s]->size(), packed[s])) return false;
            body[s] = &packed[s];
            c.compressed |= section_bit(s);
            /* One method for the file: everything libfif packs is deflate. */
            c.compress_method = COMPRESS_DEFLATE;
        }
        size[s] = body[s]->size();
    }

    out.clear();
    out.reserve(kHeaderSize + size[SEC_CHUNKS] + size[SEC_THUMBS] + size[SEC_FIC]);
    write_header(c, size, crc, out);
    for (int s = 0; s < SEC_COUNT; ++s)
        if (size[s]) out.insert(out.end(), body[s]->begin(), body[s]->end());
    return true;
}

bool parse_container(const uint8_t *data, size_t size, Container &out) {
    /* Every rejection names its reason in out.why; the CLI turns that into a
     * message, the library itself only ever reports FIF_ERR_FORMAT. */
    auto fail = [&](const char *why) { out.why = why; return false; };
    if (!data || size < 4 || std::memcmp(data, "FIF2", 4) != 0)
        return fail("not a FIF2 file (the first four bytes are not \"FIF2\")");
    if (size < 6) return fail("file ends inside the header");
    Rd r{data, data + size};
    r.p += 4;
    out.major = r.u8();
    out.minor = r.u8();
    if (out.major != kVerMajor) return fail("unsupported container major version");
    /* 3.0 to 3.2 had other header layouts: 3.0 and 3.1 only ever came from
     * development builds, and 3.2 has three padding bytes where 3.3 names the
     * compression and encryption method, so a packed 3.2 file would read as
     * packed by no method at all. */
    if (out.minor < kVerMinor) return fail("container version 3.2 and older are earlier layouts");
    if (size < kHeaderSize) return fail("file ends inside the header");
    {
        Rd c{data + kHeaderSize - 4, data + kHeaderSize};
        if (crc32(data, kHeaderSize - 4) != c.u32())
            return fail("header checksum mismatch - the header is damaged");
    }
    r.e = data + kHeaderSize - 4;              /* nothing below reads the CRC */

    out.compressed = r.u8();
    out.encrypted = r.u8();
    out.file_size = r.u64();
    out.width  = (int)r.u32();
    out.height = (int)r.u32();
    const uint8_t bpc = r.u8();
    out.color_model = r.u8();
    CodecParams &p = out.p;
    p.subsampling  = r.u8();
    p.chroma_mode  = r.u8();
    p.dc_bits      = r.u8();
    p.s_bits       = r.u8();
    p.domain_shift = r.u8();
    const uint8_t coding = r.u8();
    p.planar_mode = (coding & CODING_PLANAR) != 0;
    p.min_block    = r.u16();
    p.max_block    = r.u16();
    const uint32_t tile_size = r.u32();
    out.detail_offset = r.u64();
    out.source_width = r.u32();
    out.source_height = r.u32();
    out.source_scale = r.f64();
    out.thumb_count = r.u8();
    out.compress_method = r.u8();
    out.encrypt_method = r.u8();
    r.u8();                                    /* reserved */
    if (r.need(4)) { std::memcpy(out.remarks, r.p, 4); r.p += 4; }
    out.chunk_count = r.u32();
    uint32_t crc[SEC_COUNT];
    uint64_t sz[SEC_COUNT];
    crc[SEC_CHUNKS] = r.u32();
    for (int s = 0; s < SEC_COUNT; ++s) sz[s] = r.u64();
    crc[SEC_THUMBS] = r.u32();
    crc[SEC_FIC]    = r.u32();
    crc[SEC_RAW]    = r.u32();
    if (r.need(kHeaderReserved)) { std::memcpy(out.reserved, r.p, kHeaderReserved); r.p += kHeaderReserved; }
    if (r.bad) return fail("header is shorter than its fields");

    for (int s = 0; s < SEC_COUNT; ++s)
        if (out.locked(s) && !out.packed(s))
            return fail("a section is marked encrypted but not compressed");
    /* A file that marks sections must name the method it marked them with.
     * An unknown method is not a reason to refuse the file: it costs the
     * reader those sections (open_section), nothing more. */
    if (out.compressed && out.compress_method == COMPRESS_NONE)
        return fail("sections are marked compressed but the header names no compression method");
    if (out.encrypted && out.encrypt_method == ENCRYPT_NONE)
        return fail("sections are marked encrypted but the header names no encryption method");
    if (bpc != 8) return fail("bit depth other than 8 bits per channel");
    if (out.width <= 0 || out.height <= 0) return fail("image width or height is zero");
    if (out.width > (1 << 20) || out.height > (1 << 20))
        return fail("image width or height above 1048576");
    if (out.color_model != COLOR_GRAY && out.color_model != COLOR_YCBCR)
        return fail("unknown colour model");
    out.nplanes = (out.color_model == COLOR_GRAY) ? 1 : 3;
    out.channels = out.nplanes;
    if (out.nplanes == 1) {
        /* Grey has no colour planes; the two fields are ignored. */
        p.subsampling = FIF_SUBSAMPLING_400;
        p.chroma_mode = FIF_CHROMA_SHARED;
    } else if (p.subsampling != FIF_SUBSAMPLING_444 && p.subsampling != FIF_SUBSAMPLING_420) {
        return fail("unknown chroma subsampling");
    }

    if (!valid_block_sizes(p.min_block, p.max_block)) return fail("invalid block sizes");
    /* These size the context arrays; a wider field than the tables hold would
     * index past them, so they are a hard limit rather than a sanity check. */
    if (p.dc_bits < 1 || p.dc_bits > Contexts::MAX_DC_BITS) return fail("dc_bits out of range");
    if (p.s_bits < 1 || p.s_bits > Contexts::MAX_S_BITS) return fail("s_bits out of range");
    if (p.chroma_mode < 0 || p.chroma_mode > FIF_CHROMA_FROM_LUMA) return fail("unknown chroma mode");
    if (tile_size == 0) return fail("tile size is zero");
    /* A tile wider than the picture is one tile across; capping the value
     * keeps the rounding below in range without changing the geometry. */
    p.tile_size = (int)std::min<uint32_t>(tile_size, 1u << 22);
    if (p.domain_shift < 0 || p.domain_shift > 4) return fail("domain_shift out of range");
    if (!(out.source_scale > 0.0) || !std::isfinite(out.source_scale)) out.source_scale = 1.0;
    if (out.source_width == 0 || out.source_height == 0) {
        out.source_width = (uint32_t)out.width;
        out.source_height = (uint32_t)out.height;
    }
    out.cs = (out.nplanes == 1) ? 0 : chroma_shift(p);
    {
        const int align = p.max_block << out.cs;
        out.pw = align_up(out.width, align);
        out.ph = align_up(out.height, align);
        out.ts = effective_tile_size(p.tile_size, align);
        out.tiles_x = (out.pw + out.ts - 1) / out.ts;
        out.tiles_y = (out.ph + out.ts - 1) / out.ts;
        out.tile_count = (uint32_t)out.tiles_x * (uint32_t)out.tiles_y;
    }
    /* Two bytes of directory per tile is the floor - one varint each for the
     * base and detail length - so a section that cannot hold that much is a
     * lie, and the detail layers cannot start inside the directory.  For a
     * packed section the same holds for its unpacked size; decode checks it
     * there. */
    if (!out.packed(SEC_FIC)) {
        if ((uint64_t)out.tile_count * 2 > sz[SEC_FIC])
            return fail("tile count does not fit the size of the image data");
        if (out.detail_offset > sz[SEC_FIC] ||
            (sz[SEC_FIC] && out.detail_offset < (uint64_t)out.tile_count * 2))
            return fail("detail offset lies outside the image data");
    }

    uint64_t off = kHeaderSize;
    for (int s = 0; s < SEC_COUNT; ++s)
        if (!place(data, size, off, sz[s], crc[s], out.stored[s]))
            return fail("section sizes add up to more than any file can hold");
    out.truncated = size < off;
    /* Plain sections are their own view; packed ones wait for open_section,
     * except the chunks, which are needed right here. */
    for (int s = 0; s < SEC_COUNT; ++s) {
        Section &v = out.view(s);
        if (out.packed(s)) { v = Section{}; v.crc = crc[s]; }
        else { v = out.stored[s]; out.opened[s] = 1; }
    }

    /* A chunk section that will not unpack costs the recommendations and the
     * source name, never the picture. */
    open_section(out, SEC_CHUNKS);

    /* Encoder settings and decoder recommendations.  A damaged chunk is
     * skipped rather than fatal: nothing in a chunk is needed to decode, so
     * losing one costs a default. */
    if (out.chunks.avail) {
        Rd c{out.chunks.p, out.chunks.p + out.chunks.avail};
        ChunkView v;
        while (next_chunk(c, v)) {
            if (!v.crc_ok) continue;
            uint64_t u; double d;
            if (v.is("ORGN", "source_name") && v.value_type == FIF_VALUE_TEXT) {
                out.source_name = (const char *)v.data;
                out.source_name_len = (size_t)v.size;
            }
            else if (v.is("ENCO", "quality") && chunk_uint(v, &u)) out.quality = (int)u;
            else if (v.is("ENCO", "effort") && chunk_uint(v, &u)) out.effort = (int)u;
            else if (v.is("DECO", "deblock") && chunk_uint(v, &u)) out.rec_deblock = (int)std::min<uint64_t>(u, 1000);
            else if (v.is("DECO", "iterations") && chunk_uint(v, &u)) out.rec_iterations = (int)std::min<uint64_t>(u, 10000);
            else if (v.is("DECO", "epsilon") && chunk_float(v, &d) && d > 0 && d < 256) out.rec_epsilon = (float)d;
        }
    }
    return true;
}

fif_status open_section(Container &c, int s) {
    if (c.opened[s] > 0) return FIF_OK;
    const Section &st = c.stored[s];
    Section &v = c.view(s);
    auto refuse = [&](fif_status why) { c.opened[s] = -1; return why; };
    if (c.opened[s] < 0) {
        if (c.locked(s)) return FIF_ERR_ENCRYPTED;
        return have_zlib() ? FIF_ERR_FORMAT : FIF_ERR_UNSUPPORTED;
    }
    if (st.size == 0) { c.opened[s] = 1; return FIF_OK; }
    if (c.locked(s)) return refuse(FIF_ERR_ENCRYPTED);
    /* deflate is the only method this version packs and unpacks. */
    if (c.compress_method != COMPRESS_DEFLATE) return refuse(FIF_ERR_UNSUPPORTED);
    if (!have_zlib()) return refuse(FIF_ERR_UNSUPPORTED);
    uint64_t declared = 0;
    if (!zlib_unpack(st.p, st.avail, st.complete(), c.store[s], &declared))
        return refuse(FIF_ERR_FORMAT);
    v.p = c.store[s].data();
    v.avail = c.store[s].size();
    /* A cut-off section unpacks to a prefix of what it declared; a complete
     * one to exactly that. */
    v.size = st.complete() ? v.avail : std::max<uint64_t>(declared, v.avail);
    c.opened[s] = 1;
    return FIF_OK;
}

/* Walks the directory.  With `want` >= 0 fills `out` for that entry; the
 * return value is the entry count, or -1 on a malformed or cut directory. */
static int walk_thumbs(const Section &s, int want, ThumbView *out) {
    if (s.size == 0) return 0;
    Rd r{s.p, s.p + s.avail};
    const uint32_t count = r.u32();
    if (r.bad || count > s.size / kThumbEntry) return -1;
    uint64_t before = 0;                     /* payload bytes ahead of `want` */
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *entry = r.p;
        const uint16_t len = r.u16();
        if (r.bad || len < kThumbEntry || !r.need(len - 2)) return -1;
        Rd e{entry + 2, entry + len};
        char format[4];
        std::memcpy(format, e.p, 4); e.p += 4;
        const uint32_t w = e.u32(), h = e.u32();
        const uint64_t n = e.u64();
        const uint32_t crc = e.u32();
        r.p = entry + len;
        if (n > s.size) return -1;
        if ((int64_t)i < want) before += n;
        if ((int64_t)i == want && out) {
            std::memcpy(out->format, format, 4);
            out->width = w; out->height = h;
            out->size = n; out->crc = crc;
        }
    }
    if (want >= 0 && out) {
        /* Payloads start after the directory, in directory order. */
        const uint64_t dir = (uint64_t)(r.p - s.p);
        if (before > s.size - dir || out->size > s.size - dir - before) return -1;
        const uint64_t at = dir + before;
        out->data = s.p + at;
        out->avail = at >= s.avail ? 0 : (size_t)std::min<uint64_t>(out->size, s.avail - at);
    }
    return (int)count;
}

int thumb_count(const Section &s) { return walk_thumbs(s, -1, nullptr); }

const char *container_problem(const uint8_t *data, size_t size) {
    Container c;
    return parse_container(data, size, c) ? nullptr : c.why;
}

bool thumb_entry(const Section &s, int index, ThumbView &out) {
    if (index < 0) return false;
    const int n = walk_thumbs(s, index, &out);
    return n > index;
}

bool jpeg_dimensions(const uint8_t *d, size_t n, int *w, int *h) {
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return false;
    size_t i = 2;
    while (i + 4 <= n) {
        if (d[i] != 0xFF) return false;
        const uint8_t m = d[i + 1];
        if (m == 0xFF) { ++i; continue; }                   /* fill byte */
        if (m == 0xD9 || m == 0xDA) return false;          /* end, or scan before a frame */
        const size_t len = (size_t)(d[i + 2] << 8 | d[i + 3]);
        /* SOF0..SOF15 except DHT (C4), JPG (C8) and DAC (CC). */
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            if (i + 9 > n) return false;
            *h = d[i + 5] << 8 | d[i + 6];
            *w = d[i + 7] << 8 | d[i + 8];
            return true;
        }
        i += 2 + len;
    }
    return false;
}

} /* namespace fif */

using namespace fif;

extern "C" {

fif_status fif_get_chunk(const uint8_t *data, size_t size, int index, fif_chunk *out) {
    if (!data || !out || index < 0) return FIF_ERR_ARG;
    Container c;
    if (!parse_container(data, size, c)) return FIF_ERR_FORMAT;
    /* The pointers handed out point into `data`; an unpacked copy would die
     * with this call.  fif_unpack makes a file whose chunks can be read. */
    if (c.locked(SEC_CHUNKS)) return FIF_ERR_ENCRYPTED;
    if (c.packed(SEC_CHUNKS)) return FIF_ERR_UNSUPPORTED;
    Rd r{c.chunks.p, c.chunks.p + c.chunks.avail};
    ChunkView v;
    for (int i = 0; next_chunk(r, v); ++i) {
        if (i != index) continue;
        std::memset(out, 0, sizeof(*out));
        std::memcpy(out->type, v.type, 4);
        /* A value type this reader does not know is opaque bytes to it; the
         * enum cannot hold an arbitrary byte. */
        out->value_type = v.value_type <= FIF_VALUE_TEXT ? (fif_value_type)v.value_type
                                                         : FIF_VALUE_BINARY;
        out->name = v.name;
        out->name_len = v.name_len;
        out->data = v.data;
        out->size = v.size;
        out->crc_ok = v.crc_ok;
        return FIF_OK;
    }
    return FIF_ERR_ARG;
}

/* The index-th thumbnail of an opened thumbnail view; `out` points into the
 * view, so into `data` or into the container's store. */
static fif_status thumbnail_of(const Container &c, int index, fif_thumbnail *out) {
    std::memset(out, 0, sizeof(*out));
    const int n = thumb_count(c.thumb);
    if (n < 0) return FIF_ERR_FORMAT;
    if (index >= n) return FIF_ERR_ARG;                 /* no such thumbnail */
    ThumbView t;
    if (!thumb_entry(c.thumb, index, t)) return FIF_ERR_FORMAT;
    if (!t.complete()) return FIF_ERR_FORMAT;
    std::memcpy(out->format, t.format, 4);
    out->width = (int)t.width;
    out->height = (int)t.height;
    out->data = t.data;
    out->size = t.size;
    out->crc_ok = t.crc_ok();
    /* The file carries its own size; the directory is only a hint. */
    if (std::memcmp(t.format, "JPEG", 4) == 0)
        jpeg_dimensions(out->data, (size_t)out->size, &out->width, &out->height);
    return FIF_OK;
}

fif_status fif_get_thumbnail(const uint8_t *data, size_t size, int index, fif_thumbnail *out) {
    if (!data || !out || index < 0) return FIF_ERR_ARG;
    Container c;
    if (!parse_container(data, size, c)) return FIF_ERR_FORMAT;
    if (c.locked(SEC_THUMBS)) return FIF_ERR_ENCRYPTED;
    if (c.packed(SEC_THUMBS)) return FIF_ERR_UNSUPPORTED;   /* see fif_unpack */
    return thumbnail_of(c, index, out);
}

int fif_find_thumbnail(const uint8_t *data, size_t size, int min_edge, const char *format) {
    Container c;
    if (!data || !parse_container(data, size, c) || open_section(c, SEC_THUMBS) != FIF_OK) return -1;
    const int n = thumb_count(c.thumb);
    int best = -1, best_edge = 0;
    for (int i = 0; i < n; ++i) {
        ThumbView t;
        if (!thumb_entry(c.thumb, i, t)) return -1;
        if (format && std::strncmp(t.format, format, 4) != 0) continue;
        const int edge = (int)std::max(t.width, t.height);
        /* Smallest that reaches min_edge; while none does, the largest. */
        const bool fits = edge >= min_edge, best_fits = best >= 0 && best_edge >= min_edge;
        if (best < 0 || (fits && (!best_fits || edge < best_edge)) ||
            (!fits && !best_fits && edge > best_edge)) {
            best = i; best_edge = edge;
        }
    }
    return best;
}

fif_status fif_decode_thumbnail(const uint8_t *data, size_t size, int index, fif_image *out) {
    if (!data || !out || index < 0) return FIF_ERR_ARG;
    /* Decoded while the container, and with it an unpacked section, lives. */
    Container c;
    if (!parse_container(data, size, c)) return FIF_ERR_FORMAT;
    fif_status st = open_section(c, SEC_THUMBS);
    if (st != FIF_OK) return st;
    fif_thumbnail t;
    st = thumbnail_of(c, index, &t);
    if (st != FIF_OK) return st;
    if (std::memcmp(t.format, "JPEG", 4) != 0) return FIF_ERR_UNSUPPORTED;
    return jpeg_decode_mem(t.data, (size_t)t.size, out);
}

fif_status fif_get_raw(const uint8_t *data, size_t size, int index, fif_raw_entry *out) {
    if (!data || !out || index < 0) return FIF_ERR_ARG;
    Container c;
    if (!parse_container(data, size, c)) return FIF_ERR_FORMAT;
    if (c.raw.size == 0 && !c.packed(SEC_RAW)) return FIF_ERR_ARG;
    if (c.locked(SEC_RAW)) return FIF_ERR_ENCRYPTED;
    if (c.packed(SEC_RAW)) return FIF_ERR_UNSUPPORTED;    /* see fif_unpack */
    /* count, then the entries back to back, each a description followed by
     * its data. */
    Rd r{c.raw.p, c.raw.p + c.raw.avail};
    const uint32_t count = r.u32();
    if (r.bad) return FIF_ERR_FORMAT;
    if ((uint32_t)index >= count) return FIF_ERR_ARG;
    for (uint32_t i = 0; i <= (uint32_t)index; ++i) {
        fif_raw_entry e;
        std::memset(&e, 0, sizeof(e));
        if (!r.need(4)) return FIF_ERR_FORMAT;
        std::memcpy(e.file_type, r.p, 4); r.p += 4;
        e.file_name_len = r.u16();
        if (!r.need(e.file_name_len)) return FIF_ERR_FORMAT;
        e.file_name = (const char *)r.p; r.p += e.file_name_len;
        e.file_size = r.u64();
        if (!r.need(4)) return FIF_ERR_FORMAT;
        std::memcpy(e.file_codec, r.p, 4); r.p += 4;
        e.metadata_len = r.u32();
        if (r.bad || e.metadata_len > r.left()) return FIF_ERR_FORMAT;
        e.metadata = (const char *)r.p; r.p += e.metadata_len;
        e.data_size = r.u64();
        if (r.bad) return FIF_ERR_FORMAT;
        /* A truncated file ends inside the data of its last entry.  That entry
         * is still reported, with what arrived, and says so. */
        e.data = r.p;
        e.data_avail = (uint64_t)std::min<uint64_t>(e.data_size, r.left());
        r.p += e.data_avail;
        e.complete = e.data_avail == e.data_size;
        if (i == (uint32_t)index) { *out = e; return FIF_OK; }
        if (!e.complete) return FIF_ERR_ARG;             /* the rest was cut off */
    }
    return FIF_ERR_ARG;
}

int fif_raw_count(const uint8_t *data, size_t size) {
    Container c;
    if (!data || !parse_container(data, size, c)) return -1;
    if (c.stored[SEC_RAW].size == 0) return 0;
    if (open_section(c, SEC_RAW) != FIF_OK) return -1;
    Rd r{c.raw.p, c.raw.p + c.raw.avail};
    const uint32_t n = r.u32();
    return r.bad || n > INT32_MAX ? -1 : (int)n;
}

fif_status fif_unpack(const uint8_t *data, size_t size, uint8_t **out_data, size_t *out_size) {
    if (!data || !out_data || !out_size) return FIF_ERR_ARG;
    *out_data = nullptr; *out_size = 0;
    Container c;
    if (!parse_container(data, size, c)) return FIF_ERR_FORMAT;
    /* Each packed section is replaced by its unpacked bytes and loses its
     * bit; an encrypted one cannot be, and is copied as it is.  The CRCs in
     * the header are over the unpacked bytes either way, so they carry over.
     * A section the buffer cuts short stays short: the copy declares the
     * size the section would have had. */
    uint64_t sz[SEC_COUNT];
    uint32_t crc[SEC_COUNT];
    const Section *src[SEC_COUNT];
    for (int s = 0; s < SEC_COUNT; ++s) {
        src[s] = &c.stored[s];
        crc[s] = c.stored[s].crc;
        if (c.packed(s) && !c.locked(s)) {
            const fif_status st = open_section(c, s);
            if (st != FIF_OK) return st;
            src[s] = &c.view(s);
            c.compressed &= (uint8_t)~section_bit(s);
            /* With no packed section left, the method field says none. */
            if (!c.compressed) c.compress_method = COMPRESS_NONE;
        }
        sz[s] = src[s]->size;
    }
    std::vector<uint8_t> v;
    write_header(c, sz, crc, v);
    for (int s = 0; s < SEC_COUNT; ++s)
        if (src[s]->avail) v.insert(v.end(), src[s]->p, src[s]->p + src[s]->avail);
    uint8_t *buf = (uint8_t *)std::malloc(v.size() ? v.size() : 1);
    if (!buf) return FIF_ERR_NOMEM;
    if (!v.empty()) std::memcpy(buf, v.data(), v.size());
    *out_data = buf;
    *out_size = v.size();
    return FIF_OK;
}

} /* extern "C" */
