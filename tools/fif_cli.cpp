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

#include "fif/fif.h"
#include "debug.h"
#include "format.h"
#include "exif.h"
#ifdef _WIN32
#  include <fcntl.h>
#endif
#include <sys/stat.h>
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cctype>
#ifdef _WIN32
#  include <io.h>
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <shellapi.h>
#  define isatty _isatty
#else
#  include <unistd.h>
#endif

/* The help and usage texts quote the decoder's pixel limit.  Splicing it in
 * from the header keeps the number in the text and the number in the check
 * from drifting apart. */
#define FIF_STR_(x) #x
#define FIF_STR(x) FIF_STR_(x)
#define kMaxPixelsText FIF_STR(FIF_DEFAULT_MAX_PIXELS)

namespace {

/* Exit codes.  1: the command ran and something went wrong - a file, the
 * data, the machine.  2: the command line itself is wrong, and nothing was
 * read or written.  Scripts tell the two apart, so keep them apart. */
constexpr int kExitFail = 1;
constexpr int kExitUsage = 2;
/* 3: the command did what it was asked, but warned while doing it.  Only
 * returned with --strict, so a script that wants to see warnings can, and one
 * that does not keeps the old two codes. */
constexpr int kExitWarn = 3;

/* The version of this tool, which is not the library's: fif and libfif are
 * released on their own schedules, and the banner names both. */
constexpr int kCliVersionMajor = 2;
constexpr int kCliVersionMinor = 5;

#ifdef FIF_HAVE_PNG
constexpr bool kHavePng = true;
#else
constexpr bool kHavePng = false;
#endif
#ifdef FIF_HAVE_JPEG
constexpr bool kHaveJpeg = true;
#else
constexpr bool kHaveJpeg = false;
#endif

/* Everything the command line sets that is not a field of the library's own
 * option structs.  One run, one command, so one instance. */
struct CliState {
    int progress = -1;          /* -1: on when stderr is a terminal */
    int bench_deblock = -1;     /* decode side of `fif bench` */
    double bench_relax = 0;     /* 0 = leave the decoder's default alone */
    const char *train_path = nullptr;
    bool quality_set = false;   /* -q given, to warn when a target overrides it */
    bool no_exif = false;       /* --no-exif */
    bool overwrite = false;     /* -y: replace existing output files */
    bool quiet = false;         /* --quiet: no banner, no status lines */
    bool strict = false;        /* --strict: a warning makes the exit code 3 */
    int warnings = 0;           /* what report_warning has said so far */

    /* Pre-encode downscale. At most one of these is set; 0 = not asked for. */
    double down_factor = 0;
    int down_width = 0, down_height = 0, down_max = 0;

    /* What the image was before the downscale, for the header.  Filled by
     * apply_downscale; zero until then. */
    int src_width = 0, src_height = 0;
    double src_scale = 1.0;

    /* Chunks from --meta / --text.  The strings live here for the whole run
     * because fif_chunk only points at them. */
    std::vector<std::string> chunk_text;
    std::vector<fif_chunk> chunks;
    int thumb_size = 0, thumb_quality = 0;

