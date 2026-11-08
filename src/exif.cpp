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

/* EXIF to chunks.  See exif.h. */
#include "exif.h"
#include "fif/fif.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace fif {
namespace {

/* One TIFF block, with its byte order and bounds.  Every read is checked;
 * a bad offset yields false, never a read outside the block. */
struct Tiff {
    const uint8_t *p = nullptr;
    size_t n = 0;
    bool le = true;

    bool u16(size_t at, uint32_t *v) const {
        if (at > n || n - at < 2) return false;
        *v = le ? (uint32_t)(p[at] | p[at + 1] << 8) : (uint32_t)(p[at] << 8 | p[at + 1]);
        return true;
    }
    bool u32(size_t at, uint32_t *v) const {
        if (at > n || n - at < 4) return false;
        *v = le ? (uint32_t)p[at] | (uint32_t)p[at + 1] << 8 | (uint32_t)p[at + 2] << 16 | (uint32_t)p[at + 3] << 24
                : (uint32_t)p[at] << 24 | (uint32_t)p[at + 1] << 16 | (uint32_t)p[at + 2] << 8 | (uint32_t)p[at + 3];
        return true;
    }
};

/* One IFD entry, resolved to where its value lies. */
struct Field {
    uint32_t tag = 0, type = 0, count = 0;
    size_t at = 0;              /* offset of the value inside the block */
    bool ok = false;
};

size_t type_size(uint32_t t) {
    switch (t) {
    case 1: case 2: case 6: case 7: return 1;       /* BYTE ASCII SBYTE UNDEFINED */
    case 3: case 8: return 2;                       /* SHORT SSHORT */
    case 4: case 9: case 11: return 4;              /* LONG SLONG FLOAT */
    case 5: case 10: case 12: return 8;             /* RATIONAL SRATIONAL DOUBLE */
    default: return 0;
    }
}

/* Reads the IFD at `off` into `out`, keyed by tag. */
bool read_ifd(const Tiff &t, uint32_t off, std::vector<Field> &out) {
    uint32_t count;
    if (!t.u16(off, &count) || count > 1000) return false;
    for (uint32_t i = 0; i < count; ++i) {
        const size_t e = (size_t)off + 2 + (size_t)i * 12;
        Field f;
        if (!t.u16(e, &f.tag) || !t.u16(e + 2, &f.type) || !t.u32(e + 4, &f.count)) return false;
        const size_t sz = type_size(f.type);
        if (!sz || f.count > t.n / sz) continue;    /* unknown type or impossible length */
        const size_t bytes = sz * f.count;
        if (bytes <= 4) {
            f.at = e + 8;
        } else {
            uint32_t vo;
            if (!t.u32(e + 8, &vo) || vo > t.n || t.n - vo < bytes) continue;
            f.at = vo;
        }
        f.ok = true;
        out.push_back(f);
    }
    return true;
}

const Field *find(const std::vector<Field> &v, uint32_t tag) {
    for (const Field &f : v) if (f.tag == tag) return &f;
    return nullptr;
}

bool valid_utf8(const std::string &s) {
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        const int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (n < 0 || (n > 0 && i + (size_t)n >= s.size())) return false;
        for (int k = 1; k <= n; ++k)
            if (((unsigned char)s[i + k] >> 6) != 2) return false;
        i += (size_t)n + 1;
    }
    return true;
}

/* An ASCII field as UTF-8 text: up to the first NUL, trimmed.  EXIF says
 * ASCII, cameras and editors write Latin-1 or UTF-8 into it; text that is not
 * valid UTF-8 is read as Latin-1, which is what a stray (c) sign usually is. */
std::string text(const Tiff &t, const Field *f) {
    if (!f || !f->ok || (f->type != 2 && f->type != 7 && f->type != 1)) return {};
    const char *s = (const char *)t.p + f->at;
    size_t n = 0;
    while (n < f->count && s[n]) ++n;
    std::string r(s, n);
    while (!r.empty() && (unsigned char)r.back() <= ' ') r.pop_back();
    size_t b = 0;
    while (b < r.size() && (unsigned char)r[b] <= ' ') ++b;
    r.erase(0, b);
    if (!valid_utf8(r)) {
        std::string u;
        for (unsigned char c : r) {
            if (c < 0x80) u += (char)c;
            else { u += (char)(0xC0 | c >> 6); u += (char)(0x80 | (c & 0x3F)); }
        }
        r = u;
    }
    return r;
}

bool rational(const Tiff &t, const Field *f, double *v) {
    if (!f || !f->ok || f->count < 1) return false;
    if (f->type == 3) { uint32_t x; if (!t.u16(f->at, &x)) return false; *v = x; return true; }
    if (f->type != 5) return false;
    uint32_t a, b;
    if (!t.u32(f->at, &a) || !t.u32(f->at + 4, &b) || b == 0) return false;
    *v = (double)a / b;
    return true;
}

bool uint_value(const Tiff &t, const Field *f, uint32_t *v) {
    if (!f || !f->ok || f->count < 1) return false;
    if (f->type == 3) return t.u16(f->at, v);
    if (f->type == 4) return t.u32(f->at, v);
    return false;
}

