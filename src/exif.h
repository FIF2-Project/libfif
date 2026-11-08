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
/* EXIF, read out of a JPEG or PNG file in memory and turned into chunks.
 *
 * Byte parsing only - no libjpeg, no libpng - so a build without either still
 * carries the camera's metadata over from a file it gets some other way.  The
 * TIFF structure inside EXIF is attacker-controlled like everything else in a
 * file: every offset is checked against the block, and each IFD is visited at
 * most once. */
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fif {

struct ExifEntry {
    char type[5];               /* "ORGN": everything here comes from the source */
    std::string name;
    uint8_t value_type;         /* fif_value_type */
    std::string value;          /* UTF-8 text, or the little-endian number */
};

struct Exif {
    /* 1..8 as in the TIFF Orientation tag; 0 when the file states none. */
    int orientation = 0;
    std::vector<ExifEntry> entries;
};

/* Finds the EXIF block of a JPEG (APP1 "Exif") or PNG (eXIf chunk) and reads
 * the tags worth keeping: camera, lens, date taken, exposure, author,
 * copyright, description, software.  GPS position is left out on purpose -
 * where a photo was taken is not something to pass on without being asked.
 * False when there is no EXIF block or it is unreadable. */
bool read_exif(const uint8_t *file, size_t size, Exif &out);

} /* namespace fif */