    /* The command line as UTF-8.  On Windows argv is in the ANSI code page,
     * which turns anything outside it into '?' - fine for file names the C
     * runtime opens with the same code page, wrong for text we store.  Text
     * that ends up in the file is taken from here instead. */
    std::vector<std::string> argv_utf8;
};
CliState g;

/* ------------------------------------------------------------ diagnostics
 *
 * Every failure the tool reports goes through these, so they all look alike:
 *
 *   fif: error: cannot read 'photo.png': no such file
 *     hint: check the spelling; a relative path starts in the current directory
 *
 * The first line says what went wrong in terms of what was asked for; each
 * hint says one thing to do about it.  A hint is advice, never a restatement:
 * where there is nothing useful to suggest, there is no hint. */
#if defined(__GNUC__) || defined(__clang__)
#  define FIF_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#  define FIF_PRINTF(a, b)
#endif

void vreport(const char *lead, const char *fmt, va_list ap) {
    std::fputs(lead, stderr);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
}
FIF_PRINTF(1, 2) void report_error(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vreport("fif: error: ", fmt, ap); va_end(ap);
}
FIF_PRINTF(1, 2) void report_warning(const char *fmt, ...) {
    ++g.warnings;
    va_list ap; va_start(ap, fmt); vreport("fif: warning: ", fmt, ap); va_end(ap);
}
/* A status line: what the tool is doing, not what went wrong.  --quiet drops
 * these and keeps the warnings and errors, which is what a pipeline wants.
 * On stdout, like the result below: these lines are what the run has to say
 * about its work, and only the banner, the warnings and the errors are not. */
FIF_PRINTF(1, 2) void note(const char *fmt, ...) {
    if (g.quiet) return;
    va_list ap; va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
}
/* The one line that says what the command produced.  Not a status line: a
 * script that calls fif to write a file wants that name, its size and its
 * bitrate even under --quiet, so this is the line --quiet keeps. */
FIF_PRINTF(1, 2) void result(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
}
FIF_PRINTF(1, 2) void hint(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vreport("  hint: ", fmt, ap); va_end(ap);
}

/* Base name of a path, for the header: the directory it was encoded from is
 * nobody's business and would leak a user name into every file. */
const char *base_name(const char *path) {
    const char *b = path;
    for (const char *s = path; *s; ++s)
        if (*s == '/' || *s == '\\') b = s + 1;
    return b;
}

/* The directory part of a path, "." when there is none. */
std::string dir_name(const char *path) {
    const char *b = base_name(path);
    if (b == path) return ".";
    std::string d(path, (size_t)(b - path));
    while (d.size() > 1 && (d.back() == '/' || d.back() == '\\')) d.pop_back();
    return d;
}

/* Lower-case extension without the dot; empty when there is none. */
std::string ext_lower(const char *path) {
    const char *b = base_name(path);
    const char *dot = std::strrchr(b, '.');
    std::string e;
    if (dot && dot != b)
        for (const char *s = dot + 1; *s; ++s) e += (char)std::tolower((unsigned char)*s);
    return e;
}

/* The directory part of `path` as it was typed, separator included; empty
 * when there is none.  Default output names go next to their input. */
std::string dir_prefix(const char *path) {
    return std::string(path, (size_t)(base_name(path) - path));
}

/* The file name of `path` without its extension. */
std::string stem(const char *path) {
    const char *b = base_name(path);
    const char *dot = std::strrchr(b, '.');
    return std::string(b, dot && dot != b ? (size_t)(dot - b) : std::strlen(b));
}

/* What a file is, from its first bytes or from its name.  The library picks
 * a reader by extension alone, so most "cannot read" cases are one of these
 * disagreeing with the other, or a format no reader exists for. */
enum class Kind { Unknown, Fif2, Png, Jpeg, Pnm, PnmOther, Gif, Bmp, Tiff, Webp, Heif };

const char *kind_name(Kind k) {
    switch (k) {
    case Kind::Fif2: return "FIF2";
    case Kind::Png: return "PNG";
    case Kind::Jpeg: return "JPEG";
    case Kind::Pnm: return "PNM";
    case Kind::PnmOther: return "plain-text or 1-bit PNM";
    case Kind::Gif: return "GIF";
    case Kind::Bmp: return "BMP";
    case Kind::Tiff: return "TIFF";
    case Kind::Webp: return "WebP";
    case Kind::Heif: return "HEIF/AVIF";
    default: return "unknown";
    }
}

Kind sniff_bytes(const uint8_t *d, size_t n) {
    if (n >= 4 && !std::memcmp(d, "FIF2", 4)) return Kind::Fif2;
    if (n >= 8 && !std::memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return Kind::Png;
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return Kind::Jpeg;
    if (n >= 2 && d[0] == 'P' && (d[1] == '5' || d[1] == '6')) return Kind::Pnm;
    if (n >= 2 && d[0] == 'P' && d[1] >= '1' && d[1] <= '7') return Kind::PnmOther;
    if (n >= 4 && !std::memcmp(d, "GIF8", 4)) return Kind::Gif;
    if (n >= 2 && d[0] == 'B' && d[1] == 'M') return Kind::Bmp;
    if (n >= 4 && (!std::memcmp(d, "II*\0", 4) || !std::memcmp(d, "MM\0*", 4))) return Kind::Tiff;
    if (n >= 12 && !std::memcmp(d, "RIFF", 4) && !std::memcmp(d + 8, "WEBP", 4)) return Kind::Webp;
    if (n >= 12 && !std::memcmp(d + 4, "ftyp", 4)) return Kind::Heif;
    return Kind::Unknown;
}

Kind sniff_file(const char *path) {
    uint8_t b[16] = {};
    FILE *f = std::fopen(path, "rb");
    if (!f) return Kind::Unknown;
    const size_t n = std::fread(b, 1, sizeof b, f);
    std::fclose(f);
    return sniff_bytes(b, n);
}

Kind kind_from_ext(const char *path) {
    const std::string e = ext_lower(path);
    if (e == "fif") return Kind::Fif2;
    if (e == "png") return Kind::Png;
    if (e == "jpg" || e == "jpeg") return Kind::Jpeg;
    if (e == "ppm" || e == "pgm" || e == "pnm") return Kind::Pnm;
    if (e == "gif") return Kind::Gif;
    if (e == "bmp") return Kind::Bmp;
    if (e == "tif" || e == "tiff") return Kind::Tiff;
    if (e == "webp") return Kind::Webp;
    if (e == "heic" || e == "heif" || e == "avif") return Kind::Heif;
    return Kind::Unknown;
}

const char *ext_for(Kind k) {
    switch (k) {
    case Kind::Png: return "png";
    case Kind::Jpeg: return "jpg";
    case Kind::Pnm: return "ppm";
    case Kind::Fif2: return "fif";
    default: return nullptr;
    }
}

/* Which image formats this particular build reads and writes; the list in an
 * error should be the one that is true here, not the one in the manual. */
std::string formats_built_in() {
    std::string s;
    if (kHavePng) s += ".png, ";
    if (kHaveJpeg) s += ".jpg, ";
    return s + ".ppm, .pgm";
}

/* Says what is missing from the build and how to get it. */
void hint_missing_library(Kind k) {
    const bool png = k == Kind::Png;
    hint("this fif was built without %s; install %s so that pkg-config finds it, "
         "then rebuild with 'make clean && make' (Windows: build.bat)",
         png ? "libpng" : "libjpeg", png ? "libpng" : "libjpeg-turbo");
}

/* Why a path cannot be opened for reading, from the errno of the attempt. */
void explain_open_error(const char *path, int err) {
    switch (err) {
    case ENOENT:
        report_error("cannot open '%s': no such file", path);
        if (base_name(path) == path)
            hint("the file is looked for in the current directory; give the full path if it is elsewhere");
        else
            hint("check the spelling of the directory and the file name");
        break;
    case EACCES:
    case EPERM:
        report_error("cannot open '%s': permission denied", path);
        hint("check that your user may read the file");
        break;
    default:
        report_error("cannot open '%s': %s", path, std::strerror(err));
    }
}

/* Checks a path the command is about to read: there, a file, not empty.  The
 * cheap test up front turns "i/o error" into the reason. */
bool check_readable(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) { explain_open_error(path, errno); return false; }
    if ((st.st_mode & S_IFMT) == S_IFDIR) {
        report_error("'%s' is a directory, not a file", path);
        hint("give the path of a file inside it");
        return false;
    }
    if (st.st_size == 0) {
        report_error("'%s' is empty (0 bytes)", path);
        hint("it was probably not written completely; create or copy it again");
        return false;
    }
    FILE *f = std::fopen(path, "rb");
    if (!f) { explain_open_error(path, errno); return false; }
    std::fclose(f);
    return true;
}

/* Checks, before any work is done, that `path` can be created: an encode can
 * take minutes, and losing it to a typo in the output directory is the most
 * avoidable failure there is. */
bool check_writable(const char *path) {
    const std::string dir = dir_name(path);
    struct stat st;
    if (stat(dir.c_str(), &st) != 0) {
        report_error("cannot write '%s': the directory '%s' does not exist", path, dir.c_str());
        hint("create it first (mkdir \"%s\") or choose an existing directory", dir.c_str());
        return false;
    }
    if ((st.st_mode & S_IFMT) != S_IFDIR) {
        report_error("cannot write '%s': '%s' is not a directory", path, dir.c_str());
        return false;
    }
    if (stat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR) {
        report_error("cannot write '%s': it is a directory", path);
        hint("give a file name inside it, e.g. %s/out.png", path);
        return false;
    }
    return true;
}

/* The same file under two names would be overwritten while it is being read
 * from - or, for an encode, replaced by something the user cannot undo. */
bool same_file(const char *a, const char *b) {
    if (!std::strcmp(a, b)) return true;
    struct stat sa, sb;
    if (stat(a, &sa) != 0 || stat(b, &sb) != 0) return false;
    return sa.st_ino != 0 && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

/* An existing file is replaced only when the user says so: -y on the command
 * line, or a yes at the prompt.  The prompt needs someone to answer it, so it
 * is only shown when both stdin and stderr are a terminal; a script without -y
 * gets an error rather than a hang or a silently replaced file.  Called before
 * any lengthy work, so that the answer is not asked for after a long encode. */
bool may_write(const char *path) {
    struct stat st;
    if (g.overwrite || stat(path, &st) != 0) return true;
    if (isatty(0) && isatty(2)) {
        std::fprintf(stderr, "fif: '%s' already exists. Overwrite? [y/N] ", path);
        std::fflush(stderr);
        char line[64] = {};
        if (std::fgets(line, sizeof line, stdin) && (line[0] == 'y' || line[0] == 'Y'))
            return true;
        report_error("'%s' was not overwritten", path);
        hint("choose another output name, or add -y to overwrite without asking");
        return false;
    }
    report_error("'%s' already exists", path);
    hint("add -y to overwrite it, or choose another output name");
    return false;
}

/* An image file could not be loaded.  Works out which of the usual reasons it
 * was: wrong format, a format this build lacks, a name that lies about the
 * contents, a PNM variant the reader does not take, or a damaged file. */
void explain_image_load(const char *path, fif_status st) {
    if (st == FIF_ERR_IO) {
        if (!check_readable(path)) return;
        report_error("cannot read '%s': %s", path, std::strerror(errno));
        return;
    }
    if (st == FIF_ERR_NOMEM) {
        report_error("not enough memory to load '%s'", path);
        hint("close other programs, or reduce the image with an editor first");
        return;
    }
    const Kind k = sniff_file(path), e = kind_from_ext(path);
    const std::string ext = ext_lower(path);
    switch (k) {
    case Kind::Fif2:
        report_error("'%s' is a FIF2 file, not an input image", path);
        hint("to turn it back into a picture: fif decode \"%s\"", path);
        return;
    case Kind::Gif: case Kind::Bmp: case Kind::Tiff: case Kind::Webp: case Kind::Heif:
        report_error("'%s' is a %s image, which fif cannot read", path, kind_name(k));
        hint("convert it to PNG first, e.g. 'magick \"%s\" in.png' (ImageMagick) or with any image editor", path);
        hint("this fif reads %s", formats_built_in().c_str());
        return;
    case Kind::PnmOther:
        report_error("'%s' is a plain-text (P1-P3) or 1-bit (P4) PNM file, which fif cannot read", path);
        hint("save it as binary PPM/PGM (P6/P5) or as PNG, e.g. 'magick \"%s\" converted.png'", path);
        return;
    case Kind::Png:
    case Kind::Jpeg:
        if ((k == Kind::Png && !kHavePng) || (k == Kind::Jpeg && !kHaveJpeg)) {
            report_error("'%s' is a %s image, but this fif cannot read %s", path,
                         kind_name(k), kind_name(k));
            hint_missing_library(k);
            hint("or convert it to binary PPM, which every build reads");
            return;
        }
        break;
    default:
        break;
    }
    if (k != Kind::Unknown && k != e) {
        report_error("'%s' is named .%s but contains a %s image", path,
                     ext.empty() ? "(no extension)" : ext.c_str(), kind_name(k));
        hint("fif picks the reader by the extension; rename the file to .%s", ext_for(k));
        return;
    }
    if (k == Kind::Unknown) {
        report_error("cannot read '%s': not a PNG, JPEG or PNM image", path);
        hint("this fif reads %s", formats_built_in().c_str());
        return;
    }
    if (k == Kind::Pnm) {
        /* The one PNM the reader refuses although it is binary: more than
         * eight bits per sample. */
        FILE *f = std::fopen(path, "rb");
        int w = 0, h = 0, mx = 0;
        if (f) {
            char m[3] = {};
            if (std::fscanf(f, "%2s %d %d %d", m, &w, &h, &mx) != 4) mx = 0;
            std::fclose(f);
        }
        if (mx > 255) {
            report_error("'%s' has %d as its largest sample value; fif reads 8-bit PNM only (255)", path, mx);
            hint("convert it to 8 bits, e.g. 'magick \"%s\" -depth 8 in.ppm', or use a 16-bit PNG, which is reduced automatically", path);
            return;
        }
    }
    if (st == FIF_ERR_UNSUPPORTED) {
        report_error("'%s' uses a pixel layout fif does not support", path);
        hint("fif encodes 8-bit greyscale or RGB; save the image as plain RGB PNG");
        return;
    }
    report_error("cannot read '%s': the %s data is damaged or cut off", path, kind_name(k));
    hint("open it in an image viewer to check; if it is broken there as well, create or download it again");
}

/* Why a file could not be created or written, from errno. */
void explain_write_error(const char *path, int err) {
    switch (err) {
    case ENOENT:
        report_error("cannot write '%s': the directory '%s' does not exist", path, dir_name(path).c_str());
        hint("create it first, or choose an existing directory");
        break;
    case EACCES:
    case EPERM:
        report_error("cannot write '%s': permission denied", path);
        hint("choose a directory you may write to, or check whether the file is write-protected");
        break;
    case EISDIR:
        report_error("cannot write '%s': it is a directory", path);
        hint("give a file name inside it");
        break;
#ifdef EROFS
    case EROFS:
        report_error("cannot write '%s': the file system is read-only", path);
        hint("write to another drive or directory");
        break;
#endif
    case ENOSPC:
        report_error("cannot write '%s': the disk is full", path);
        hint("free some space, or write to another drive");
        break;
    default:
        if (err) report_error("cannot write '%s': %s", path, std::strerror(err));
        else report_error("cannot write '%s'", path);
    }
}

/* An image output file is chosen by its extension; refuse a name this build
 * cannot write before decoding anything, not after. */
bool check_image_output(const char *path, const char *in_path) {
    const Kind k = kind_from_ext(path);
    const std::string ext = ext_lower(path);
    if (k == Kind::Fif2) {
        report_error("the output '%s' must be an image, not a FIF2 file", path);
        hint("did you swap the two file names? the FIF2 file comes first: fif decode in.fif out.png");
        return false;
    }
    if ((k == Kind::Png && !kHavePng) || (k == Kind::Jpeg && !kHaveJpeg)) {
        report_error("cannot write '%s': this fif cannot write %s", path, kind_name(k));
        hint_missing_library(k);
        hint("or write .ppm, which every build supports");
        return false;
    }
    if (k != Kind::Png && k != Kind::Jpeg && k != Kind::Pnm) {
        if (ext.empty()) report_error("the output '%s' has no extension", path);
        else report_error("cannot write '%s': .%s is not an image format fif writes", path, ext.c_str());
        hint("the extension picks the format; this fif writes %s", formats_built_in().c_str());
        return false;
    }
    if (in_path && same_file(path, in_path)) {
        report_error("the output '%s' is the input file", path);
        hint("choose a different output name");
        return false;
    }
    return check_writable(path);
}

/* Turns `img` the way the EXIF orientation tag says it is meant to be seen.
 * A FIF2 decoder knows nothing about EXIF, so a portrait photo that a phone
 * stored sideways with a tag would otherwise come out sideways for good. */
bool apply_orientation(fif_image *img, int o) {
    if (o <= 1 || o > 8) return true;
    const bool swap = o >= 5;
    const int w = img->width, h = img->height, ch = img->channels;
    fif_image d{};
    if (fif_image_alloc(&d, swap ? h : w, swap ? w : h, ch) != FIF_OK) return false;
    for (int y = 0; y < d.height; ++y)
        for (int x = 0; x < d.width; ++x) {
            int sx = x, sy = y;
            switch (o) {
            case 2: sx = w - 1 - x; break;                       /* mirrored */
            case 3: sx = w - 1 - x; sy = h - 1 - y; break;       /* 180 */
            case 4: sy = h - 1 - y; break;                       /* flipped */
            case 5: sx = y; sy = x; break;                       /* transposed */
            case 6: sx = y; sy = h - 1 - x; break;               /* 90 clockwise */
            case 7: sx = w - 1 - y; sy = h - 1 - x; break;       /* transversed */
            case 8: sx = w - 1 - y; sy = x; break;               /* 90 anticlockwise */
            }
            std::memcpy(d.pixels + (size_t)y * d.stride + (size_t)x * ch,
                        img->pixels + (size_t)sy * img->stride + (size_t)sx * ch, (size_t)ch);
        }
    fif_image_free(img);
    *img = d;
    return true;
}

/* EXIF of the input: rotates the picture upright and queues the camera's
 * metadata as chunks.  An entry the user set with --meta/--text (same type
 * and name) wins over the camera's.  Before the downscale, so that --width
 * and friends refer to the picture as it is seen. */
bool apply_exif(const char *path, fif_image *img) {
    if (g.no_exif) return true;
    FILE *f = std::fopen(path, "rb");
    if (!f) return true;
    std::vector<uint8_t> head;
    uint8_t buf[65536];
    size_t got;
    /* EXIF sits near the start of a JPEG; a PNG may put eXIf anywhere, so
     * the whole file is read, up to a sane bound. */
    while ((got = std::fread(buf, 1, sizeof buf, f)) > 0 && head.size() < ((size_t)1 << 28))
        head.insert(head.end(), buf, buf + got);
    std::fclose(f);
    fif::Exif ex;
    if (!fif::read_exif(head.data(), head.size(), ex)) return true;
    int added = 0;
    for (const fif::ExifEntry &e : ex.entries) {
        bool taken = false;
        for (size_t k = 0; k < g.chunks.size() && !taken; ++k)
            taken = !std::memcmp(g.chunks[k].type, e.type, 4) &&
                    g.chunk_text[k].compare(0, g.chunk_text[k].find('\0'), e.name) == 0;
        if (taken) continue;
        fif_chunk c{};
        std::memcpy(c.type, e.type, 5);
        c.value_type = (fif_value_type)e.value_type;
        g.chunk_text.push_back(e.name + '\0' + e.value);
        g.chunks.push_back(c);
        ++added;
    }
    static const char *const turn[] = {"", "", "mirrored", "rotated 180 degrees", "flipped",
                                       "transposed", "rotated 90 degrees clockwise",
                                       "transversed", "rotated 90 degrees anticlockwise"};
    if (ex.orientation > 1) {
        if (!apply_orientation(img, ex.orientation)) {
            report_error("not enough memory to turn the picture upright");
            return false;
        }
        note("exif        %d entr%s, %s as the orientation tag asks\n",
             added, added == 1 ? "y" : "ies", turn[ex.orientation]);
    } else if (added) {
        note("exif        %d entr%s\n", added, added == 1 ? "y" : "ies");
    }
    return true;
}

/* Reduce `img` in place if a downscale was requested.  Done before anything
 * else looks at the image, so every reported dimension, bpp and metric refers
 * to what was actually encoded rather than to the file on disk. */
int apply_downscale(fif_image *img) {
    g.src_width = img->width;
    g.src_height = img->height;
    g.src_scale = 1.0;
    int w = 0, h = 0;
    const double aspect = (double)img->height / img->width;

    if (g.down_factor > 0) {
        w = (int)(img->width / g.down_factor + 0.5);
        h = (int)(img->height / g.down_factor + 0.5);
    } else if (g.down_width > 0) {
        w = g.down_width;  h = (int)(g.down_width * aspect + 0.5);
        if (w >= img->width)
            report_warning("--width %d is not smaller than the image (%d wide); fif only reduces, "
                           "so the image is encoded at its own size", w, img->width);
    } else if (g.down_height > 0) {
        h = g.down_height; w = (int)(g.down_height / aspect + 0.5);
        if (h >= img->height)
            report_warning("--height %d is not smaller than the image (%d tall); fif only reduces, "
                           "so the image is encoded at its own size", h, img->height);
    } else if (g.down_max > 0) {
        const int longest = img->width > img->height ? img->width : img->height;
        if (longest <= g.down_max) return 0;          /* already small enough */
        const double s = (double)g.down_max / longest;
        w = (int)(img->width * s + 0.5);
        h = (int)(img->height * s + 0.5);
    } else {
        return 0;
    }

    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > img->width)  w = img->width;
    if (h > img->height) h = img->height;
    if (w == img->width && h == img->height) return 0;

    fif_image small{};
    const fif_status st = fif_image_downscale(img, w, h, &small);
    if (st != FIF_OK) {
        report_error("cannot reduce the image to %dx%d: %s", w, h, fif_status_string(st));
        if (st == FIF_ERR_NOMEM) hint("not enough memory; reduce the image with an editor first");
        return 1;
    }
    note("downscaled  %dx%d -> %dx%d\n", img->width, img->height, w, h);
    fif_image_free(img);
    *img = small;
    /* The factor the user asked for when there was one; otherwise the one the
     * longer edge actually shrank by. */
    g.src_scale = g.down_factor > 0 ? g.down_factor
        : (g.src_width >= g.src_height ? (double)g.src_width / w
                                       : (double)g.src_height / h);
    return 0;
}

/* ------------------------------------------------------------------ help
 *
 * Three levels.  `fif` alone says where help is; `fif help` lists the
 * commands with a few examples and no options; `fif help <command>` (or
 * `fif <command> --help`) lists that command's options, grouped by what they
 * are for.  Everything goes to stdout - help that was asked for is output,
 * not a diagnostic, and should page and grep like any other. */
const char kHelpOverview[] =
"Usage: fif <command> <files> [options]\n"
"\n"
"Commands\n"
"  encode     <in> [<out.fif>]        compress a picture into a FIF2 file\n"
"  decode     <in.fif> [<out>]        turn a FIF2 file back into a picture\n"
"  info       <in.fif>                show what a FIF2 file contains\n"
"  thumbnail  <in.fif> [<out>]        export the preview stored in a file\n"
"  compare    <a> <b>                 measure how close two pictures are\n"
"  bench      <in>                    try settings: size, quality and speed\n"
"  version                            versions, features and links\n"
"  debug      <in.fif>                for developers: stream the decode state\n"
"\n"
"The output name may be left out; the result then goes next to the input.\n"
"An existing file is only replaced after asking, or with -y.\n"
"\n"
"Every command takes two more options\n"
"  --quiet                            only the one line that names the result\n"
"  --strict                           a warning makes the exit code 3\n"
"\n"
"Examples\n"
"  fif encode photo.jpg                       (writes photo.fif)\n"
"  fif encode photo.jpg web.fif -q 50 --max-dim 1920 --thumbnail 256\n"
"  fif decode photo.fif photo.png\n"
"  fif decode photo.fif poster.png --scale 2\n"
"\n"
"All options of one command: fif help <command>   (or: fif <command> --help)\n";

const char kHelpEncode[] =
"Usage: fif encode <in> [<out.fif>] [options]\n"
"\n"
"Compresses a PNG, JPEG or PNM picture. Without <out.fif> the result is\n"
"<in> with the extension .fif. Options come after the file names.\n"
"\n"
"Quality and speed\n"
"  -q, --quality N        0..100 (default 70); lower = smaller file\n"
"  -e, --effort N         0..9 (default 5); 2 is much faster and almost as good\n"
"  --target-bpp X         pick the quality for X bits per pixel (file size)\n"
"  --target-psnr X        pick the quality for X dB (picture quality)\n"
"\n"
"Picture size (reduces before encoding; never enlarges)\n"
"  --max-dim N            longer edge at most N pixels\n"
"  --width N, --height N  this width or height, keeping the shape\n"
"  --downscale F          smaller by the factor F, e.g. 2 = half\n"
"\n"
"Colour\n"
"  --subsampling S        420 colour at half resolution, like JPEG (default)\n"
"                         444 full colour resolution, for graphics and text\n"
"                         400 greyscale\n"
"\n"
"Stored with the picture\n"
"  --thumbnail N          a JPEG preview, N pixels on the longer edge\n"
"                         (default: none)\n"
"  --meta KEY=VALUE       a metadata entry, e.g. --meta author=Ada\n"
"  --text KEY=VALUE       a text entry, e.g. --text \"copyright=(c) 2026 Ada\"\n"
"  --no-exif              ignore the camera data (EXIF) of the input; by\n"
"                         default it is kept and the picture turned upright\n"
"\n"
"Other\n"
"  -y, --overwrite        replace an existing output file without asking\n"
"  -t, --threads N        threads (default 0 = 3/4 of the processor cores)\n"
"  --no-progress          hide the progress bar\n"
"\n"
"For specialists (the defaults come from measurements)\n"
"  --chroma M             colour coding: shared (default) | from-luma\n"
"  --thumbnail-quality Q  JPEG quality of the preview, 1..100 (default 85)\n"
"  --misc KEY=VALUE       an entry of type MISC\n"
"  --chunk TYPE:KEY=VALUE an entry of your own 1-4 character type\n"
"  --compress LIST        zlib-compress parts of the file: chunks, thumbnails,\n"
"                         fic or all (default: none)\n"
"  --remarks ABCD         the four-character REMARKS field (default: empty)\n"
"  --no-credits           leave out the credits entry (~130 bytes)\n"
"  --base-only            store only the coarse base layer: a much smaller file\n"
"                         that decodes to a blurred picture; no preview\n"
"  --no-chunks            store no chunks at all (encoder, colour, credits, your\n"
"                         own); with --base-only the file is header + base layer\n"
"  --tile N              tile edge in pixels (default 0 = 256 or 512)\n"
"  --min-block N          smallest block, power of two (default 4)\n"
"  --max-block N          largest block, power of two up to 256 (default 32)\n"
"  --local-radius N       search window (default: set by --effort)\n"
"  --kd-candidates N      feature search width, 0 = off (default: by --effort)\n"
"  --domain-shift N       search grid density 0..4 (default: by --effort)\n"
"  --planar N             1 = flat blocks may carry a gradient (default 1)\n"
"  --stats                print block counts and where the bits went\n"
"  --progress             show the progress bar even when not in a terminal\n"
"  --train-contexts F     write context statistics to F (codec development)\n"
"\n"
"Examples\n"
"  fif encode photo.jpg -q 60\n"
"  fif encode scan.png scan.fif --subsampling 400 --target-bpp 0.8 -e 2\n";

const char kHelpDecode[] =
"Usage: fif decode <in.fif> [<out.png|jpg|ppm>] [options]\n"
"\n"
"Turns a FIF2 file back into a picture. The extension of <out> picks the\n"
"format. Without <out> the picture gets the name of the original, as stored\n"
"in the file, next to <in.fif>; a file without a stored name gives\n"
"<in>.png.\n"
"\n"
"Size (default: the size it was encoded at)\n"
"  --scale S              2..16: N times larger, with real detail;\n"
"                         1/2, 1/4, 1/8: fast, smaller previews\n"
"  --width W, --height H  this width or height; the other side follows\n"
"  --size WxH             fill this box, trim what sticks out, never stretch\n"
"\n"
"Part of the picture (default: all of it)\n"
"  --region WxH+X+Y       only this rectangle; same pixels, much faster\n"
"  --tiles LIST           only these tiles, e.g. 0,5-6; the rest stays grey\n"
"                         ('fif info <in.fif> --tiles' lists them)\n"
"  --base-only            only the coarse picture, without the fine detail\n"
"\n"
"Look (default: as the file recommends)\n"
"  --deblock N            smooth the block edges, 0 (off) to 100;\n"
"                         -1 = as the file recommends (default)\n"
"  --dither               avoid banding in smooth gradients (default: off)\n"
"\n"
"Other\n"
"  -y, --overwrite        replace an existing output file without asking\n"
"  -t, --threads N        threads (default 0 = 3/4 of the processor cores)\n"
"  --no-progress          hide the progress bar\n"
"\n"
"For specialists\n"
"  --no-smooth-dc         flat areas as plain blocks, not soft gradients\n"
"  --chroma-upsampling M  4:2:0 colour: edge (default) | bilinear\n"
"  --iterations N         at most N rounds (default 0 = file's advice, else 16)\n"
"  --eps X                stop when no pixel changes by more than X\n"
"                         (default 0 = file's advice, else 0.5)\n"
"  --relaxation X         step size of the rounds (default 1, the best)\n"
"  --max-pixels N         warn above this many pixels (default " kMaxPixelsText ")\n"
"  --progress             show the progress bar even when not in a terminal\n"
"\n"
"Examples\n"
"  fif decode photo.fif poster.png --scale 3\n"
"  fif decode photo.fif wallpaper.png --size 2560x1440\n";

const char kHelpInfo[] =
"Usage: fif info <in.fif> [--tiles]\n"
"\n"
"Shows what a FIF2 file contains: picture size, the original's name and\n"
"size, quality, stored previews, metadata, attached data, and whether the\n"
"file is intact. Decodes nothing, so it is instant.\n"
"\n"
"  --tiles                also list the tiles: position, size, byte ranges\n"
"\n"
"Example\n"
"  fif info photo.fif\n";

const char kHelpThumbnail[] =
"Usage: fif thumbnail <in.fif> [<out>] [--id N]\n"
"\n"
"Exports the preview stored in a FIF2 file. Without <out> it is exported as\n"
"<in>-thumb.jpg next to <in.fif>. To .jpg a JPEG preview is copied\n"
"unchanged; other extensions convert it.\n"
"\n"
"  --id N                 which preview, if the file has several, counted from 1;\n"
"                         as 'fif info' lists them (default: the largest)\n"
"  -y, --overwrite        replace an existing output file without asking\n"
"\n"
"Examples\n"
"  fif thumbnail photo.fif\n"
"  fif thumbnail photo.fif small.png --id 1\n";

const char kHelpBench[] =
"Usage: fif bench <in> [options]\n"
"\n"
"Encodes and decodes in one step, writes no file, and shows the file size,\n"
"the quality (PSNR, SSIM, MS-SSIM) and the speed - the quick way to try\n"
"settings. Takes the encode options (fif help encode) except -y, and for\n"
"the decode:\n"
"\n"
"  --deblock N            smooth the block edges, 0 (off) to 100;\n"
"                         -1 = as the file recommends (default)\n"
"  --relaxation X         step size of the rounds (default 1)\n"
"  --stats                also show block counts and where the bits went\n"
"\n"
"Example\n"
"  fif bench photo.jpg --max-dim 1920 -q 60 -e 2\n";

const char kHelpCompare[] =
"Usage: fif compare <a> <b>\n"
"\n"
"Measures how close two pictures of the same size are, as PSNR (in dB),\n"
"SSIM and MS-SSIM (0 to 1). For all three, higher means closer. A greyscale\n"
"picture is compared with the brightness of a colour one.\n"
"\n"
"Example\n"
"  fif compare original.png decoded.png\n";

const char kHelpVersion[] =
"Usage: fif version\n"
"\n"
"Shows the versions of the tool, the library and the file format, what this\n"
"build supports, and where to find the source code and the format.\n";

const char kHelpDebug[] =
"Usage: fif debug <in.fif> [options]\n"
"\n"
"Streams the decode state - one JSON line, then raw frames - for debugging\n"
"and analysis tools. Takes the decode options --scale (N, 1/2, 1/4,\n"
"1/8), --iterations, --eps, --relaxation, --deblock, --base-only,\n"
"--no-smooth-dc, --chroma-upsampling, --dither and -t like fif decode (same\n"
"defaults), and:\n"
"\n"
"  --iteration N          write iteration N as an image instead; 0 = DC start\n"
"                         (default: stream every frame)\n"
"  -o, --out <file>       where --iteration writes (default <in>-iterNN.png)\n"
"  -y, --overwrite        replace an existing --iteration file without asking\n"
"  --ref <image>          the original: adds it as a frame, and PSNR and SSIM\n"
"                         of every frame against it\n"
"  --prefix N             decode only the first N bytes (default 0 = all)\n"
"  --only-tiles LIST      decode just these tiles, e.g. 0,3,5-8, or none\n"
"                         (default all)\n"
"  --preview-max N        cap the long edge of the stored frames\n"
"                         (default 0 = full size)\n"
"  --no-leaves            leave the leaf data out of the JSON (default: in)\n"
"\n"
"Examples\n"
"  fif debug photo.fif --iteration 3 -o step3.png\n";

/* ------------------------------------------------------------ the commands
 *
 * One table for what each command takes, so a missing file name, a wrong
 * option and an option that belongs to another command can all be explained
 * from the same facts. */
struct Command;
using CommandFn = int (*)(const Command &, int, char **);
struct Command {
    const char *name;
    const char *synopsis;
    const char *help;
    const char *files[2];       /* what the positional arguments are; null = none */
    int required;               /* how many of them must be given */
    const char *const *options;
    CommandFn run;
};
int cmd_encode(const Command &, int, char **);
int cmd_decode(const Command &, int, char **);
int cmd_info(const Command &, int, char **);
int cmd_thumbnail(const Command &, int, char **);
int cmd_bench(const Command &, int, char **);
int cmd_compare(const Command &, int, char **);
int cmd_version(const Command &, int, char **);
int cmd_debug(const Command &, int, char **);

const char *const kEncodeOpts[] = {
    "-q", "--quality", "-e", "--effort", "-t", "--threads", "--tile", "--min-block",
    "--max-block", "--target-psnr", "--target-bpp", "--local-radius", "--kd-candidates",
    "--domain-shift", "--planar", "--thumbnail", "--thumbnail-quality", "--meta", "--text",
    "--misc", "--chunk", "--compress", "--remarks", "--no-credits", "--no-exif",
    "--train-contexts", "--downscale", "--width", "--height", "--max-dim", "--subsampling",
    "--chroma", "--stats", "--progress", "--no-progress", "-y", "--overwrite",
    "--base-only", "--no-chunks", nullptr};
const char *const kBenchOpts[] = {
    "-q", "--quality", "-e", "--effort", "-t", "--threads", "--tile", "--min-block",
    "--max-block", "--target-psnr", "--target-bpp", "--local-radius", "--kd-candidates",
    "--domain-shift", "--planar", "--thumbnail", "--thumbnail-quality", "--meta", "--text",
    "--misc", "--chunk", "--compress", "--remarks", "--no-credits", "--no-exif",
    "--downscale", "--width", "--height", "--max-dim", "--subsampling", "--chroma",
    "--progress", "--no-progress", "--deblock", "--relaxation", "--stats", nullptr};
const char *const kDecodeOpts[] = {
    "--scale", "--width", "--height", "--size", "--region", "--tiles", "--iterations", "--eps",
    "--relaxation", "--deblock", "--no-smooth-dc", "--chroma-upsampling", "--dither", "--base-only",
    "--max-pixels", "--progress", "--no-progress", "-t", "--threads", "-y", "--overwrite", nullptr};
const char *const kInfoOpts[] = {"--tiles", nullptr};
const char *const kDebugOpts[] = {
    "--scale", "--iterations", "--eps", "--relaxation", "--deblock", "-t", "--threads",
    "--base-only", "--no-smooth-dc", "--chroma-upsampling", "--dither", "--ref",
    "--prefix", "--no-leaves", "--preview-max", "--only-tiles", "--iteration", "-o", "--out",
    "-y", "--overwrite", nullptr};
const char *const kThumbOpts[] = {"--id", "-y", "--overwrite", nullptr};
const char *const kNoOpts[] = {nullptr};

const Command kCommands[] = {
    {"encode", "fif encode <in> [<out.fif>] [options]", kHelpEncode, {"input image", "output file (.fif)"}, 1, kEncodeOpts, cmd_encode},
    {"decode", "fif decode <in.fif> [<out.png|jpg|ppm>] [options]", kHelpDecode, {"FIF2 file", "output image"}, 1, kDecodeOpts, cmd_decode},
    {"info", "fif info <in.fif> [--tiles]", kHelpInfo, {"FIF2 file", nullptr}, 1, kInfoOpts, cmd_info},
    {"thumbnail", "fif thumbnail <in.fif> [<out>] [--id N]", kHelpThumbnail, {"FIF2 file", "output image"}, 1, kThumbOpts, cmd_thumbnail},
    {"bench", "fif bench <in> [options]", kHelpBench, {"input image", nullptr}, 1, kBenchOpts, cmd_bench},
    {"compare", "fif compare <a> <b>", kHelpCompare, {"first image", "second image"}, 2, kNoOpts, cmd_compare},
    {"version", "fif version", kHelpVersion, {nullptr, nullptr}, 0, kNoOpts, cmd_version},
    {"debug", "fif debug <in.fif> [options]", kHelpDebug, {"FIF2 file", nullptr}, 1, kDebugOpts, cmd_debug},
};

const Command *find_command(const char *name) {
    for (const Command &c : kCommands)
        if (!std::strcmp(c.name, name)) return &c;
    return nullptr;
}

/* Edit distance, for "did you mean".  Options are short; the quadratic table
 * costs nothing. */
int edit_distance(const char *a, const char *b) {
    const size_t n = std::strlen(a), m = std::strlen(b);
    std::vector<int> prev(m + 1), cur(m + 1);
    for (size_t j = 0; j <= m; ++j) prev[j] = (int)j;
    for (size_t i = 1; i <= n; ++i) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= m; ++j)
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1,
                               prev[j - 1] + (a[i - 1] != b[j - 1])});
        std::swap(prev, cur);
    }
    return prev[m];
}