void add_text(Exif &out, const char *type, const char *name, const std::string &v) {
    if (v.empty()) return;
    ExifEntry e{};
    std::memcpy(e.type, type, 5);
    e.name = name;
    e.value_type = FIF_VALUE_TEXT;
    e.value = v;
    out.entries.push_back(std::move(e));
}
void add_uint(Exif &out, const char *name, uint32_t v) {
    ExifEntry e{};
    std::memcpy(e.type, "ORGN", 5);
    e.name = name;
    e.value_type = FIF_VALUE_UINT;
    for (int i = 0; i < 4; ++i) e.value += (char)(v >> (8 * i) & 0xFF);
    out.entries.push_back(std::move(e));
}
void add_float(Exif &out, const char *name, double v) {
    ExifEntry e{};
    std::memcpy(e.type, "ORGN", 5);
    e.name = name;
    e.value_type = FIF_VALUE_FLOAT;
    uint64_t bits;
    std::memcpy(&bits, &v, 8);
    for (int i = 0; i < 8; ++i) e.value += (char)(bits >> (8 * i) & 0xFF);
    out.entries.push_back(std::move(e));
}

/* Placeholders cameras write into ImageDescription; not a description. */
bool junk_description(const std::string &s) {
    static const char *const junk[] = {"OLYMPUS DIGITAL CAMERA", "SONY DSC", "DIGITAL CAMERA",
                                       "default", "Exif_JPEG_PICTURE", "SAMSUNG", "LG Electronics"};
    for (const char *j : junk) if (s == j) return true;
    return false;
}

/* The TIFF block inside the file: JPEG APP1 "Exif\0\0", or PNG eXIf. */
bool locate(const uint8_t *d, size_t n, const uint8_t **tiff, size_t *tn) {
    if (n >= 4 && d[0] == 0xFF && d[1] == 0xD8) {
        size_t i = 2;
        while (i + 4 <= n) {
            if (d[i] != 0xFF) return false;
            const uint8_t m = d[i + 1];
            if (m == 0xFF) { ++i; continue; }
            if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) { i += 2; continue; }
            if (m == 0xDA || m == 0xD9) return false;   /* image data: no EXIF before it */
            const size_t len = (size_t)(d[i + 2] << 8 | d[i + 3]);
            if (len < 2 || i + 2 + len > n) return false;
            if (m == 0xE1 && len >= 8 && !std::memcmp(d + i + 4, "Exif\0\0", 6)) {
                *tiff = d + i + 10;
                *tn = len - 8;
                return true;
            }
            i += 2 + len;
        }
        return false;
    }
    if (n >= 8 && !std::memcmp(d, "\x89PNG\r\n\x1a\n", 8)) {
        size_t i = 8;
        while (i + 12 <= n) {
            const size_t len = (size_t)d[i] << 24 | (size_t)d[i + 1] << 16 | (size_t)d[i + 2] << 8 | d[i + 3];
            if (len > n - i - 12) return false;
            if (!std::memcmp(d + i + 4, "eXIf", 4)) { *tiff = d + i + 8; *tn = len; return true; }
            if (!std::memcmp(d + i + 4, "IEND", 4)) return false;
            i += 12 + len;
        }
    }
    return false;
}

} /* namespace */

bool read_exif(const uint8_t *file, size_t size, Exif &out) {
    out = Exif{};
    Tiff t;
    if (!file || !locate(file, size, &t.p, &t.n) || t.n < 8) return false;
    if (!std::memcmp(t.p, "II*\0", 4)) t.le = true;
    else if (!std::memcmp(t.p, "MM\0*", 4)) t.le = false;
    else return false;
    uint32_t ifd0;
    std::vector<Field> f0, fx;
    if (!t.u32(4, &ifd0) || !read_ifd(t, ifd0, f0)) return false;
    uint32_t xo;
    if (uint_value(t, find(f0, 0x8769), &xo) && xo != ifd0) read_ifd(t, xo, fx);

    uint32_t o;
    if (uint_value(t, find(f0, 0x0112), &o) && o >= 1 && o <= 8) out.orientation = (int)o;

    add_text(out, "ORGN", "camera_make", text(t, find(f0, 0x010F)));
    add_text(out, "ORGN", "camera_model", text(t, find(f0, 0x0110)));
    add_text(out, "ORGN", "lens", text(t, find(fx, 0xA434)));
    /* "2024:05:01 14:03:22" -> "2024-05-01T14:03:22", plus the offset from
     * UTC where the camera recorded one. */
    std::string when = text(t, find(fx, 0x9003));
    if (when.size() == 19 && when[4] == ':' && when[7] == ':' && when[10] == ' ') {
        when[4] = when[7] = '-';
        when[10] = 'T';
        when += text(t, find(fx, 0x9011));
        add_text(out, "ORGN", "date_taken", when);
    }
    double v;
    uint32_t u;
    if (rational(t, find(fx, 0x829A), &v) && v > 0) {
        char b[32];
        if (v < 1.0) std::snprintf(b, sizeof b, "1/%.0f", 1.0 / v);
        else std::snprintf(b, sizeof b, "%.3g", v);
        add_text(out, "ORGN", "exposure_time", b);
    }
    if (rational(t, find(fx, 0x829D), &v) && v > 0) add_float(out, "f_number", std::round(v * 10) / 10);
    if (uint_value(t, find(fx, 0x8827), &u) && u > 0) add_uint(out, "iso", u);
    if (rational(t, find(fx, 0x920A), &v) && v > 0) add_float(out, "focal_length", std::round(v * 10) / 10);
    if (uint_value(t, find(fx, 0xA405), &u) && u > 0) add_uint(out, "focal_35mm", u);
    add_text(out, "ORGN", "author", text(t, find(f0, 0x013B)));
    add_text(out, "ORGN", "copyright", text(t, find(f0, 0x8298)));
    const std::string desc = text(t, find(f0, 0x010E));
    if (!junk_description(desc)) add_text(out, "ORGN", "description", desc);
    add_text(out, "ORGN", "software", text(t, find(f0, 0x0131)));
    return true;
}

} /* namespace fif */