/* The closest name in a null-terminated list, or null when nothing is close
 * enough to be a plausible typo. */
const char *closest(const char *word, const char *const *names) {
    const char *best = nullptr;
    int best_d = INT_MAX;
    for (const char *const *n = names; *n; ++n) {
        const int d = edit_distance(word, *n);
        if (d < best_d) { best_d = d; best = *n; }
    }
    const int limit = std::max(1, (int)std::strlen(word) / 3);
    return best_d <= limit ? best : nullptr;
}

const char *closest_command(const char *word) {
    const char *names[std::size(kCommands) + 1] = {};
    for (size_t i = 0; i < std::size(kCommands); ++i) names[i] = kCommands[i].name;
    return closest(word, names);
}

bool has_option(const Command &c, const char *opt) {
    for (const char *const *o = c.options; *o; ++o)
        if (!std::strcmp(*o, opt)) return true;
    return false;
}

void hint_usage(const Command &c) {
    hint("usage: %s", c.synopsis);
    hint("'fif help %s' lists its options", c.name);
}

/* Something on the command line that the command does not take. */
void unknown_option(const Command &c, const char *a) {
    if (a[0] != '-') {
        report_error("fif %s: unexpected argument '%s'", c.name, a);
        hint("file names come first and there are no more of them; a file name with spaces needs quotes");
        hint_usage(c);
        return;
    }
    /* --quality=80: the value is a separate word here. */
    if (const char *eq = std::strchr(a, '=')) {
        const std::string name(a, eq);
        if (has_option(c, name.c_str())) {
            report_error("fif %s: write '%s %s' instead of '%s'", c.name, name.c_str(), eq + 1, a);
            return;
        }
    }
    report_error("fif %s: unknown option '%s'", c.name, a);
    std::string elsewhere;
    for (const Command &o : kCommands)
        if (&o != &c && has_option(o, a)) elsewhere += (elsewhere.empty() ? "fif " : ", fif ") + std::string(o.name);
    const char *closest_name = closest(a, c.options);
    if (closest_name) hint("did you mean %s?", closest_name);
    if (!elsewhere.empty())
        hint("%s is an option of %s, not of fif %s", a, elsewhere.c_str(), c.name);
    if (!closest_name && elsewhere.empty()) hint_usage(c);
}

/* The positional arguments.  Missing ones are named, and the most common
 * mistake - options before the file names - is recognised as such. */
bool need_files(const Command &c, int argc, char **argv) {
    const int want = c.required;
    for (int k = 0; k < want; ++k) {
        const int i = 2 + k;
        if (i < argc && argv[i][0] == '-' && argv[i][1] != '\0') {
            report_error("fif %s: options must come after the file names", c.name);
            hint_usage(c);
            return false;
        }
        if (i >= argc) {
            if (k == 0 && want == 2)
                report_error("fif %s needs two files: the %s and the %s", c.name, c.files[0], c.files[1]);
            else
                report_error("fif %s: the %s is missing", c.name, c.files[k]);
            hint_usage(c);
            return false;
        }
    }
    return true;
}

/* The value after an option.  A missing one is reported - it used to end the
 * run with exit code 2 and not a word. */
const char *take_value(int argc, char **argv, int &i) {
    const char *opt = argv[i];
    if (i + 1 >= argc || (argv[i + 1][0] == '-' && argv[i + 1][1] == '-')) {
        report_error("%s needs a value", opt);
        return nullptr;
    }
    return argv[++i];
}

/* A whole number in [lo, hi].  atoi took "8O" for 8 and "abc" for 0, and the
 * encode then ran with a setting nobody asked for; anything but a clean number
 * is refused now.  `advice` is printed on any failure. */
bool arg_int(int argc, char **argv, int &i, int *out, int lo, int hi,
             const char *advice = nullptr) {
    const char *opt = argv[i];
    const char *v = take_value(argc, argv, i);
    char range[64];
    if (hi == INT_MAX) std::snprintf(range, sizeof range, "%d or more", lo);
    else std::snprintf(range, sizeof range, "%d..%d", lo, hi);
    if (!v) {
        hint("give a whole number, %s", range);
        if (advice) hint("%s", advice);
        return false;
    }
    errno = 0;
    char *end = nullptr;
    const long x = std::strtol(v, &end, 10);
    if (end == v || *end != '\0' || errno == ERANGE) {
        report_error("%s wants a whole number (%s), got '%s'", opt, range, v);
        if (advice) hint("%s", advice);
        return false;
    }
    if (x < lo || x > hi) {
        report_error("%s %s is out of range; allowed is %s", opt, v, range);
        if (advice) hint("%s", advice);
        return false;
    }
    *out = (int)x;
    return true;
}

/* A number, with the range checked at the call site where the reason for it
 * is known. */
bool arg_dbl(int argc, char **argv, int &i, double *out, const char *advice = nullptr) {
    const char *opt = argv[i];
    const char *v = take_value(argc, argv, i);
    if (!v) { if (advice) hint("%s", advice); return false; }
    errno = 0;
    char *end = nullptr;
    const double x = std::strtod(v, &end);
    if (end == v || *end != '\0' || errno == ERANGE || !std::isfinite(x)) {
        report_error("%s wants a number, got '%s'", opt, v);
        if (advice) hint("%s", advice);
        return false;
    }
    *out = x;
    return true;
}

bool arg_i64(int argc, char **argv, int &i, long long *out, long long lo,
             const char *advice = nullptr) {
    const char *opt = argv[i];
    const char *v = take_value(argc, argv, i);
    if (!v) { if (advice) hint("%s", advice); return false; }
    errno = 0;
    char *end = nullptr;
    const long long x = std::strtoll(v, &end, 10);
    if (end == v || *end != '\0' || errno == ERANGE || x < lo) {
        report_error("%s wants a whole number of at least %lld, got '%s'", opt, lo, v);
        if (advice) hint("%s", advice);
        return false;
    }
    *out = x;
    return true;
}

/* --scale takes "N" to render N times larger, or "1/N" to render N times
 * smaller.  Spelling the reduction as a fraction rather than as a second
 * option keeps one axis in one place, and reads the way the result does. */
bool arg_scale(int argc, char **argv, int &i, int *num, int *den) {
    const char *v = take_value(argc, argv, i);
    if (!v) { hint("e.g. '--scale 2' renders twice as large, '--scale 1/4' a quarter"); return false; }
    char *end = nullptr;
    const long n = std::strtol(v, &end, 10);
    auto bad = [&]() {
        report_error("--scale wants N or 1/N, got '%s'", v);
        hint("e.g. '--scale 2' renders twice as large, '--scale 1/4' a quarter; "
             "for an exact pixel size use --width, --height or --size");
        return false;
    };
    if (end == v || n < 1) return bad();
    if (*end == '\0') {
        if (n > 16) {
            report_error("--scale %ld is too large; the decoder enlarges at most 16 times", n);
            hint("2 to 3 times gives the best results; enlarge further in an image editor if you must");
            return false;
        }
        *num = (int)n; *den = 1; return true;
    }
    if (*end != '/') return bad();
    const char *ds = end + 1;
    const long d = std::strtol(ds, &end, 10);
    if (end == ds || *end != '\0' || d < 1 || n != 1) return bad();
    *num = 1; *den = (int)d;
    return true;
}

/* A tile list: numbers and ranges, "0,3,5-8".  The same spelling as page
 * ranges in a print dialog, which is where people have met it before. */
bool parse_tile_list(const char *opt, const char *v, std::vector<int> &out) {
    out.clear();
    const char *s = v;
    while (*s) {
        char *end = nullptr;
        const long a = std::strtol(s, &end, 10);
        long b = a;
        bool ok = end != s && a >= 0 && std::isdigit((unsigned char)*s);
        if (ok && *end == '-') {
            const char *t = end + 1;
            b = std::strtol(t, &end, 10);
            ok = end != t && std::isdigit((unsigned char)*t) && b >= a;
        }
        ok = ok && (*end == ',' || *end == '\0') && b - a < (1 << 20);
        if (!ok) {
            report_error("%s wants tile numbers and ranges such as 0,3,5-8, got '%s'", opt, v);
            hint("'fif info <file> --tiles' lists the tiles with their numbers and positions");
            return false;
        }
        for (long k = a; k <= b; ++k) out.push_back((int)k);
        s = *end == ',' ? end + 1 : end;
    }
    if (out.empty()) {
        report_error("%s needs at least one tile number", opt);
        return false;
    }
    return true;
}

/* A region in the geometry spelling ImageMagick and X11 use: WxH+X+Y, the
 * size first, then where it starts. */
bool parse_region(const char *v, int *x, int *y, int *w, int *h) {
    char tail = 0;
    if (std::sscanf(v, "%dx%d+%d+%d%c", w, h, x, y, &tail) == 4 && *w > 0 && *h > 0 &&
        *x >= 0 && *y >= 0)
        return true;
    report_error("--region wants WIDTHxHEIGHT+X+Y, got '%s'", v);
    hint("e.g. --region 640x480+100+50: 640 by 480 pixels, starting 100 from the left and 50 "
         "from the top, in the picture's own pixels ('fif info' shows its size)");
    return false;
}

/* Megapixels per second, or nothing if the clock did not move. */
double rate(double mp, double secs) { return secs > 0 ? mp / secs : 0.0; }

std::string human(double bytes) {
    char buf[64];
    if (bytes < 1024) std::snprintf(buf, sizeof buf, "%.0f B", bytes);
    else if (bytes < 1024 * 1024) std::snprintf(buf, sizeof buf, "%.1f KiB", bytes / 1024);
    else std::snprintf(buf, sizeof buf, "%.2f MiB", bytes / (1024 * 1024));
    return buf;
}

/* A pixel count the way people say it: 1000, 2.1 million, 457 million. */
std::string count_text(double v) {
    char b[64];
    if (v < 1e6) std::snprintf(b, sizeof b, "%.0f", v);
    else std::snprintf(b, sizeof b, "%.3g million", v / 1e6);
    return b;
}

/* `decoding` is true for the one subcommand that also decodes, and gates the
 * two decode-side options.  Accepting an option and then ignoring it is worse
 * than refusing it. */
int parse_encode_opts(const Command &cmd, int argc, char **argv, int start,
                      fif_encode_options &o, bool *stats, bool decoding) {
    for (int i = start; i < argc; ++i) {
        const char *a = argv[i];
        if (!std::strcmp(a, "-q") || !std::strcmp(a, "--quality")) {
            if (!arg_int(argc, argv, i, &o.quality, 0, 100,
                         "lower means smaller files; 50-85 suits most photographs, 70 is the default"))
                return -1;
            g.quality_set = true;
        }
        else if (!std::strcmp(a, "-e") || !std::strcmp(a, "--effort")) {
            if (!arg_int(argc, argv, i, &o.effort, 0, 9,
                         "2 is the best speed/size compromise; above 5 the gain is small and slow"))
                return -1;
        }
        else if (!std::strcmp(a, "-t") || !std::strcmp(a, "--threads")) {
            if (!arg_int(argc, argv, i, &o.threads, 0, 4096, "0 uses 3/4 of the processor cores (all of 1 or 2)"))
                return -1;
        }
        else if (!std::strcmp(a, "--tile")) {
            if (!arg_int(argc, argv, i, &o.tile_size, 0, 1 << 20,
                         "0 picks the size automatically (256 or 512); larger tiles find better "
                         "matches, smaller ones run in parallel better"))
                return -1;
        }
        else if (!std::strcmp(a, "--min-block")) {
            if (!arg_int(argc, argv, i, &o.min_block, 2, 256, "a power of two: 2, 4, 8, ... 256 (default 4)"))
                return -1;
        }
        else if (!std::strcmp(a, "--max-block")) {
            if (!arg_int(argc, argv, i, &o.max_block, 2, 256, "a power of two: 2, 4, 8, ... 256 (default 32)"))
                return -1;
        }
        else if (!std::strcmp(a, "--target-psnr")) {
            const char *adv = "a quality in dB, e.g. 32; photographs typically reach 28-40 dB";
            if (!arg_dbl(argc, argv, i, &o.target_psnr, adv)) return -1;
            if (o.target_psnr <= 0 || o.target_psnr > 99) {
                report_error("--target-psnr %s is out of range; it wants a value in dB above 0", argv[i]);
                hint("%s", adv);
                return -1;
            }
        }
        else if (!std::strcmp(a, "--target-bpp")) {
            const char *adv = "bits per pixel, e.g. 1.0; the file size is about bpp x width x height / 8 bytes";
            if (!arg_dbl(argc, argv, i, &o.target_bpp, adv)) return -1;
            if (o.target_bpp <= 0 || o.target_bpp > 64) {
                report_error("--target-bpp %s is out of range; it wants a value above 0", argv[i]);
                hint("%s", adv);
                return -1;
            }
        }
        else if (!std::strcmp(a, "--local-radius")) {
            if (!arg_int(argc, argv, i, &o.local_radius, 0, 256, "domains within N grid steps; effort sets it otherwise"))
                return -1;
        }
        else if (!std::strcmp(a, "--kd-candidates")) {
            if (!arg_int(argc, argv, i, &o.kd_candidates, 0, 4096, "0 turns the feature-space search off"))
                return -1;
        }
        else if (!std::strcmp(a, "--domain-shift")) {
            if (!arg_int(argc, argv, i, &o.domain_shift, 0, 4,
                         "larger means a denser domain grid: better matches, more bits per index"))
                return -1;
        }
        else if (!std::strcmp(a, "--planar")) {
            if (!arg_int(argc, argv, i, &o.planar, 0, 1, "1 = flat blocks may carry a gradient (default), 0 = off"))
                return -1;
        }
        else if (!std::strcmp(a, "--thumbnail")) {
            if (!arg_int(argc, argv, i, &g.thumb_size, 0, 65535,
                         "the longer edge of the preview in pixels, e.g. 256; 0 = no preview"))
                return -1;
        }
        else if (!std::strcmp(a, "--thumbnail-quality")) {
            if (!arg_int(argc, argv, i, &g.thumb_quality, 1, 100, "JPEG quality of the preview, default 85"))
                return -1;
        }
        else if (!std::strcmp(a, "--meta") || !std::strcmp(a, "--text") ||
                 !std::strcmp(a, "--misc") || !std::strcmp(a, "--chunk")) {
            const char *v = take_value(argc, argv, i);
            const char *kv = v && (size_t)i < g.argv_utf8.size() ? g.argv_utf8[(size_t)i].c_str() : v;
            /* --chunk carries its own type: TYPE:KEY=VALUE, the type up to
             * four characters, padded with spaces as four-character codes are. */
            char type[5] = "    ";
            const bool own_type = a[2] == 'c';
            if (own_type && kv) {
                const char *colon = std::strchr(kv, ':');
                const size_t tl = colon ? (size_t)(colon - kv) : 0;
                bool ok = colon && tl >= 1 && tl <= 4;
                for (size_t k = 0; ok && k < tl; ++k)
                    ok = kv[k] > 0x20 && kv[k] < 0x7F;
                if (!ok) {
                    report_error("--chunk wants TYPE:KEY=VALUE with a type of 1-4 printable ASCII "
                                 "characters, got '%s'", kv);
                    hint("e.g. --chunk ACME:render-hint=soft; the predefined types have their own "
                         "options: --meta, --text, --misc");
                    return -1;
                }
                std::memcpy(type, kv, tl);
                kv = colon + 1;
            } else {
                std::memcpy(type, a[2] == 'm' ? (a[3] == 'e' ? "META" : "MISC") : "TEXT", 4);
            }
            const char *eq = kv ? std::strchr(kv, '=') : nullptr;
            if (!eq || eq == kv) {
                if (kv) report_error("%s wants %sKEY=VALUE, got '%s'", a, own_type ? "TYPE:" : "", kv);
                hint("e.g. %s %sauthor=Ada; quote it when the value has spaces: %s \"%scopyright=(c) 2026 Ada\"",
                     a, own_type ? "ACME:" : "", a, own_type ? "ACME:" : "");
                return -1;
            }
            if (eq - kv > 0xFFFF) {
                report_error("the key of %s is longer than 65535 bytes", a);
                return -1;
            }
            fif_chunk c{};
            std::memcpy(c.type, type, 5);
            c.value_type = FIF_VALUE_TEXT;
            /* Stored as "key\0value" so the one string owns both halves;
             * the pointers are fixed up once all options are read. */
            g.chunk_text.push_back(std::string(kv, eq) + '\0' + (eq + 1));
            g.chunks.push_back(c);
        }
        else if (!std::strcmp(a, "--compress")) {
            const char *v = take_value(argc, argv, i);
            int bits = 0;
            bool ok = v != nullptr;
            for (const char *t = v; ok && *t;) {
                const char *e = std::strchr(t, ',');
                const std::string w(t, e ? (size_t)(e - t) : std::strlen(t));
                if (w == "chunks") bits |= FIF_SECTION_CHUNKS;
                else if (w == "thumbnails") bits |= FIF_SECTION_THUMBNAILS;
                else if (w == "fic") bits |= FIF_SECTION_FIC;
                else if (w == "all") bits |= FIF_SECTION_CHUNKS | FIF_SECTION_THUMBNAILS | FIF_SECTION_FIC;
                else ok = false;
                t = e ? e + 1 : t + w.size();
            }
            if (!ok) {
                if (v) report_error("--compress wants sections separated by commas, got '%s'", v);
                hint("the sections are chunks, thumbnails and fic, or all; e.g. --compress chunks");
                return -1;
            }
            if (!fif::have_zlib()) {
                report_error("--compress needs zlib, and this fif was built without it");
                hint("install zlib so that pkg-config finds it and rebuild with 'make clean && make'");
                return -1;
            }
            o.compress |= bits;
        }
        else if (!std::strcmp(a, "--no-credits")) { o.omit_credits = 1; }
        else if (!decoding && !std::strcmp(a, "--base-only")) { o.base_only = 1; }
        else if (!decoding && !std::strcmp(a, "--no-chunks")) { o.omit_chunks = 1; }
        else if (!std::strcmp(a, "--no-exif")) { g.no_exif = true; }
        else if (!std::strcmp(a, "--remarks")) {
            const char *v = take_value(argc, argv, i);
            const size_t len = v ? std::strlen(v) : 0;
            bool ok = v && len >= 1 && len <= 4;
            for (size_t k = 0; ok && k < len; ++k) ok = v[k] >= 0x20 && v[k] < 0x7F;
            if (!ok) {
                if (v) report_error("--remarks wants 1 to 4 printable ASCII characters, got '%s'", v);
                hint("e.g. --remarks CHKD; shorter values are padded with spaces");
                return -1;
            }
            std::memcpy(o.remarks, "    ", 4);
            std::memcpy(o.remarks, v, len);
        }
        else if (!std::strcmp(a, "--train-contexts") && !decoding) {
            if (!(g.train_path = take_value(argc, argv, i))) return -1;
        }
        /* Downscale before encoding.  Mutually exclusive; last one wins is
         * too surprising, so the others are cleared as each is set. */
        else if (!std::strcmp(a, "--downscale")) {
            const char *adv = "a factor of at least 1: 2 halves width and height; "
                              "--max-dim N caps the longer edge instead";
            if (!arg_dbl(argc, argv, i, &g.down_factor, adv)) return -1;
            if (g.down_factor < 1.0) {
                report_error("--downscale %s would enlarge the image; it takes a factor of at least 1", argv[i]);
                hint("fif only reduces before encoding; to get a larger picture, decode with --scale");
                return -1;
            }
            g.down_width = g.down_height = g.down_max = 0;
        }
        else if (!std::strcmp(a, "--width")) {
            if (!arg_int(argc, argv, i, &g.down_width, 1, 1 << 20, "the width to reduce the input to, in pixels"))
                return -1;
            g.down_factor = 0; g.down_height = g.down_max = 0;
        }
        else if (!std::strcmp(a, "--height")) {
            if (!arg_int(argc, argv, i, &g.down_height, 1, 1 << 20, "the height to reduce the input to, in pixels"))
                return -1;
            g.down_factor = 0; g.down_width = g.down_max = 0;
        }
        else if (!std::strcmp(a, "--max-dim")) {
            if (!arg_int(argc, argv, i, &g.down_max, 1, 1 << 20,
                         "the longest edge in pixels, e.g. 1920; smaller images are left alone"))
                return -1;
            g.down_factor = 0; g.down_width = g.down_height = 0;
        }
        else if (!std::strcmp(a, "--subsampling")) {
            const char *v = take_value(argc, argv, i);
            if (v && !std::strcmp(v, "444")) o.subsampling = FIF_SUBSAMPLING_444;
            else if (v && !std::strcmp(v, "420")) o.subsampling = FIF_SUBSAMPLING_420;
            else if (v && !std::strcmp(v, "400")) o.subsampling = FIF_SUBSAMPLING_400;
            else {
                if (v) report_error("unknown --subsampling value '%s'", v);
                hint("use 444 (full colour resolution), 420 (half, like JPEG; the default) "
                     "or 400 (greyscale)%s", v && std::strchr(v, ':') ? " - without the colons" : "");
                return -1;
            }
        } else if (!std::strcmp(a, "--chroma")) {
            const char *v = take_value(argc, argv, i);
            if (v && !std::strcmp(v, "shared")) o.chroma_mode = FIF_CHROMA_SHARED;
            else if (v && !std::strcmp(v, "from-luma")) o.chroma_mode = FIF_CHROMA_FROM_LUMA;
            else {
                if (v) report_error("unknown --chroma mode '%s'", v);
                hint("use shared (the default) or from-luma (faster decoding, slightly larger)");
                return -1;
            }
        } else if (!std::strcmp(a, "--stats")) { if (stats) *stats = true; }
        else if (!std::strcmp(a, "--progress")) { g.progress = 1; }
        else if (!std::strcmp(a, "--no-progress")) { g.progress = 0; }
        else if (decoding && !std::strcmp(a, "--deblock")) {
            if (!arg_int(argc, argv, i, &g.bench_deblock, -1, 100,
                         "-1 = the file's recommendation, 0 = off, 100 = strongest"))
                return -1;
        }
        else if (decoding && !std::strcmp(a, "--relaxation")) {
            if (!arg_dbl(argc, argv, i, &g.bench_relax, "1 is the default and the measured optimum"))
                return -1;
        }
        else { unknown_option(cmd, a); return -1; }
    }

    /* Checks that need every option: the block sizes are a pair, and two
     * targets cannot both be met. */
    auto pow2 = [](int v) { return v > 0 && (v & (v - 1)) == 0; };
    if (!pow2(o.min_block) || !pow2(o.max_block)) {
        const bool lo = !pow2(o.min_block);
        report_error("--%s %d is not a power of two", lo ? "min-block" : "max-block",
                     lo ? o.min_block : o.max_block);
        hint("use 2, 4, 8, 16, 32, 64, 128 or 256");
        return -1;
    }
    if (o.min_block > o.max_block) {
        report_error("--min-block %d is larger than --max-block %d", o.min_block, o.max_block);
        hint("raise --max-block as well, or lower --min-block (defaults: 4 and 32)");
        return -1;
    }
    if (o.target_bpp > 0 && o.target_psnr > 0)
        report_warning("--target-bpp and --target-psnr both given; --target-bpp decides, "
                       "--target-psnr is ignored");
    if ((o.target_bpp > 0 || o.target_psnr > 0) && g.quality_set)
        report_warning("-q is ignored: --target-%s chooses the quality itself",
                       o.target_bpp > 0 ? "bpp" : "psnr");
    if (o.base_only && (o.target_bpp > 0 || o.target_psnr > 0)) {
        report_error("--base-only cannot be combined with --target-%s",
                     o.target_bpp > 0 ? "bpp" : "psnr");
        hint("the target is measured on the full file; use -q to set the quality instead");
        return -1;
    }
    if (o.base_only && g.thumb_size > 0) {
        report_warning("--thumbnail is ignored: --base-only stores no preview");
        g.thumb_size = 0;
    }
    if (o.omit_chunks && (!g.chunks.empty() || o.omit_credits))
        report_warning("--no-chunks leaves out every chunk, so --meta, --text, --misc, --chunk "
                       "and --no-credits have no effect");
    if (g.thumb_size > 0 && !kHaveJpeg) {
        report_error("--thumbnail needs JPEG support, and this fif was built without it");
        hint_missing_library(Kind::Jpeg);
        hint("or leave out --thumbnail");
        return -1;
    }
    if (decoding && g.bench_relax > 1.2)
        report_warning("--relaxation %.3g: above about 1.2 the iteration stops converging and "
                       "the picture breaks up", g.bench_relax);
    return 0;
}

/* A progress bar, on stderr, only when stderr is a terminal: piping the tool
 * into anything - the benchmark scripts included - must not put control
 * characters in the output.  Overwrites one line and never scrolls. */
struct Bar {
    const char *label;
    int last = -1;
    static void tick(void *user, double f) {
        Bar *b = (Bar *)user;
        const int pct = (int)(f * 100.0 + 0.5);
        if (pct == b->last) return;
        b->last = pct;
        const int w = 28, fill = pct * w / 100;
        std::fprintf(stderr, "\r%-8s [", b->label);
        for (int i = 0; i < w; ++i) std::fputc(i < fill ? '#' : ' ', stderr);
        std::fprintf(stderr, "] %3d%%", pct);
        std::fflush(stderr);
    }
    void done() {
        if (last >= 0) { std::fprintf(stderr, "\r\033[K"); std::fflush(stderr); }
    }
};

bool want_progress() {
    if (g.progress == 0 || g.quiet) return false;
    return g.progress > 0 || isatty(2);
}

void announce_threads(int threads) {
    const int n = fif_thread_count(threads), cores = fif_cpu_cores();
    if (cores > 0) note("using %d of %d threads\n", n, cores);
    else note("using %d threads\n", n);
}

const char *chroma_name(int m) {
    switch (m) {
    case FIF_CHROMA_FROM_LUMA:   return "from-luma";
    default:                     return "shared";
    }
}

const char *subsampling_name(int s) {
    switch (s) {
    case FIF_SUBSAMPLING_420: return "4:2:0";
    case FIF_SUBSAMPLING_400: return "4:0:0";
    default:                  return "4:4:4";
    }
}

/* 4:0:0 gives back one channel where the source had three; the metrics compare
 * like for like, so drop the source to luma rather than let them refuse. */
fif_image to_luma(const fif_image &im, std::vector<uint8_t> &store) {
    if (im.channels == 1) return im;
    store.resize((size_t)im.width * im.height);
    for (int y = 0; y < im.height; ++y) {
        const uint8_t *r = im.pixels + (size_t)y * im.stride;
        for (int x = 0; x < im.width; ++x) {
            const uint8_t *px = r + (size_t)x * im.channels;
            store[(size_t)y * im.width + x] = (uint8_t)std::lround(
                0.299 * px[0] + 0.587 * px[1] + 0.114 * px[2]);
        }
    }
    fif_image g = im;
    g.channels = 1;
    g.stride = (size_t)im.width;
    g.pixels = store.data();
    return g;
}

/* Bit budget per symbol class, and the average cost of one instance - the
 * numbers the rate-distortion search needs to estimate its own output. */
void print_bits(const fif_stats &st) {
    const double tot = st.bits_split + st.bits_mode + st.bits_dc +
                       st.bits_domain + st.bits_sym + st.bits_contrast +
                       st.bits_planar + st.bits_grad;
    if (tot <= 0) return;
    const double leaves = (double)st.blocks_total;
    const double frac = (double)st.blocks_fractal;
    struct Row { const char *name; double bits; double per; };
    const Row rows[] = {
        {"split",    st.bits_split,    leaves},
        {"mode",     st.bits_mode,     leaves},
        {"dc",       st.bits_dc,       leaves},
        {"domain",   st.bits_domain,   frac},
        {"symmetry", st.bits_sym,      frac},
        {"contrast", st.bits_contrast, frac},
        {"planar",   st.bits_planar,   leaves - frac},
        {"gradient", st.bits_grad,     leaves - frac},
    };
    std::printf("  bit budget\n");
    for (const Row &r : rows)
        std::printf("    %-9s %6.2f %%   %7.3f bits each\n",
                    r.name, 100.0 * r.bits / tot,
                    r.per > 0 ? r.bits / r.per : 0.0);
}

void print_stats(const fif_stats &st, const fif_image &img) {
    const double mp = (double)img.width * img.height / 1e6;
    std::fprintf(stderr,
        "  blocks       %lld total, %lld fractal, %lld flat\n"
        "  evaluations  %lld (range,domain,symmetry) triples\n"
        "  size         %s  (%.4f bpp)\n"
        "  encode       %.3f s  (%.2f Mpixel/s), search %.3f s\n",
        (long long)st.blocks_total, (long long)st.blocks_fractal,
        (long long)st.blocks_flat, (long long)st.candidate_evals,
        human((double)st.compressed_bytes).c_str(), st.bits_per_pixel,
        st.encode_seconds, rate(mp, st.encode_seconds), st.search_seconds);
    print_bits(st);
}

int write_file(const char *path, const uint8_t *d, size_t n) {
    FILE *f = std::fopen(path, "wb");
    if (!f) { explain_write_error(path, errno); return kExitFail; }
    const bool ok = std::fwrite(d, 1, n, f) == n;
    const int werr = errno;
    const bool closed = std::fclose(f) == 0;
    if (!ok || !closed) {
        explain_write_error(path, ok ? errno : werr);
        hint("'%s' is incomplete; delete it", path);
        return kExitFail;
    }
    return 0;
}

uint8_t *read_file(const char *path, size_t *n) {
    if (!check_readable(path)) return nullptr;
    FILE *f = std::fopen(path, "rb");
    if (!f) { explain_open_error(path, errno); return nullptr; }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        report_error("cannot read '%s': its size cannot be determined", path);
        std::fclose(f);
        return nullptr;
    }
    uint8_t *buf = (uint8_t *)std::malloc((size_t)sz);
    if (!buf) {
        report_error("not enough memory to load '%s'", path);
        std::fclose(f);
        return nullptr;
    }
    if (std::fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        report_error("cannot read '%s': %s", path, std::strerror(errno));
        std::free(buf); std::fclose(f); return nullptr;
    }
    std::fclose(f);
    *n = (size_t)sz;
    return buf;
}

/* Header and chunk fields that come from the command line rather than from
 * the encoder: where the image came from, the preview, the user's chunks. */
void apply_container_opts(fif_encode_options &o, const char *in_path, const fif_image &img) {
    o.source_name = base_name(g.argv_utf8.size() > 2 ? g.argv_utf8[2].c_str() : in_path);
    o.source_width = g.src_width > 0 ? g.src_width : img.width;
    o.source_height = g.src_height > 0 ? g.src_height : img.height;
    o.source_scale = g.src_scale;
    /* What the source cost on disk.  A path we can no longer stat - the file
     * was read a moment ago - simply leaves the chunk out. */
    struct stat sb;
    o.source_bytes = stat(in_path, &sb) == 0 ? (int64_t)sb.st_size : 0;
    for (size_t k = 0; k < g.chunks.size(); ++k) {
        const std::string &t = g.chunk_text[k];
        const size_t z = t.find('\0');
        g.chunks[k].name = t.c_str();
        g.chunks[k].name_len = z;
        g.chunks[k].data = (const uint8_t *)t.c_str() + z + 1;
        g.chunks[k].size = t.size() - z - 1;
    }
    o.chunks = g.chunks.empty() ? nullptr : g.chunks.data();
    o.chunk_count = (int)g.chunks.size();
    if (g.thumb_size > 0) {
        const int cap = std::max(img.width, img.height) / 3;
        if (cap < 1) {
            report_warning("the image is too small for a preview (under 3 pixels); none stored");
            g.thumb_size = 0;
        } else if (g.thumb_size > cap) {
            report_warning("--thumbnail %d reduced to %d px, a third of the image's longer edge",
                           g.thumb_size, cap);
            g.thumb_size = cap;
        }
    }
    o.thumbnail_size = g.thumb_size;
    o.thumbnail_quality = g.thumb_quality;
}

/* A file that should be FIF2 could not be used.  "malformed stream" is true
 * but useless; this works out which of the usual reasons it is - not FIF2 at
 * all, another container version, a damaged header, a cut-off file, damaged
 * image data - and what to do about each. */
void explain_fif_error(const char *path, const uint8_t *d, size_t n, fif_status st) {
    if (st == FIF_ERR_NOMEM) {
        report_error("not enough memory to decode '%s'", path);
        hint("decode smaller with --scale 1/2 (or 1/4), or use fewer threads with -t 1");
        return;
    }
    if (st == FIF_ERR_ENCRYPTED) {
        report_error("the image data of '%s' is encrypted; a password is needed to unpack it", path);
        hint("this fif cannot decrypt; open the file with the program that encrypted it");
        return;
    }
    if (st == FIF_ERR_UNSUPPORTED) {
        fif_info in{};
        if (fif_get_info(d, n, &in) == FIF_OK && (in.compressed & FIF_SECTION_FIC) &&
            !fif::have_zlib()) {
            report_error("the image data of '%s' is zlib-compressed, and this fif was built without zlib",
                         path);
            hint("install zlib so that pkg-config finds it and rebuild with 'make clean && make'");
            return;
        }
    }
    if (st != FIF_ERR_FORMAT) {
        report_error("cannot use '%s': %s", path, fif_status_string(st));
        return;
    }
    const Kind k = sniff_bytes(d, n);
    if (k != Kind::Fif2) {
        if (k != Kind::Unknown) {
            report_error("'%s' is a %s image, not a FIF2 file", path, kind_name(k));
            hint("to compress it: fif encode \"%s\"", path);
        } else {
            report_error("'%s' is not a FIF2 file", path);
            char head[40] = {};
            for (size_t i = 0; i < 4 && i < n; ++i)
                std::snprintf(head + 3 * i, sizeof head - 3 * i, "%02X ", d[i]);
            hint("a FIF2 file starts with the bytes 46 49 46 32 (\"FIF2\"); this one starts with %s", head);
            hint("an interrupted download, an HTML error page or a renamed file look like this");
        }
        return;
    }
    if (n >= 6 && (d[4] != fif::kVerMajor || d[5] < fif::kVerMinor)) {
        report_error("'%s' uses format version %d.%d; this fif reads version %d.%d and later %d.x",
                     path, d[4], d[5], fif::kVerMajor, fif::kVerMinor, fif::kVerMajor);
        /* Versions below 3.2 were the layouts of development versions. */
        if (d[4] < fif::kVerMajor || (d[4] == fif::kVerMajor && d[5] < fif::kVerMinor))
            hint("it was written by a development version of libfif; encode it again from the original image");
        else
            hint("it was written by a newer libfif; update fif to read it");
        return;
    }
    if (const char *why = fif::container_problem(d, n)) {
        if (std::strstr(why, "ends inside the header")) {
            report_error("'%s' is cut off: it ends inside the header (%zu bytes)", path, n);
            hint("the file is incomplete; download or copy it again");
        } else if (std::strstr(why, "checksum")) {
            report_error("'%s' is damaged: the header checksum does not match", path);
            hint("the first bytes of the file were altered, e.g. by a transfer in text mode or a "
                 "storage fault; copy it again from the source");
        } else if (std::strstr(why, "newer")) {
            report_error("'%s' needs a newer fif: %s", path, why);
            hint("update libfif");
        } else {
            report_error("'%s' is damaged: %s", path, why);
            hint("the header holds values no encoder writes; the file is corrupt or was not made by libfif");
        }
        return;
    }
    /* The header is fine, so the image data is not. */
    fif_info in{};
    const bool have_info = fif_get_info(d, n, &in) == FIF_OK;
    if (have_info && in.truncated) {
        report_error("'%s' is cut off too early to decode: %zu of %lld bytes present", path,
                     n, (long long)in.file_size);
        if (in.base_prefix_bytes > (int64_t)n)
            hint("a first, coarse picture needs the first %lld bytes", (long long)in.base_prefix_bytes);
        hint("download or copy the file again");
        if (in.thumbnail_count > 0 && in.thumbnail_crc_ok != -1)
            hint("the stored preview is complete: fif thumbnail \"%s\"", path);
        return;
    }
    report_error("'%s' is damaged: the image data does not decode", path);
    if (have_info && in.fic_crc_ok == 0)
        hint("the checksum of the image data does not match, so the file changed after it was written");
    hint("'fif info \"%s\"' shows which parts are intact", path);
    if (have_info && in.thumbnail_count > 0)
        hint("the stored preview may still be fine: fif thumbnail \"%s\"", path);
    hint("if you have the original image, encode it again");
}

/* Reads a file that is meant to be FIF2 and checks its header.  Null after
 * reporting what is wrong. */
uint8_t *read_fif(const char *path, size_t *n) {
    uint8_t *data = read_file(path, n);
    if (!data) return nullptr;
    fif_info in{};
    const fif_status st = fif_get_info(data, *n, &in);
    if (st != FIF_OK) {
        explain_fif_error(path, data, *n, st);
        std::free(data);
        return nullptr;
    }
    return data;
}

/* What a successful read of a FIF2 file still should not keep quiet about. */
void warn_fif_state(const char *path, const uint8_t *data, size_t n) {
    fif_info in{};
    if (fif_get_info(data, n, &in) != FIF_OK) return;
    if (in.truncated)
        report_warning("'%s' is cut off (%zu of %lld bytes); the picture is decoded at reduced quality",
                       path, n, (long long)in.file_size);
    else if (in.fic_crc_ok == 0)
        report_warning("'%s': the checksum of the image data does not match; parts of the "
                       "picture may be wrong", path);
}

/* After a --target encode: say which quality was chosen, and say so loudly
 * when the target was out of reach - the library then returns the closest
 * result, which is a fine file but not the one that was asked for. */
void report_target(const fif_encode_options &o, const fif_image &img,
                   const uint8_t *data, size_t n, double bpp) {
    fif_info in{};
    const int q = fif_get_info(data, n, &in) == FIF_OK ? in.quality : -1;
    if (o.target_bpp > 0) {
        note("target      %.4g bpp -> quality %d, %.4f bpp\n", o.target_bpp, q, bpp);
        if (bpp > o.target_bpp * 1.02) {
            report_warning("%.4g bpp is not reachable for this image; even quality %d needs %.4f bpp",
                           o.target_bpp, q, bpp);
            hint("for smaller files reduce the pixel count (--max-dim, --downscale) or drop the "
                 "colour (--subsampling 400)");
        }
        return;
    }
    /* A PSNR target needs a decode to check; it is a fraction of the encode. */
    fif_decode_options d;
    fif_decode_options_defaults(&d);
    d.threads = o.threads;
    fif_image dec{};
    if (fif_decode(data, n, &d, &dec, nullptr) != FIF_OK) return;
    std::vector<uint8_t> luma;
    const fif_image ref = dec.channels == img.channels ? img : to_luma(img, luma);
    const double psnr = fif_psnr(&ref, &dec);
    fif_image_free(&dec);
    note("target      %.4g dB -> quality %d, %.2f dB\n", o.target_psnr, q, psnr);
    if (psnr < o.target_psnr - 0.05) {
        report_warning("%.4g dB is not reachable for this image; the best result has %.2f dB",
                       o.target_psnr, psnr);
        hint("fractal coding levels off: near the top, more bytes buy almost no quality. A smaller "
             "image (--max-dim), --subsampling 444 or more effort (-e 7) raise the ceiling a little");
        hint("this file is the largest the search produced; with a lower target it gets much smaller");
    }
}

/* What the file turned out to be, against what was asked for.  Three things
 * the encoder decides quietly and the caller would want to know: a tile size
 * that had to be rounded, a compressed section that came out no smaller than
 * the plain one, and - for the FIC section - the byte-range access that
 * compressing it gives up. */
void warn_about_result(const fif_encode_options &o, const uint8_t *out, size_t n) {
    fif_info fi{};
    if (fif_get_info(out, n, &fi) != FIF_OK) return;

    if (o.tile_size > 0 && fi.tile_size != o.tile_size) {
        report_warning("--tile %d was rounded up to %d", o.tile_size, fi.tile_size);
        hint("a tile is a whole number of largest blocks (%d) across, doubled for "
             "4:2:0 colour", fi.max_block);
    }
    if (!fi.compressed) return;

    uint8_t *flat = nullptr;
    size_t flat_n = 0;
    if (fif_unpack(out, n, &flat, &flat_n) != FIF_OK) return;
    fif_info uf{};
    if (fif_get_info(flat, flat_n, &uf) == FIF_OK) {
        const struct { const char *name; int bit; int64_t stored, plain; } sec[] = {
            {"chunks",     FIF_SECTION_CHUNKS,     fi.chunk_bytes,     uf.chunk_bytes},
            {"thumbnails", FIF_SECTION_THUMBNAILS, fi.thumbnail_bytes, uf.thumbnail_bytes},
            {"fic",        FIF_SECTION_FIC,        fi.fic_bytes,       uf.fic_bytes},
        };
        for (const auto &s : sec) {
            if (!(fi.compressed & s.bit) || s.stored < s.plain) continue;
            report_warning("--compress %s made that section larger by %lld bytes "
                           "(%s stored against %s plain)",
                           s.name, (long long)(s.stored - s.plain),
                           human((double)s.stored).c_str(),
                           human((double)s.plain).c_str());
            if (s.bit == FIF_SECTION_FIC)
                hint("range-coded image data does not compress; leave 'fic' out of --compress");
        }
        if (fi.compressed & FIF_SECTION_FIC)
            hint("compressed image data can no longer be fetched in parts by byte ranges");
    }
    fif_free(flat);
}

int cmd_encode(const Command &cmd, int argc, char **argv) {
    /* `fif encode photo.png`: without an output name the output is the input
     * with the extension .fif, next to it.  Inserted into the argument list
     * so that everything below sees the usual two names. */
    std::string auto_out;
    std::vector<char *> args;
    if (argc >= 3 && argv[2][0] != '-' && (argc == 3 || argv[3][0] == '-')) {
        const char *in = argv[2];
        const char *b = base_name(in);
        const char *dot = std::strrchr(b, '.');
        auto_out.assign(in, dot && dot != b ? dot : in + std::strlen(in));
        auto_out += ".fif";
        args.assign(argv, argv + argc);
        args.insert(args.begin() + 3, auto_out.data());
        if (g.argv_utf8.size() > 2) {
            std::string u = g.argv_utf8[2];
            const char *ub = base_name(u.c_str());
            const char *ud = std::strrchr(ub, '.');
            if (ud && ud != ub) u.erase((size_t)(ud - u.c_str()));
            g.argv_utf8.insert(g.argv_utf8.begin() + 3, u + ".fif");
        }
        argv = args.data();
        ++argc;
    }
    if (!need_files(cmd, argc, argv)) return kExitUsage;
    fif_encode_options o;
    fif_encode_options_defaults(&o);
    bool want_stats = false;
    if (parse_encode_opts(cmd, argc, argv, 4, o, &want_stats, false) != 0) return kExitUsage;

    const char *in_path = argv[2], *out_path = argv[3];
    /* What an encode writes is a FIF2 file, so its name ends in .fif.  A name
     * that does not say so gets the extension appended, before anything asks
     * whether that file exists: the question, and the -y that answers it,
     * have to be about the file that will really be written.  A name that
     * claims to be a picture is refused below instead - appending there
     * would leave something like photo.png.fif. */
    std::string out_fixed;
    if (ext_lower(out_path) != "fif" && kind_from_ext(out_path) == Kind::Unknown) {
        out_fixed = out_path;
        while (!out_fixed.empty() && out_fixed.back() == '.') out_fixed.pop_back();
        out_fixed += ".fif";
        note("target      %s -> %s\n", out_path, out_fixed.c_str());
        out_path = out_fixed.c_str();
        if (g.argv_utf8.size() > 3) g.argv_utf8[3] += ".fif";
    }
    if (same_file(in_path, out_path)) {
        report_error("the output '%s' is the input file; encoding would overwrite your original", out_path);
        if (ext_lower(in_path) == "fif") {
            std::string stem(in_path, std::strlen(in_path) - 4);
            hint("choose a different output name, e.g. %s-2.fif", stem.c_str());
        } else {
            hint("choose a different output name, e.g. %s.fif", in_path);
        }
        return kExitUsage;
    }
    const Kind out_kind = kind_from_ext(out_path);
    if (out_kind != Kind::Fif2 && out_kind != Kind::Unknown) {
        report_error("the output '%s' is named like a %s image, but it will hold FIF2 data",
                     out_path, kind_name(out_kind));
        hint("name it .fif; to turn a FIF2 file into an image, use fif decode");
        return kExitUsage;
    }
    if (!check_readable(in_path) || !check_writable(out_path) || !may_write(out_path) ||
        (g.train_path && !may_write(g.train_path)))
        return kExitFail;

    if (g.train_path) fif_context_training_begin();

    fif_image img{};
    fif_status st = fif_image_load(in_path, &img);
    if (st != FIF_OK) {
        explain_image_load(in_path, st);
        return kExitFail;
    }
    if (!apply_exif(in_path, &img) || apply_downscale(&img) != 0) {
        fif_image_free(&img);
        return kExitFail;
    }
    /* 4:0:0 is not a subsampling of the colour, it is the absence of it: the
     * file is written as a greyscale picture and the colour is gone for good.
     * Worth one line, as the thumbnail limit gets one. */
    if (o.subsampling == FIF_SUBSAMPLING_400 && img.channels == 3) {
        report_warning("--subsampling 400 stores '%s' as greyscale; its colour is discarded",
                       base_name(in_path));
        hint("use --subsampling 420 for photographs, or 444 for graphics, to keep the colour");
    }
    apply_container_opts(o, in_path, img);
    uint8_t *out = nullptr;
    size_t n = 0;
    fif_stats stats{};
    Bar bar{"encode"};
    if (want_progress()) { o.progress = Bar::tick; o.progress_user = &bar; }
    announce_threads(o.threads);
    st = fif_encode(&img, &o, &out, &n, &stats);
    bar.done();
    if (st != FIF_OK) {
        report_error("encoding '%s' failed: %s", in_path, fif_status_string(st));
        if (st == FIF_ERR_NOMEM)
            hint("not enough memory: reduce the image (--max-dim), use fewer threads (-t 1) or smaller tiles (--tile 256)");
        else if (st == FIF_ERR_UNSUPPORTED)
            hint("the input must be 8-bit greyscale or RGB");
        else if (st == FIF_ERR_ARG)
            hint("an option combination the encoder refuses; try again without the specialist options");
        fif_image_free(&img);
        return kExitFail;
    }
    int rc = write_file(out_path, out, n);
    if (!rc) {
        result("%s: %dx%d -> %s (%.4f bpp) in %.3f s\n",
             out_path, img.width, img.height, human((double)n).c_str(),
             stats.bits_per_pixel, stats.encode_seconds);
        warn_about_result(o, out, n);
    }
    if (!rc && (o.target_bpp > 0 || o.target_psnr > 0))
        report_target(o, img, out, n, stats.bits_per_pixel);
    if (want_stats) print_stats(stats, img);
    if (g.train_path && !fif_context_training_write(g.train_path)) {
        explain_write_error(g.train_path, errno);
        rc = kExitFail;
    }
    fif_free(out);
    fif_image_free(&img);
    return rc;
}

/* A chunk's value as text: the text itself (at most `max_text` bytes), a
 * number, or its size for anything else. */
std::string chunk_value_text(const fif_chunk &c, size_t max_text = 400) {
    const uint64_t sz = c.size;
    char b[64];
    if (c.value_type == FIF_VALUE_TEXT)
        return std::string((const char *)c.data, (size_t)std::min<uint64_t>(sz, max_text));
    if ((c.value_type == FIF_VALUE_UINT || c.value_type == FIF_VALUE_INT) &&
        (sz == 1 || sz == 2 || sz == 4 || sz == 8)) {
        uint64_t u = 0;
        for (uint64_t k = 0; k < sz; ++k) u |= (uint64_t)c.data[k] << (8 * k);
        if (c.value_type == FIF_VALUE_INT && sz < 8 && (u >> (8 * sz - 1)) & 1)
            u |= ~0ull << (8 * sz);
        if (c.value_type == FIF_VALUE_INT) std::snprintf(b, sizeof b, "%lld", (long long)u);
        else std::snprintf(b, sizeof b, "%llu", (unsigned long long)u);
    } else if (c.value_type == FIF_VALUE_FLOAT && (sz == 4 || sz == 8)) {
        double d;
        if (sz == 4) { float f; std::memcpy(&f, c.data, 4); d = f; }
        else std::memcpy(&d, c.data, 8);
        std::snprintf(b, sizeof b, "%g", d);
    } else {
        std::snprintf(b, sizeof b, "%llu bytes", (unsigned long long)sz);
    }
    return b;
}

/* The decoder options fif decode and fif debug share: 1 = taken, 0 = not
 * one of them, -1 = taken but wrong, and reported. */
int decode_option(int argc, char **argv, int &i, fif_decode_options &o) {
    const char *a = argv[i];
    const auto ok = [](bool taken) { return taken ? 1 : -1; };
    if (!std::strcmp(a, "--scale"))
        return ok(arg_scale(argc, argv, i, &o.scale_num, &o.scale_den));
    if (!std::strcmp(a, "-t") || !std::strcmp(a, "--threads"))
        return ok(arg_int(argc, argv, i, &o.threads, 0, 4096,
                          "0 uses 3/4 of the processor cores (all of 1 or 2)"));
    if (!std::strcmp(a, "--iterations"))
        return ok(arg_int(argc, argv, i, &o.max_iterations, 0, 100000,
                          "0 = the file's recommendation, else 16; fewer is faster but less converged"));
    if (!std::strcmp(a, "--deblock"))
        return ok(arg_int(argc, argv, i, &o.deblock, -1, 100,
                          "-1 = the file's recommendation (the default), 0 = off, 100 = strongest"));
    if (!std::strcmp(a, "--eps")) {
        const char *adv = "the largest pixel change at which to stop, e.g. 0.5; 0 = the file's recommendation";
        if (!arg_dbl(argc, argv, i, &o.convergence, adv)) return -1;
        if (o.convergence < 0) {
            report_error("--eps %s is negative", argv[i]);
            hint("%s", adv);
            return -1;
        }
        return 1;
    }
    if (!std::strcmp(a, "--relaxation")) {
        if (!arg_dbl(argc, argv, i, &o.relaxation, "1 is the default and the measured optimum"))
            return -1;
        if (o.relaxation > 1.2)
            report_warning("--relaxation %s: above about 1.2 the iteration stops converging and "
                           "the picture breaks up", argv[i]);
        return 1;
    }
    if (!std::strcmp(a, "--chroma-upsampling")) {
        const char *v = take_value(argc, argv, i);
        if (v && !std::strcmp(v, "edge")) o.chroma_bilinear = 0;
        else if (v && !std::strcmp(v, "bilinear")) o.chroma_bilinear = 1;
        else {
            if (v) report_error("unknown --chroma-upsampling '%s'", v);
            hint("use edge (follows the brightness edges; the default) or bilinear "
                 "(the reference interpolation)");
            return -1;
        }
        return 1;
    }
    if (!std::strcmp(a, "--no-smooth-dc")) { o.smooth_dc = 0; return 1; }
    if (!std::strcmp(a, "--base-only")) { o.base_only = 1; return 1; }
    if (!std::strcmp(a, "--dither")) { o.dither = 1; return 1; }
    return 0;
}

/* Where fif decode writes when no output is named: the original picture's
 * name as the file records it (ORGN source_name), next to the FIF2 file.  The
 * stored name comes from the file, so it is reduced to a plain file name -
 * "../" or a drive letter in it must not decide where fif writes.  An
 * extension this fif cannot write becomes .png; a file that names no source
 * gives <in>.png. */
std::string default_decode_name(const char *fif_path, const uint8_t *d, size_t n) {
    std::string name;
    fif_info in{};
    uint8_t *flat = nullptr;
    size_t flat_n = 0;
    if (fif_get_info(d, n, &in) == FIF_OK && in.chunks_compressed &&
        fif_unpack(d, n, &flat, &flat_n) == FIF_OK)
        fif_get_info(flat, flat_n, &in);
    if (in.source_name && in.source_name_len) name.assign(in.source_name, in.source_name_len);
    fif_free(flat);
    name = base_name(name.c_str());
    for (char &ch : name)
        if ((unsigned char)ch < 0x20 || std::strchr("<>:\"|?*", ch)) ch = '_';
    if (name.empty() || name == "." || name == "..") name = stem(fif_path) + ".png";
    const Kind k = kind_from_ext(name.c_str());
    if (!((k == Kind::Png && kHavePng) || (k == Kind::Jpeg && kHaveJpeg) || k == Kind::Pnm))
        name = stem(name.c_str()) + (kHavePng ? ".png" : ".ppm");
    return dir_prefix(fif_path) + name;
}

/* ---------------------------------------------------------------- debug
 *
 * Streams one decode to stdout for the visualiser: a single JSON line, then
 * the frames back to back as raw RGB (or grey) - every iteration, then the
 * finished picture, then with --ref the reference.  Nothing is written to
 * disk; the viewer asks for a configuration, gets exactly it, and caches in
 * its own memory.
 *
 * Raw rather than PNG on purpose: the consumer is a canvas, which wants bytes
 * it can hand to ImageData anyway, and it saves an encoder on this side and a
 * decoder on that one. */

/* Just enough JSON for the debug stream.  The writer places the commas, so a
 * field can be added anywhere without minding its neighbours. */
class Json {
public:
    Json &open(char c)  { sep(); s_ += c; fresh_ = true; return *this; }
    Json &close(char c) { s_ += c; fresh_ = false; return *this; }
    Json &key(const char *k) { sep(); quote(k, std::strlen(k)); s_ += ':'; fresh_ = true; return *this; }
    Json &num(long long v) { char b[32]; std::snprintf(b, sizeof b, "%lld", v); return raw(b); }
    Json &num(double v, int digits) {
        char b[64];
        std::snprintf(b, sizeof b, "%.*f", digits, std::isfinite(v) ? v : 0.0);
        return raw(b);
    }
    Json &boolean(bool v) { return raw(v ? "true" : "false"); }
    Json &str(const char *p, size_t n) { sep(); quote(p, n); fresh_ = false; return *this; }
    Json &str(const std::string &v) { return str(v.data(), v.size()); }

    Json &kv(const char *k, long long v) { return key(k).num(v); }
    Json &kv(const char *k, double v, int digits) { return key(k).num(v, digits); }
    Json &kv(const char *k, const std::string &v) { return key(k).str(v); }
    Json &kb(const char *k, bool v) { return key(k).boolean(v); }
    Json &pair(const char *k, long long a, long long b) { return key(k).open('[').num(a).num(b).close(']'); }

    const std::string &text() const { return s_; }

private:
    Json &raw(const char *v) { sep(); s_ += v; fresh_ = false; return *this; }
    void sep() { if (!fresh_) s_ += ','; fresh_ = false; }
    void quote(const char *p, size_t n) {
        s_ += '"';
        for (size_t i = 0; i < n; ++i) {
            const unsigned char c = (unsigned char)p[i];
            if (c == '"' || c == '\\') { s_ += '\\'; s_ += (char)c; }
            else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); s_ += b; }
            else s_ += (char)c;
        }
        s_ += '"';
    }
    std::string s_;
    bool fresh_ = true;
};

/* What the header says, for the viewer's file panel and byte map. */
void json_file(Json &j, const uint8_t *d, size_t n) {
    fif_info in{};
    if (fif_get_info(d, n, &in) != FIF_OK) return;
    /* Packed chunks are read from an unpacked copy, the source name included. */
    uint8_t *flat = nullptr;
    size_t flat_n = 0;
    if (in.chunks_compressed && fif_unpack(d, n, &flat, &flat_n) != FIF_OK) flat = nullptr;
    fif_info names{};
    if (flat) fif_get_info(flat, flat_n, &names);
    const fif_info &src = flat ? names : in;
    char ver[16];
    std::snprintf(ver, sizeof ver, "%d.%d", in.version_major, in.version_minor);
    j.key("file").open('{');
    j.kv("version", ver).kv("width", in.width).kv("height", in.height)
     .kv("channels", in.channels).kv("min_block", in.min_block).kv("max_block", in.max_block)
     .kv("tile_size", in.tile_size).kv("tiles_x", in.tiles_x).kv("tiles_y", in.tiles_y)
     .kv("quality", in.quality).kv("effort", in.effort)
     .kv("subsampling", subsampling_name(in.subsampling))
     .kv("chroma", in.channels == 1 ? "" : chroma_name(in.chroma_mode))
     .kv("compressed", in.compressed).kv("encrypted", in.encrypted)
     .kv("compress_method", in.compress_method).kv("encrypt_method", in.encrypt_method)
     .kv("file_size", in.file_size)
     .kb("truncated", in.truncated != 0)
     .kv("base_prefix_bytes", in.base_prefix_bytes).kv("detail_offset", in.detail_offset)
     .kv("thumbnails", in.thumbnail_count).kv("chunk_count", in.chunk_count)
     .kv("raw_count", in.raw_count)
     .kv("source_name", std::string(src.source_name ? src.source_name : "", src.source_name_len))
     .kv("source_width", in.source_width).kv("source_height", in.source_height)
     .kv("source_scale", in.source_scale, 4);
    /* Sections follow the 144-byte header back to back, in this order. */
    long long off = (long long)fif::kHeaderSize;
    j.key("sections").open('{').pair("header", 0, (long long)fif::kHeaderSize);
    const std::pair<const char *, int64_t> secs[] = {
        {"chunks", in.chunk_bytes}, {"thumbnails", in.thumbnail_bytes},
        {"fic", in.fic_bytes}, {"raw", in.raw_bytes}};
    for (const auto &s : secs) { j.pair(s.first, off, s.second); off += s.second; }
    j.close('}');
    j.key("crc").open('{').kv("chunks", in.chunk_crc_ok).kv("thumbnails", in.thumbnail_crc_ok)
     .kv("fic", in.fic_crc_ok).kv("raw", in.raw_crc_ok).close('}');
    j.close('}');

    j.key("chunks").open('[');
    fif_chunk c{};
    for (int i = 0; fif_get_chunk(flat ? flat : d, flat ? flat_n : n, i, &c) == FIF_OK; ++i)
        j.open('{').kv("type", c.type).kv("name", std::string(c.name, c.name_len))
         .kv("value", chunk_value_text(c)).kb("crc_ok", c.crc_ok != 0).close('}');
    j.close(']');
    fif_free(flat);
}

/* The original picture, brought onto the frame grid of the debug stream so
 * that every frame can be measured against it: turned upright as the encoder
 * turned it, reduced to the coded size as the encoder reduced it, then
 * sampled where the frames sample the decoded image. */
bool reference_frame(const char *path, int coded_w, int coded_h, int image_w, int image_h,
                     const fif::DebugSink &sink, std::vector<uint8_t> &out) {
    if (!check_readable(path)) return false;
    fif_image ref{};
    const fif_status st = fif_image_load(path, &ref);
    if (st != FIF_OK) { explain_image_load(path, st); return false; }
    size_t fn = 0;
    if (uint8_t *bytes = read_file(path, &fn)) {
        fif::Exif ex;
        if (fif::read_exif(bytes, fn, ex)) apply_orientation(&ref, ex.orientation);
        std::free(bytes);
    }
    if ((ref.width != coded_w || ref.height != coded_h) &&
        ref.width >= coded_w && ref.height >= coded_h) {
        fif_image small{};
        if (fif_image_downscale(&ref, coded_w, coded_h, &small) == FIF_OK) {
            fif_image_free(&ref);
            ref = small;
        }
    }
    if (ref.width != coded_w || ref.height != coded_h) {
        report_error("the reference '%s' is %d x %d; the file was coded at %d x %d", path,
                     ref.width, ref.height, coded_w, coded_h);
        hint("give the original the file was encoded from");
        fif_image_free(&ref);
        return false;
    }
    const int fw = sink.width(), fh = sink.height(), ch = sink.channels(), step = sink.step();
    out.assign((size_t)fw * fh * ch, 0);
    for (int y = 0; y < fh; ++y) {
        const int ry = std::min(coded_h - 1, (int)((int64_t)y * step * coded_h / image_h));
        const uint8_t *row = ref.pixels + (size_t)ry * ref.stride;
        for (int x = 0; x < fw; ++x) {
            const int rx = std::min(coded_w - 1, (int)((int64_t)x * step * coded_w / image_w));
            const uint8_t *px = row + (size_t)rx * ref.channels;
            uint8_t *o = &out[((size_t)y * fw + x) * ch];
            if (ch == ref.channels) std::memcpy(o, px, (size_t)ch);
            else if (ch == 1) o[0] = (uint8_t)std::lround(fif::luma601(px[0], px[1], px[2]));
            else o[0] = o[1] = o[2] = px[0];
        }
    }
    fif_image_free(&ref);
    return true;
}

int cmd_debug(const Command &cmd, int argc, char **argv) {
    if (!need_files(cmd, argc, argv)) return kExitUsage;

    fif_decode_options o;
    fif_decode_options_defaults(&o);
    long long prefix = 0;          /* 0 = whole file */
    bool want_leaves = true;
    std::vector<int> only_list;
    bool only_tiles = false;
    int preview_max = 0;           /* 0 = full resolution */
    int iteration = -1;            /* -1 = stream every frame */
    const char *iter_out = nullptr;
    const char *ref_path = nullptr;

    for (int i = 3; i < argc; ++i) {
        const char *a = argv[i];
        if (const int r = decode_option(argc, argv, i, o)) {
            if (r < 0) return kExitUsage;
            if (!std::strcmp(a, "--scale") && (o.scale_den > 8 || (o.scale_den & (o.scale_den - 1)))) {
                report_error("fif debug reduces by 1/2, 1/4 or 1/8 only, got --scale %s", argv[i]);
                hint("the visualiser needs the attractor itself, not a filtered copy");
                return kExitUsage;
            }
        }
        else if (!std::strcmp(a, "--prefix")) {
            if (!arg_i64(argc, argv, i, &prefix, 0, "the number of bytes to decode; 0 = all"))
                return kExitUsage;
        }
        else if (!std::strcmp(a, "--no-leaves")) { want_leaves = false; }
        else if (!std::strcmp(a, "--preview-max")) {
            if (!arg_int(argc, argv, i, &preview_max, 0, INT_MAX, "the long edge in pixels; 0 = full size"))
                return kExitUsage;
        }
        else if (!std::strcmp(a, "--only-tiles")) {
            const char *v = take_value(argc, argv, i);
            if (!v) { hint("tile numbers and ranges, e.g. --only-tiles 0,3,5-8"); return kExitUsage; }
            /* "none" decodes nothing: every tile stays grey, as if no byte
             * of the image data had arrived. */
            if (std::strcmp(v, "none") && !parse_tile_list(a, v, only_list)) return kExitUsage;
            only_tiles = true;
        }
        else if (!std::strcmp(a, "--iteration")) {
            if (!arg_int(argc, argv, i, &iteration, 0, INT_MAX, "0 is the DC field the decode starts from"))
                return kExitUsage;
        }
        else if (!std::strcmp(a, "-o") || !std::strcmp(a, "--out")) {
            if (!(iter_out = take_value(argc, argv, i))) return kExitUsage;
        }
        else if (!std::strcmp(a, "--ref")) {
            if (!(ref_path = take_value(argc, argv, i))) {
                hint("the original image the file was encoded from");
                return kExitUsage;
            }
        }
        else { unknown_option(cmd, a); return kExitUsage; }
    }
    if (iter_out && !check_image_output(iter_out, argv[2])) return kExitUsage;
    /* Where --iteration writes, settled - and the overwrite question asked -
     * before the decode rather than after it. */
    std::string iter_path;
    if (iteration >= 0) {
        if (iter_out) {
            iter_path = iter_out;
        } else {
            char suffix[32];
            std::snprintf(suffix, sizeof suffix, "-iter%02d.png", iteration);
            iter_path = dir_prefix(argv[2]) + stem(argv[2]) + suffix;
        }
        if (!may_write(iter_path.c_str())) return kExitFail;
    }

    size_t n = 0;
    uint8_t *data = read_fif(argv[2], &n);
    if (!data) return kExitFail;
    const size_t full_size = n;
    /* Truncation is the quality axis of the format, so a prefix is simply a
     * shorter buffer - not a special decode mode. */
    if (prefix > 0 && (size_t)prefix < n) n = (size_t)prefix;

    fif_image img{};
    fif_stats st{};
    fif::DebugSink sink;
    sink.want_leaves = want_leaves;
    sink.preview_max = preview_max;
    if (only_tiles) {
        /* The mask is sized by the largest id seen. */
        for (int v : only_list) {
            if ((size_t)v >= sink.tile_mask.size()) sink.tile_mask.resize((size_t)v + 1, 0);
            sink.tile_mask[(size_t)v] = 1;
        }
        if (sink.tile_mask.empty()) sink.tile_mask.push_back(0);
    }
    const fif_status rc = fif::decode_with_debug(data, n, &o, &img, &st, &sink);
    if (rc != FIF_OK) {
        report_error("cannot decode '%s': %s", argv[2], fif_status_string(rc));
        std::free(data);
        return kExitFail;
    }

    /* --iteration picks one frame out of the ladder and writes it as an image
     * instead of streaming the lot.  Iteration 0 is the DC field the
     * decode starts from, so this is also how you look at the seed. */
    if (iteration >= 0) {
        std::free(data);
        const int last = sink.frames();          /* == the real decode output */
        if (iteration > last) {
            report_error("--iteration %d is past the last iteration of this decode (%d)",
                         iteration, last);
            hint("use 0..%d; %d is the finished, deblocked picture", last, last);
            fif_image_free(&img);
            return kExitFail;
        }
        const std::string &path = iter_path;
        /* The last index is the deblocked output the decoder really produces;
         * everything before it is a frame the sink captured mid-iteration. */
        fif_image one = img;
        if (iteration < last) {
            const std::vector<uint8_t> &f = sink.frame(iteration);
            one.width = sink.width();
            one.height = sink.height();
            one.channels = sink.channels();
            one.stride = (size_t)one.width * one.channels;
            one.pixels = const_cast<uint8_t *>(f.data());
        }
        const fif_status wr = fif_image_save(path.c_str(), &one);
        fif_image_free(&img);
        if (wr != FIF_OK) {
            if (wr == FIF_ERR_IO) explain_write_error(path.c_str(), errno);
            else report_error("cannot write '%s': %s", path.c_str(), fif_status_string(wr));
            return kExitFail;
        }
        std::printf("%s: iteration %d of %d, %dx%d\n", path.c_str(), iteration,
                    last, one.width, one.height);
        return 0;
    }

    /* The iteration frames stop at the attractor.  The decoder then runs the
     * whole-image post-processing over the assembled planes, so the last
     * iteration is not what a plain decode writes out - the real output is
     * appended as one more frame, decimated like the others. */
    const int fw = sink.width(), fh = sink.height(), ch = sink.channels(), step = sink.step();
    std::vector<std::vector<uint8_t>> extra(1, std::vector<uint8_t>((size_t)fw * fh * ch, 128));
    for (int dy = 0; dy < fh && dy * step < img.height; ++dy) {
        const uint8_t *src = img.pixels + (size_t)dy * step * img.stride;
        for (int dx = 0; dx < fw && dx * step < img.width; ++dx)
            std::memcpy(&extra[0][((size_t)dy * fw + dx) * ch], src + (size_t)dx * step * ch, (size_t)ch);
    }
    fif_info in{};
    fif_get_info(data, full_size, &in);
    if (ref_path) {
        extra.emplace_back();
        if (!reference_frame(ref_path, in.width, in.height, img.width, img.height, sink, extra[1])) {
            std::free(data);
            fif_image_free(&img);
            return kExitFail;
        }
    }
    const int nf = sink.frames();
    const auto frame_at = [&](int i) -> const std::vector<uint8_t> & {
        return i < nf ? sink.frame(i) : extra[(size_t)(i - nf)];
    };
    const auto as_image = [&](const std::vector<uint8_t> &f) {
        fif_image v{};
        v.pixels = const_cast<uint8_t *>(f.data());
        v.width = fw; v.height = fh; v.channels = ch; v.stride = (size_t)fw * ch;
        return v;
    };

    Json j;
    j.open('{');
    /* The layout of this stream, raised whenever a field the visualiser
     * relies on changes, so that it can refuse a binary that does not match
     * instead of showing half a picture. */
    j.kv("debug_format", 3);
    /* width/height are the frame buffers; image_* is the decode the leaf and
     * tile coordinates are expressed in.  They differ when --preview-max bit. */
    j.kv("width", fw).kv("height", fh).kv("image_width", img.width).kv("image_height", img.height)
     .kv("preview_step", step).kv("channels", ch)
     .kv("frames", nf + (long long)extra.size()).kv("final_index", nf)
     .kv("ref_index", ref_path ? nf + 1 : -1)
     .kv("scale", o.scale_num).kv("scale_den", o.scale_den)
     .kv("file_size", (long long)full_size).kv("decoded_bytes", (long long)n)
     .kv("iterations_used", st.iterations_used).kv("source", argv[2]);
    j.key("options").open('{')
     .kv("max_iterations", o.max_iterations).kv("eps", o.convergence, 3)
     .kv("relaxation", o.relaxation, 3).kv("deblock", o.deblock)
     .kb("base_only", o.base_only != 0).kb("smooth_dc", o.smooth_dc != 0)
     .kb("chroma_bilinear", o.chroma_bilinear != 0).kb("dither", o.dither != 0)
     .close('}');
    json_file(j, data, full_size);
    std::free(data);
    if (ref_path) {
        /* Against the reference: per frame, so the curve shows how quality
         * builds up over the iterations.  Exact only when the frames are the
         * decode itself; otherwise both sides are point-sampled alike. */
        const fif_image r = as_image(extra[1]);
        j.key("quality").open('{').kb("exact", step == 1 && o.scale_num == 1 && o.scale_den == 1);
        j.key("psnr").open('[');
        for (int i = 0; i <= nf; ++i) { const fif_image f = as_image(frame_at(i)); j.num(fif_psnr(&f, &r), 3); }
        j.close(']').key("ssim").open('[');
        for (int i = 0; i <= nf; ++i) { const fif_image f = as_image(frame_at(i)); j.num(fif_ssim(&f, &r), 5); }
        j.close(']').close('}');
    }
    j.key("tiles").open('[');
    for (const fif::DebugTile &t : sink.tiles) {
        long long luma = 0;
        for (const fif::DebugLeaf &d : t.leaves) luma += d.plane == 0;
        j.open('{').kv("id", t.id).kv("x", t.x).kv("y", t.y).kv("w", t.w).kv("h", t.h)
         .kv("iters", t.iters).kv("ms", t.ms, 3).kv("leaves", luma)
         .pair("base", t.base_off, t.base_len).pair("detail", t.det_off, t.det_len);
        j.key("dmax").open('[');
        for (float v : t.dmax) j.num(v, 4);
        j.close(']').close('}');
    }
    j.close(']');
    /* Leaves as one flat array of numbers, not objects: a 4K tile set runs to
     * six figures of them and the field names would be most of the bytes. */
    j.key("leaf_fields").open('[');
    for (const char *f : {"tile", "plane", "x", "y", "size", "domain", "sym", "flags",
                          "dc", "s", "gx", "gy", "dx", "dy", "dext"})
        j.str(f, std::strlen(f));
    j.close(']').key("leaves").open('[');
    for (const fif::DebugTile &t : sink.tiles)
        for (const fif::DebugLeaf &d : t.leaves)
            j.num(d.tile).num(d.plane).num(d.x).num(d.y).num(d.size).num(d.domain)
             .num(d.sym).num(d.flags).num(d.dc, 2).num(d.s, 4).num(d.gx, 2).num(d.gy, 2)
             .num(d.dx).num(d.dy).num(d.dext);
    j.close(']').close('}');
    fif_image_free(&img);

#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);   /* or every 0x0A becomes 0x0D0A */
#endif
    std::fwrite(j.text().data(), 1, j.text().size(), stdout);
    std::fputc('\n', stdout);
    for (int i = 0; i < nf + (int)extra.size(); ++i) {
        const std::vector<uint8_t> &f = frame_at(i);
        if (std::fwrite(f.data(), 1, f.size(), stdout) != f.size()) {
            report_error("writing frame %d to stdout failed: %s", i, std::strerror(errno));
            return kExitFail;
        }
    }
    std::fflush(stdout);
    return 0;
}

/* How many pixels a decode of this file with these options will allocate:
 * the planes padded to whole blocks, scaled the way the options ask.  The
 * decoder counts them the same way, so a warning about the size and the
 * decoder's own limit talk about the same number. */
double decoded_pixels(const fif_info &in, const fif_decode_options &o) {
    if (in.width <= 0 || in.height <= 0 || in.max_block <= 0) return 0;
    double num = o.scale_num, den = o.scale_den;
    if (o.out_width > 0 || o.out_height > 0) {
        const double fw = o.out_width > 0 ? (double)o.out_width / in.width : 0;
        const double fh = o.out_height > 0 ? (double)o.out_height / in.height : 0;
        num = std::max(1.0, std::ceil(std::max(fw, fh) - 1e-9)); den = 1;
    }
    if (num <= 0 || den <= 0) return 0;
    const int align = in.max_block << (in.subsampling == FIF_SUBSAMPLING_420 ? 1 : 0);
    const auto padded = [&](int v) {
        return std::ceil((double)((v + align - 1) / align * align) * num / den);
    };
    return padded(in.width) * padded(in.height);
}

int cmd_decode(const Command &cmd, int argc, char **argv) {
    if (!need_files(cmd, argc, argv)) return kExitUsage;
    fif_decode_options o;
    fif_decode_options_defaults(&o);
    std::vector<int> tiles;
    /* The output is optional: the word after the input, unless it is an
     * option. */
    const bool named = argc > 3 && argv[3][0] != '-';
    for (int i = named ? 4 : 3; i < argc; ++i) {
        const char *a = argv[i];
        if (const int r = decode_option(argc, argv, i, o)) { if (r < 0) return kExitUsage; }
        else if (!std::strcmp(a, "--width") || !std::strcmp(a, "--height")) {
            int *dst = (a[2] == 'w') ? &o.out_width : &o.out_height;
            if (!arg_int(argc, argv, i, dst, 1, 1 << 24,
                         "the output size in pixels; the other side follows the picture's shape"))
                return kExitUsage;
        }
        else if (!std::strcmp(a, "--size")) {
            const char *v = take_value(argc, argv, i);
            char tail = 0;
            if (!v || std::sscanf(v, "%dx%d%c", &o.out_width, &o.out_height, &tail) != 2 ||
                o.out_width < 1 || o.out_height < 1) {
                if (v) report_error("--size wants WIDTHxHEIGHT, got '%s'", v);
                hint("e.g. --size 1920x1080 (fills the box, trims the overhang); "
                     "give only --width or --height to keep the whole picture");
                return kExitUsage;
            }
        }
        else if (!std::strcmp(a, "--region")) {
            const char *v = take_value(argc, argv, i);
            if (!v) { hint("e.g. --region 640x480+100+50"); return kExitUsage; }
            if (!parse_region(v, &o.region_x, &o.region_y, &o.region_w, &o.region_h))
                return kExitUsage;
        }
        else if (!std::strcmp(a, "--tiles")) {
            const char *v = take_value(argc, argv, i);
            if (!v) { hint("e.g. --tiles 0,3,5-8"); return kExitUsage; }
            if (!parse_tile_list(a, v, tiles)) return kExitUsage;
        }
        else if (!std::strcmp(a, "--max-pixels")) {
            long long mp = 0;
            if (!arg_i64(argc, argv, i, &mp, 1, "the size from which a decode warns about its memory, in pixels; default " kMaxPixelsText))
                return kExitUsage;
            o.max_pixels = (int64_t)mp;
        }
        else if (!std::strcmp(a, "--progress")) { g.progress = 1; }
        else if (!std::strcmp(a, "--no-progress")) { g.progress = 0; }
        else { unknown_option(cmd, a); return kExitUsage; }
    }
    if ((o.out_width > 0 || o.out_height > 0) && (o.scale_num != 1 || o.scale_den != 1))
        report_warning("--scale is ignored: --width, --height and --size set the output size");
    if (named && !check_image_output(argv[3], argv[2])) return kExitUsage;

    size_t n = 0;
    uint8_t *data = read_fif(argv[2], &n);
    if (!data) return kExitFail;
    const std::string out_name = named ? argv[3] : default_decode_name(argv[2], data, n);
    const char *out = out_name.c_str();
    if ((!named && !check_image_output(out, argv[2])) || !may_write(out)) {
        std::free(data);
        return kExitFail;
    }

    /* Region and tiles are checked against the file here, where the message
     * can say what the file actually has. */
    {
        fif_info fi{};
        fif_get_info(data, n, &fi);
        if (o.region_w > 0 && (o.region_x + (int64_t)o.region_w > fi.width ||
                               o.region_y + (int64_t)o.region_h > fi.height)) {
            report_error("--region %dx%d+%d+%d reaches past the picture, which is %d x %d",
                         o.region_w, o.region_h, o.region_x, o.region_y, fi.width, fi.height);
            if (o.region_x < fi.width && o.region_y < fi.height)
                hint("the largest region starting there is %dx%d+%d+%d",
                     std::min(o.region_w, fi.width - o.region_x),
                     std::min(o.region_h, fi.height - o.region_y), o.region_x, o.region_y);
            std::free(data);
            return kExitUsage;
        }
        for (int t : tiles)
            if (t >= fi.tile_count) {
                report_error("--tiles: there is no tile %d; '%s' has %lld tiles (0..%lld)", t,
                             argv[2], (long long)fi.tile_count, (long long)fi.tile_count - 1);
                hint("'fif info \"%s\" --tiles' lists them with their positions", argv[2]);
                std::free(data);
                return kExitUsage;
            }
        if (!tiles.empty()) { o.tile_list = tiles.data(); o.tile_list_len = (int)tiles.size(); }
        /* The pixel limit warns, it does not refuse.  A decode that large is
         * usually what the user asked for - their own big photograph, or a
         * --scale on top of it - and the one thing they cannot see coming is
         * what it costs in memory.  So: say it, raise the limit for this run,
         * and decode.  (The library keeps refusing above its limit; this is
         * the tool deciding to trust the person at the keyboard.) */
        const int64_t limit = o.max_pixels > 0 ? o.max_pixels
                                               : (int64_t)FIF_DEFAULT_MAX_PIXELS;
        const double px = decoded_pixels(fi, o);
        if (px > (double)limit) {
            report_warning("the picture decodes to %s pixels, above the limit of %s: "
                           "increased RAM usage", count_text(px).c_str(),
                           count_text((double)limit).c_str());
            hint("decode it smaller if the machine is short of memory, e.g. --scale 1/2");
            o.max_pixels = (int64_t)1 << 60;
        }
    }

    fif_image img{};
    fif_stats stats{};
    Bar bar{"decode"};
    if (want_progress()) { o.progress = Bar::tick; o.progress_user = &bar; }
    announce_threads(o.threads);
    const auto t0 = std::chrono::steady_clock::now();
    fif_status st = fif_decode(data, n, &o, &img, &stats);
    const double secs = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    bar.done();
    if (st != FIF_OK) {
        fif_info in{};
        const bool have_info = fif_get_info(data, n, &in) == FIF_OK;
        const bool sized = o.out_width > 0 || o.out_height > 0;
        if (st == FIF_ERR_UNSUPPORTED && have_info && sized) {
            std::string want = o.out_width > 0 ? "a width of " + std::to_string(o.out_width) : "";
            if (o.out_height > 0)
                want += (want.empty() ? "a height of " : " and a height of ") + std::to_string(o.out_height);
            report_error("%s is more than 16 times the encoded size (%d x %d); the decoder does "
                         "not enlarge further", want.c_str(), in.width, in.height);
            hint("ask for at most %d x %d, and enlarge the result in an image editor if you must",
                 in.width * 16, in.height * 16);
        } else if (st == FIF_ERR_TOO_LARGE && have_info) {
            /* The limit is there to stop a crafted header from asking for a
             * terabyte, so it stays where it is - but someone who just pointed
             * the tool at their own 900 megapixel image deserves the number
             * and the flag rather than a shrug.  The decoder counts the plane
             * padded to whole blocks and scaled, so the suggestion does too. */
            double num = o.scale_num, den = o.scale_den;
            if (sized) {
                const double fw = o.out_width > 0 ? (double)o.out_width / in.width : 0;
                const double fh = o.out_height > 0 ? (double)o.out_height / in.height : 0;
                num = std::max(1.0, std::ceil(std::max(fw, fh) - 1e-9)); den = 1;
            }
            const int align = in.max_block << (in.subsampling == FIF_SUBSAMPLING_420 ? 1 : 0);
            const auto padded = [&](int v) {
                return std::ceil((double)((v + align - 1) / align * align) * num / den);
            };
            const double px = padded(in.width) * padded(in.height);
            const double limit = (double)(o.max_pixels > 0 ? o.max_pixels
                                                           : (int64_t)FIF_DEFAULT_MAX_PIXELS);
            report_error("the decoded picture would have %s pixels; the limit is %s",
                         count_text(px).c_str(), count_text(limit).c_str());
            hint("if you trust the file, raise the limit: --max-pixels %lld", (long long)std::ceil(px));
            hint("or decode it smaller, e.g. --scale 1/2");
        } else {
            explain_fif_error(argv[2], data, n, st);
        }
        std::free(data);
        return kExitFail;
    }
    warn_fif_state(argv[2], data, n);
    std::free(data);
    st = fif_image_save(out, &img);
    if (st == FIF_ERR_IO) explain_write_error(out, errno);
    else if (st != FIF_OK) report_error("cannot write '%s': %s", out, fif_status_string(st));
    else
        result("%s: %dx%d in %.3f s (%d iterations, %.2f Mpixel/s)\n",
                     out, img.width, img.height, secs, stats.iterations_used,
                     rate((double)img.width * img.height / 1e6, secs));
    fif_image_free(&img);
    return st == FIF_OK ? 0 : kExitFail;
}

int cmd_info(const Command &cmd, int argc, char **argv) {
    if (!need_files(cmd, argc, argv)) return kExitUsage;
    bool list_tiles = false;
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--tiles")) list_tiles = true;
        else { unknown_option(cmd, argv[i]); return kExitUsage; }
    }
    size_t n = 0;
    uint8_t *data = read_fif(argv[2], &n);
    if (!data) return kExitFail;
    fif_info in{};
    fif_get_info(data, n, &in);
    /* Compressed sections are listed from an unpacked copy: the accessors
     * hand out pointers into the buffer they are given. */
    uint8_t *flat = nullptr;
    size_t flat_n = 0;
    fif_info fin = in;
    const int packed = in.compressed & (FIF_SECTION_CHUNKS | FIF_SECTION_THUMBNAILS |
                                        FIF_SECTION_FIC | FIF_SECTION_RAW);
    if (packed) {
        const fif_status us = fif_unpack(data, n, &flat, &flat_n);
        if (us == FIF_OK) fif_get_info(flat, flat_n, &fin);
        else if (us == FIF_ERR_UNSUPPORTED)
            report_warning("some parts are zlib-compressed and this fif was built without zlib; "
                           "they are not shown");
        else
            report_warning("a compressed part of the file is damaged; the compressed parts are not shown");
    }
    const uint8_t *meta = flat ? flat : data;
    const size_t meta_n = flat ? flat_n : n;
    auto crc = [](int ok) { return ok > 0 ? "ok" : ok == 0 ? "MISMATCH" : "not checked"; };
    std::printf("file        %s\n", argv[2]);
    std::printf("format      FIF2 v%d.%d\n", in.version_major, in.version_minor);
    std::printf("dimensions  %d x %d, %d channel(s)\n", in.width, in.height, in.channels);
    std::printf("size        %s  (%.4f bpp)%s\n", human((double)n).c_str(),
                8.0 * n / ((double)in.width * in.height),
                in.truncated ? "  - truncated, the header declares more" : "");
    std::printf("source      %.*s%s%d x %d",
                (int)fin.source_name_len, fin.source_name ? fin.source_name : "",
                fin.source_name_len ? ", " : "", in.source_width, in.source_height);
    if (in.source_scale != 1.0) std::printf(", reduced by %.4g before encoding", in.source_scale);
    std::printf("\n");
    std::printf("blocks      %d..%d, tile %d\n", in.min_block, in.max_block, in.tile_size);
    if (in.subsampling == FIF_SUBSAMPLING_400)
        std::printf("colour      greyscale\n");
    else
        std::printf("chroma      %s, %s\n", subsampling_name(in.subsampling),
                    chroma_name(in.chroma_mode));
    /* Against the size the header declares, not the bytes at hand: on a file
     * that was cut off the two differ, and measuring the base layer against
     * the shorter one is what used to print "101.2%". */
    const double declared = in.file_size > 0 ? (double)in.file_size : (double)n;
    if (in.base_prefix_bytes > (int64_t)n)
        std::printf("base layer  first %s of %s (%.1f%%) decodes to the coarse picture"
                    " - not here yet, the file stops at %s\n",
                    human((double)in.base_prefix_bytes).c_str(),
                    human(declared).c_str(),
                    100.0 * (double)in.base_prefix_bytes / declared,
                    human((double)n).c_str());
    else if (in.base_prefix_bytes > 0)
        std::printf("base layer  first %s of %s (%.1f%%) decodes to the coarse picture\n",
                    human((double)in.base_prefix_bytes).c_str(),
                    human(declared).c_str(),
                    100.0 * (double)in.base_prefix_bytes / declared);
    else if (in.fic_bytes)
        std::printf("base layer  the first %s of the unpacked image data\n",
                    human((double)in.detail_offset).c_str());
    std::printf("tiles       %lld (%d x %d), independently decodable\n",
                (long long)in.tile_count, in.tiles_x, in.tiles_y);
    if (in.remarks[0]) std::printf("remarks     \"%s\"\n", in.remarks);
    /* Per section: the stored size, and how it is stored when it is not
     * plain - one bit per section in each of the two header fields. */
    /* The two method bytes of the header say how, for the whole file; a
     * number this build does not know is printed as the number, so a locked
     * or packed file at least names what it was made with. */
    const char *cm = in.compress_method == FIF_COMPRESS_DEFLATE ? "zlib" : nullptr;
    const char *em = in.encrypt_method == FIF_ENCRYPT_BLOWFISH ? "blowfish" : nullptr;
    char cmbuf[24], embuf[24];
    if (!cm) { std::snprintf(cmbuf, sizeof cmbuf, "method %d", in.compress_method); cm = cmbuf; }
    if (!em) { std::snprintf(embuf, sizeof embuf, "method %d", in.encrypt_method); em = embuf; }
    auto section = [&](int64_t stored, int64_t unpacked, int bit) {
        std::string t = human((double)stored);
        if (in.encrypted & bit) t += std::string(" (encrypted, ") + em + ")";
        else if (in.compressed & bit)
            t += flat ? " (" + std::string(cm) + ", " + human((double)unpacked) + " unpacked)"
                      : " (" + std::string(cm) + ")";
        return t;
    };
    std::printf("sections    chunks %s, thumbnail %s, fic %s, raw %s\n",
                section(in.chunk_bytes, fin.chunk_bytes, FIF_SECTION_CHUNKS).c_str(),
                section(in.thumbnail_bytes, fin.thumbnail_bytes, FIF_SECTION_THUMBNAILS).c_str(),
                section(in.fic_bytes, fin.fic_bytes, FIF_SECTION_FIC).c_str(),
                section(in.raw_bytes, fin.raw_bytes, FIF_SECTION_RAW).c_str());
    std::printf("checksums   fic %s", crc(in.fic_crc_ok));
    if (in.chunk_bytes) std::printf(", chunks %s", crc(in.chunk_crc_ok));
    if (in.thumbnail_bytes) std::printf(", thumbnail %s", crc(in.thumbnail_crc_ok));
    if (in.raw_bytes) std::printf(", raw %s", crc(in.raw_crc_ok));
    std::printf("\n");

    if (in.thumbnail_count)
        std::printf("thumbnails  %d\n", in.thumbnail_count);
    fif_thumbnail th{};
    for (int i = 0; fif_get_thumbnail(meta, meta_n, i, &th) == FIF_OK; ++i)
        std::printf("  [%d] %d x %d %s, %s%s\n", i + 1, th.width, th.height, th.format,
                    human((double)th.size).c_str(), th.crc_ok ? "" : "   (checksum MISMATCH)");

    std::printf("chunks      %d\n", in.chunk_count);
    fif_chunk c{};
    for (int i = 0; fif_get_chunk(meta, meta_n, i, &c) == FIF_OK; ++i) {
        std::printf("  %s  %-14.*s ", c.type, (int)c.name_len, c.name);
        const uint64_t sz = c.size;
        if (c.value_type == FIF_VALUE_TEXT) {
            /* A text over several lines continues under its first line. */
            const size_t len = (size_t)std::min<uint64_t>(sz, 400);
            const char *t = (const char *)c.data;
            std::fputc('"', stdout);
            for (size_t k = 0; k < len; ++k) {
                if (t[k] == '\n') std::printf("\n%24s", "");
                else if (t[k] != '\r') std::fputc(t[k], stdout);
            }
            std::printf("\"%s", sz > len ? " ..." : "");
        } else {
            std::fputs(chunk_value_text(c).c_str(), stdout);
        }
        std::printf("%s\n", c.crc_ok ? "" : "   (checksum MISMATCH)");
    }

    if (in.raw_bytes) {
        const int rc = fif_raw_count(meta, meta_n);
        if (rc >= 0) std::printf("raw data    %d entr%s\n", rc, rc == 1 ? "y" : "ies");
        else std::printf("raw data    not readable\n");
        fif_raw_entry r{};
        for (int i = 0; i < rc && fif_get_raw(meta, meta_n, i, &r) == FIF_OK; ++i)
            std::printf("  [%d] %s  %.*s  codec %s, %s stored of %s%s  %.*s\n", i,
                        r.file_type, (int)r.file_name_len, r.file_name, r.file_codec,
                        human((double)r.data_size).c_str(), human((double)r.file_size).c_str(),
                        r.complete ? "" : " (truncated)",
                        (int)std::min<size_t>(r.metadata_len, 200), r.metadata);
    }

    /* The tile map: what --region and --tiles of fif decode refer to, and the
     * byte ranges a client fetches to decode one tile on its own. */
    if (list_tiles) {
        std::printf("tile map    index: x,y wxh  base offset+size  detail offset+size\n");
        if (in.compressed & FIF_SECTION_FIC)
            std::printf("            the image data is compressed: offsets are into the unpacked copy\n");
        fif_tile t{};
        for (int i = 0; fif_get_tile(meta, meta_n, i, &t) == FIF_OK; ++i) {
            auto range = [](int64_t off, int64_t sz) {
                return off < 0 ? std::string("missing") : std::to_string(off) + "+" + std::to_string(sz);
            };
            std::printf("  [%d] %d,%d %dx%d  base %s  detail %s\n", i, t.x, t.y, t.w, t.h,
                        range(t.base_offset, t.base_size).c_str(),
                        range(t.detail_offset, t.detail_size).c_str());
        }
    }
    fif_free(flat);
    std::free(data);
    return 0;
}

/* Writes the embedded preview.  To .jpg it copies the stored bytes as they
 * are - no second lossy pass; to any other format it decodes and re-saves. */
int cmd_thumbnail(const Command &cmd, int argc, char **argv) {
    if (!need_files(cmd, argc, argv)) return kExitUsage;
    /* The output is optional; which preview is an option, --id, so that a
     * number on its own is never mistaken for a file name or the other way
     * round. */
    auto is_number = [](const char *a) {
        for (const char *c = a; *c; ++c) if (!std::isdigit((unsigned char)*c)) return false;
        return *a != '\0';
    };
    const bool has_out = argc > 3 && argv[3][0] != '-' && !is_number(argv[3]);
    const char *named = has_out ? argv[3] : nullptr;
    int want = 0;                  /* --id, counted from 1 as fif info lists them; 0 = largest */
    for (int i = has_out ? 4 : 3; i < argc; ++i) {
        const char *a = argv[i];
        if (!std::strcmp(a, "--id")) {
            if (!arg_int(argc, argv, i, &want, 1, INT_MAX,
                         "the number of the preview; 'fif info' lists them, starting at 1"))
                return kExitUsage;
        } else if (is_number(a)) {
            report_error("fif thumbnail: to choose preview %s, write --id %s", a, a);
            hint("e.g. fif thumbnail \"%s\" --id %s", argv[2], a);
            return kExitUsage;
        } else {
            unknown_option(cmd, a);
            return kExitUsage;
        }
    }
    if (named && !check_image_output(named, argv[2])) return kExitUsage;
    size_t n = 0;
    uint8_t *data = read_fif(argv[2], &n);
    if (!data) return kExitFail;
    fif_info in{};
    fif_get_info(data, n, &in);
    if (in.encrypted & FIF_SECTION_THUMBNAILS) {
        report_error("the previews in '%s' are encrypted; a password is needed, and fif cannot decrypt",
                     argv[2]);
        std::free(data);
        return kExitFail;
    }
    /* A packed section is read from an unpacked copy: the accessors hand out
     * pointers into the buffer they are given. */
    if (in.compressed & FIF_SECTION_THUMBNAILS) {
        uint8_t *flat = nullptr;
        size_t flat_n = 0;
        const fif_status us = fif_unpack(data, n, &flat, &flat_n);
        if (us != FIF_OK) {
            report_error("the compressed previews in '%s' cannot be unpacked: %s", argv[2],
                         us == FIF_ERR_UNSUPPORTED ? "this fif was built without zlib"
                                                   : fif_status_string(us));
            std::free(data);
            return kExitFail;
        }
        std::free(data);
        data = (uint8_t *)std::malloc(flat_n);
        if (!data) { fif_free(flat); report_error("out of memory"); return kExitFail; }
        std::memcpy(data, flat, flat_n);
        n = flat_n;
        fif_free(flat);
    }
    if (in.thumbnail_count == 0) {
        report_error("'%s' has no embedded preview", argv[2]);
        hint("a quick small decode does the same job: fif decode \"%s\" preview.png --scale 1/8",
             argv[2]);
        hint("to embed one, encode with --thumbnail 256");
        std::free(data);
        return kExitFail;
    }
    /* Default: the largest, which is the one a viewer would scale down.
     * `idx` is the library's index, from 0; every number shown is idx + 1. */
    const int idx = want > 0 ? want - 1 : fif_find_thumbnail(data, n, INT_MAX, nullptr);
    if (idx >= in.thumbnail_count) {
        report_error("'%s' has %d preview%s; there is none with --id %d", argv[2], in.thumbnail_count,
                     in.thumbnail_count == 1 ? "" : "s", want);
        if (in.thumbnail_count == 1) hint("the only one has --id 1; leave out --id to get it");
        else hint("use --id 1..%d, or leave out --id to get the largest", in.thumbnail_count);
        std::free(data);
        return kExitFail;
    }
    fif_thumbnail th{};
    fif_status st = fif_get_thumbnail(data, n, idx, &th);
    if (st != FIF_OK) {
        report_error("preview %d of '%s' is damaged or cut off", idx + 1, argv[2]);
        hint("the picture itself may still decode: fif decode \"%s\" preview.png --scale 1/8", argv[2]);
        std::free(data);
        return kExitFail;
    }
    /* Without a name: <in>-thumb.jpg, or -thumb<N> for a chosen one, next to
     * the input; a preview that is not a JPEG is converted to PNG. */
    std::string out_name;
    if (named) {
        out_name = named;
    } else {
        out_name = dir_prefix(argv[2]) + stem(argv[2]) + "-thumb" +
                   (want > 0 ? std::to_string(want) : std::string()) +
                   (!std::strcmp(th.format, "JPEG") || !kHavePng ? ".jpg" : ".png");
        if (!check_image_output(out_name.c_str(), argv[2])) { std::free(data); return kExitFail; }
    }
    const char *out = out_name.c_str();
    if (!may_write(out)) { std::free(data); return kExitFail; }
    if (!th.crc_ok) report_warning("the checksum of preview %d does not match; it may show errors", idx + 1);
    int rc;
    if (!std::strcmp(th.format, "JPEG") && kind_from_ext(out) == Kind::Jpeg) {
        rc = write_file(out, th.data, (size_t)th.size);
    } else {
        fif_image img{};
        st = fif_decode_thumbnail(data, n, idx, &img);
        if (st == FIF_ERR_UNSUPPORTED) {
            report_error("cannot convert preview %d (%s) to '%s'", idx + 1, th.format, out);
            if (!std::strcmp(th.format, "JPEG"))
                hint_missing_library(Kind::Jpeg);
            hint("export it to a .jpg file instead: the stored preview is then copied as it is");
        } else if (st != FIF_OK) {
            report_error("preview %d of '%s' does not decode: %s", idx + 1, argv[2], fif_status_string(st));
        } else {
            st = fif_image_save(out, &img);
            if (st == FIF_ERR_IO) explain_write_error(out, errno);
            else if (st != FIF_OK) report_error("cannot write '%s': %s", out, fif_status_string(st));
        }
        fif_image_free(&img);
        rc = st == FIF_OK ? 0 : kExitFail;
    }
    if (!rc) result("%s: %dx%d preview exported\n", out, th.width, th.height);
    std::free(data);
    return rc;
}

int cmd_compare(const Command &cmd, int argc, char **argv) {
    if (!need_files(cmd, argc, argv)) return kExitUsage;
    if (argc > 4) { unknown_option(cmd, argv[4]); return kExitUsage; }
    fif_image a{}, b{};
    fif_status st = fif_image_load(argv[2], &a);
    if (st != FIF_OK) {
        explain_image_load(argv[2], st);
        return kExitFail;
    }
    st = fif_image_load(argv[3], &b);
    if (st != FIF_OK) {
        explain_image_load(argv[3], st);
        fif_image_free(&a);
        return kExitFail;
    }
    if (a.width != b.width || a.height != b.height) {
        report_error("the images differ in size (%d x %d and %d x %d); only images of the same "
                     "size can be compared", a.width, a.height, b.width, b.height);
        const fif_image &big = a.width * (int64_t)a.height > b.width * (int64_t)b.height ? a : b;
        hint("if one is a decode, decode it at the other's size: fif decode in.fif out.png --size %dx%d",
             big.width, big.height);
        hint("if the encode was reduced (--max-dim, --downscale), compare against a reduced original instead");
        fif_image_free(&a); fif_image_free(&b);
        return kExitFail;
    }
    /* One side grey and the other colour is what 4:0:0 produces, and comparing
     * its luma against the source's is the meaningful measurement. */
    std::vector<uint8_t> la, lb;
    const fif_image ca = (a.channels == b.channels) ? a : to_luma(a, la);
    const fif_image cb = (a.channels == b.channels) ? b : to_luma(b, lb);
    std::printf("psnr      %.2f dB\n", fif_psnr(&ca, &cb));
    std::printf("ssim      %.4f\n", fif_ssim(&ca, &cb));
    std::printf("ms-ssim   %.4f\n", fif_ms_ssim(&ca, &cb));
    fif_image_free(&a); fif_image_free(&b);
    return 0;
}

int cmd_bench(const Command &cmd, int argc, char **argv) {
    if (!need_files(cmd, argc, argv)) return kExitUsage;
    fif_encode_options o;
    fif_encode_options_defaults(&o);
    bool want_stats = false;
    if (parse_encode_opts(cmd, argc, argv, 3, o, &want_stats, true) != 0) return kExitUsage;

    fif_image src{};
    fif_status st = fif_image_load(argv[2], &src);
    if (st != FIF_OK) {
        explain_image_load(argv[2], st);
        return kExitFail;
    }
    if (apply_downscale(&src) != 0) { fif_image_free(&src); return kExitFail; }
    uint8_t *data = nullptr;
    size_t n = 0;
    fif_stats est{};
    announce_threads(o.threads);
    st = fif_encode(&src, &o, &data, &n, &est);
    if (st != FIF_OK) {
        report_error("encoding '%s' failed: %s", argv[2], fif_status_string(st));
        if (st == FIF_ERR_NOMEM)
            hint("not enough memory: reduce the image (--max-dim) or use fewer threads (-t 1)");
        fif_image_free(&src);
        return kExitFail;
    }
    fif_decode_options d;
    fif_decode_options_defaults(&d);
    d.threads = o.threads;
    d.deblock = g.bench_deblock;
    if (g.bench_relax > 0) d.relaxation = g.bench_relax;
    fif_image dec{};
    fif_stats dst{};
    const auto t0 = std::chrono::steady_clock::now();
    st = fif_decode(data, n, &d, &dec, &dst);
    const double dsec = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    if (st != FIF_OK) {
        report_error("decoding the fresh encode failed: %s", fif_status_string(st));
        hint("this is a bug in libfif - please report it with the image and the options used");
        fif_free(data); fif_image_free(&src);
        return kExitFail;
    }
    const double mp = (double)src.width * src.height / 1e6;
    std::printf("%-24s %d x %d  q=%d e=%d %s %s\n", argv[2], src.width, src.height,
                o.quality, o.effort,
                subsampling_name(o.subsampling),
                o.subsampling == FIF_SUBSAMPLING_400
                    ? "" : chroma_name(o.chroma_mode));
    std::printf("  size      %s   %.4f bpp   (%.1f : 1)\n",
                human((double)n).c_str(), est.bits_per_pixel,
                (double)src.width * src.height * src.channels / (double)n);
    std::vector<uint8_t> luma;
    const fif_image ref = (dec.channels == src.channels)
                        ? src : to_luma(src, luma);
    std::printf("  psnr      %.2f dB\n", fif_psnr(&ref, &dec));
    std::printf("  ssim      %.4f\n", fif_ssim(&ref, &dec));
    std::printf("  ms-ssim   %.4f\n", fif_ms_ssim(&ref, &dec));
    std::printf("  encode    %.3f s   %.2f Mpixel/s\n", est.encode_seconds,
                rate(mp, est.encode_seconds));
    std::printf("  decode    %.3f s   %.2f Mpixel/s   %d iterations\n", dsec,
                rate(mp, dsec), dst.iterations_used);
    /* The codec's own accounting is for whoever tunes it, not for someone
     * trying out settings; --stats asks for it, as with fif encode. */
    if (want_stats) {
        std::printf("  collage   %.2f dB   (encoder-side, before iteration)\n", est.collage_psnr);
        std::printf("  blocks    %lld (%lld fractal, %lld flat), %lld evaluations\n",
                    (long long)est.blocks_total, (long long)est.blocks_fractal,
                    (long long)est.blocks_flat, (long long)est.candidate_evals);
        print_bits(est);
    }
    fif_free(data);
    fif_image_free(&src);
    fif_image_free(&dec);
    return 0;
}

/* The name the program was started as - "fif", "fif.exe" - so the banner
 * names the file the user actually has.  On Windows argv[0] is whatever was
 * typed, often without ".exe"; the module path is the file itself. */
std::string exe_name(const char *argv0) {
#ifdef _WIN32
    wchar_t w[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, w, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        std::string u(len > 1 ? (size_t)len - 1 : 0, '\0');
        if (len > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &u[0], len, nullptr, nullptr);
        return base_name(u.c_str());
    }
#endif
    const char *b = argv0 && *argv0 ? base_name(argv0) : "fif";
    return *b ? b : "fif";
}

/* Who made the tool and the format, on every run.  On stderr, like every
 * other message: stdout carries data - fif info's report, fif debug's frame
 * stream - and a banner in it would break every script that reads it. */
void print_banner(const char *argv0) {
    std::fprintf(stderr,
        "%s v%d.%d with libfif v%d.%d - (c) 2026 Oskar Behzadpour\n"
        "Supports the FIF2 format v%d.%d by Rezar Behzad\n"
        "More info: www.FractalImageCompression.com\n\n",
        exe_name(argv0).c_str(), kCliVersionMajor, kCliVersionMinor,
        FIF_VERSION_MAJOR, FIF_VERSION_MINOR, fif::kVerMajor, fif::kVerMinor);
}

/* The three versions in the order they wrap each other - the program, the
 * library inside it, the format the library implements - with what this
 * build can do and where the rest lives.  On stdout: it was asked for. */
int cmd_version(const Command &cmd, int argc, char **argv) {
    if (argc > 2) { unknown_option(cmd, argv[2]); return kExitUsage; }
    std::string build = std::string("SIMD ") + fif_simd_name();
    if (kHavePng) build += ", PNG";
    if (kHaveJpeg) build += ", JPEG";
    if (fif::have_zlib()) build += ", zlib";
    const std::string exe = exe_name(argv[0]);
    std::printf("  %-12s %-9s %s\n", "Component", "Version", "Description");
    std::printf("  %-12s %-9s %s\n", "---------", "-------", "-----------");
    std::printf("  %-12s %d.%-7d %s\n", exe.c_str(), kCliVersionMajor, kCliVersionMinor,
                "command-line tool");
    std::printf("  %-12s %d.%-7d library (%s)\n", "libfif", FIF_VERSION_MAJOR, FIF_VERSION_MINOR,
                build.c_str());
    std::printf("  %-12s %d.%-7d file format\n", "FIF2", fif::kVerMajor, fif::kVerMinor);
    std::printf("\n");
    std::printf("  %-12s %s\n", "Source", "https://github.com/FIF2-Project/libfif");
    std::printf("  %-12s %s\n", "Format", "https://fif2.org");
    std::printf("  %-12s %s\n", "About", "https://www.FractalImageCompression.com");
    return 0;
}

} /* namespace */

int main(int argc, char **argv) {
#ifdef _WIN32
    {
        int wn = 0;
        LPWSTR *wv = CommandLineToArgvW(GetCommandLineW(), &wn);
        if (wv && wn == argc) {
            for (int k = 0; k < wn; ++k) {
                const int len = WideCharToMultiByte(CP_UTF8, 0, wv[k], -1, nullptr, 0, nullptr, nullptr);
                std::string u(len > 0 ? (size_t)len - 1 : 0, '\0');
                if (len > 1) WideCharToMultiByte(CP_UTF8, 0, wv[k], -1, &u[0], len, nullptr, nullptr);
                g.argv_utf8.push_back(std::move(u));
            }
        }
        if (wv) LocalFree(wv);
    }
#else
    for (int k = 0; k < argc; ++k) g.argv_utf8.emplace_back(argv[k]);
#endif
    /* --quiet and --strict belong to no single command: they say how the tool
     * should talk and what it should return, for every one of them.  Taken out
     * of the argument list here, before anything is printed, so that the file
     * names keep their places and the banner can already be suppressed. */
    std::vector<char *> args(argv, argv + argc);
    for (size_t i = 2; i < args.size();) {
        const bool quiet = !std::strcmp(args[i], "--quiet");
        const bool strict = !std::strcmp(args[i], "--strict");
        if (quiet || strict) {
            if (quiet) g.quiet = true; else g.strict = true;
            args.erase(args.begin() + (long)i);
            if (i < g.argv_utf8.size()) g.argv_utf8.erase(g.argv_utf8.begin() + (long)i);
        } else {
            ++i;
        }
    }
    argc = (int)args.size();
    argv = args.data();

    if (!g.quiet) print_banner(argc > 0 ? argv[0] : nullptr);
    if (argc < 2) {
        std::fprintf(stderr, "Run 'fif help' for usage.\n");
        return kExitUsage;
    }
    const char *name = argv[1];
    const auto is_help = [](const char *a) {
        return !std::strcmp(a, "-h") || !std::strcmp(a, "--help") || !std::strcmp(a, "help");
    };
    /* `fif help` and `fif help <command>`. */
    if (is_help(name)) {
        if (argc < 3) { std::fputs(kHelpOverview, stdout); return 0; }
        name = argv[2];
        if (const Command *c = find_command(name)) { std::fputs(c->help, stdout); return 0; }
        report_error("there is no command '%s' to explain", name);
        if (const char *closest_name = closest_command(name)) hint("did you mean 'fif help %s'?", closest_name);
        hint("'fif help' lists the commands");
        return kExitUsage;
    }
    if (!std::strcmp(name, "--version")) name = "version";
    const Command *cmd = find_command(name);
    /* `fif decode --help`, `fif decode -h` anywhere among the arguments, and
     * `fif decode help` - the last only as the sole argument, so that a file
     * named "help" can still be decoded. */
    if (cmd) {
        bool want = argc == 3 && !std::strcmp(argv[2], "help");
        for (int i = 2; i < argc && !want; ++i)
            want = !std::strcmp(argv[i], "-h") || !std::strcmp(argv[i], "--help");
        if (want) { std::fputs(cmd->help, stdout); return 0; }
    }
    if (!cmd) {
        report_error("unknown command '%s'", name);
        /* `fif photo.png photo.fif`: the files are right, the verb is missing. */
        const Kind k = kind_from_ext(name);
        if (k == Kind::Fif2)
            hint("to turn a FIF2 file into a picture: fif decode \"%s\"", name);
        else if (k != Kind::Unknown)
            hint("to compress a picture: fif encode \"%s\"", name);
        else if (const char *closest_name = closest_command(name))
            hint("did you mean 'fif %s'?", closest_name);
        hint("commands: encode, decode, info, thumbnail, compare, bench, version, debug; "
             "'fif help' explains them");
        return kExitUsage;
    }
    /* -y belongs to every command that writes a file and may stand anywhere
     * after the command; taken out here so that the file names keep their
     * places. */
    if (has_option(*cmd, "-y")) {
        for (size_t i = 2; i < args.size();) {
            if (!std::strcmp(args[i], "-y") || !std::strcmp(args[i], "--overwrite")) {
                g.overwrite = true;
                args.erase(args.begin() + (long)i);
                if (i < g.argv_utf8.size()) g.argv_utf8.erase(g.argv_utf8.begin() + (long)i);
            } else {
                ++i;
            }
        }
        argc = (int)args.size();
        argv = args.data();
    }
    const int rc = cmd->run(*cmd, argc, argv);
    /* A warning never changes the code on its own - a decode of a file that
     * was cut short still succeeded - so a script that has to notice one asks
     * for it. */
    if (rc == 0 && g.strict && g.warnings > 0) return kExitWarn;
    return rc;
}
