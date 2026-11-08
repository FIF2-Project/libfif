# Design notes

How libfif encodes and decodes FIF2, and why: the measurements behind each
design decision, including the ideas that were tried and removed.

For the file format itself, see [fif2.org](https://fif2.org).
For using the tool and the library, see [Using the fif tool](https://fif2.org/lib-cli.html) and [Using the library](https://fif2.org/lib-api.html) on fif2.org.

## How it encodes

Each tile is partitioned by a rate-distortion quadtree: a block is split only
when the distortion it saves beats `lambda` times the bits it costs, with
`lambda` driven by `-q`. Every leaf is matched against a pool of domain blocks —
twice its size, decimated by 2×2 averaging — under the eight dihedral
transforms, and coded as `dc + s·(D − mean D)`.

The contrast `s` is never searched: for a fixed domain it has a closed form, so
the only per-candidate work is one inner product. That inner product is the
whole hot loop, and it is the only thing worth vectorising (`src/kernels.h`,
which carries NEON, AVX, SSE2 and scalar kernels and picks one at compile time;
`fif version` reports which). It is an encoder-side kernel only — the decoder
never calls it — so the width can change which domain the search picks, and
therefore the bitstream, but not how a stream decodes.

Candidates come from two complementary sources:

- a **local window** in the domain grid, which covers the short-range
  self-similarity that dominates smooth photographic material, and
- a **kd-tree over feature keys** — blocks reduced to 4×4, centred, brought
  into a canonical dihedral orientation, unit-normalised, DCT'd, truncated to
  the 8 lowest AC coefficients. For centred unit-norm vectors the least-squares
  error is monotone in euclidean distance, so nearest-neighbour search in this
  space *is* the search for the best affine match (Saupe, 1995).

**Grid density matters far more than search reach.** With the domain grid at
half the block size, a good match sits within a block or two, so the window can
shrink as the grid tightens. Measured against the coarse grid with a wide
window it replaced: smaller files, half a dB better, and three times faster. The
index does not get more expensive either, because a closer match means a smaller
displacement from the predictor.

Which one wins is a property of the picture, not of the settings. On content
with long-range self-similarity the tree is worth 8 dB over a local window; on
smooth gradients it costs rate for nothing, because every useful match is next
door and a distant domain index codes expensively. From effort 3 up the encoder
therefore codes a tile **both ways and keeps whichever came out cheaper**,
judged on the payload that was actually produced and the error that actually
remains — no rate model, and no assumption about what the requantisation did.

Doing that for every tile doubles the search, so it is done on a sample spread
across the image and, when the sample agrees, the rest is committed to the
winner. Taking the sample from the front instead measured clearly worse: the
leading tiles are one strip of the picture and routinely disagree with it.

Candidate selection is itself rate-distortion optimised: the domain index is
coded as a displacement from the neighbouring block's domain, so a match on the
far side of the tile costs real bits, and the search is charged for them the way
a video encoder is charged for a long motion vector.

## Colour

`--subsampling 444|420|400` and `--chroma shared|from-luma`.

In **shared** mode Cb and Cr inherit the luma quadtree, domain index and
dihedral transform, and re-fit only their own DC and contrast. At 4:2:0 this
was strictly better than coding chroma with its own quadtree - smaller files
*and* higher quality, because the inherited partition reaches 2×2 chroma blocks
for the price of a mode bit - which is why the independent mode was removed.

**from-luma** takes the idea one step further: the domain of a chroma leaf is
the luma block it sits on top of, so there is no index and no symmetry to code
and — the part that matters — the chroma planes are no longer part of the
contraction. They are a function of the converged luma, painted in one pass.
Measured on an 8.3 MP photograph at 4:4:4, decode drops from 0.132 s to 0.078 s
(41% faster) for 0.05 dB and 0.9% more bytes. At 4:2:0 there is simply less
chroma to save on and the gain is a few percent.

**4:0:0** throws the colour away and codes luma only. The header then reports
one channel and the Gray colour model. On the same photograph it is about 30%
of the file gone at q=60.

## Planar leaves

Most leaves in a photograph carry no transform at all — at q=30 on an 8.3 MP
picture, 86% of them — and a leaf with nothing but a mean has to be split
whenever the region under it is a ramp rather than a step. So a leaf with no
transform may instead carry a linear gradient: the coded DC stays the block
mean and two small residuals give its end-to-end rise in x and y.

The point is not the two coefficients, it is the splits they prevent. At a
matched rate this is worth **+0.3 to +0.75 dB** on a photograph, and it lifts
the codec's quality ceiling by about 0.9 dB — the old ceiling was set by
approximating smooth gradients with `min_block` steps, and no amount of rate
could buy past it.

It has to be offered to the chroma planes as well, not only luma. Measured with
luma-only planar, colour images got *worse*: a coarser luma quadtree is
inherited by shared chroma, which then had no way to answer a big smooth block
with anything but a bigger step. `--planar 0` turns the whole thing off.

The flag goes in the base layer and the two coefficients in the detail layer.
Putting the coefficients in the base as well is the obvious reading of "the DC
field belongs to the base", and it is wrong: they are a fifth of the stream, and
they took the base layer from 48% to 71% of the file to buy a first picture only
half a dB better. Split as it is, the progressive curve on an 8.3 MP photograph
runs 27.3 dB at 48% of the file, 28.9 at 75%, 32.1 at 100%.

## Entropy coding

An adaptive binary range coder (LZMA's variant), driven by contexts built from
block size and the already-coded neighbours. Nothing is transmitted to set it
up: adaptive models learn the picture as they go, whereas rANS would need a
frequency table per context and the tables would cost more than the contexts
save.

Starting every context at one half costs real bits while the models learn —
about 9% of the payload on a 128-pixel tile. The contexts therefore start from
probabilities trained offline over a set of images (`tools/train_contexts.sh`
rebuilds `src/ctxtable.inc`), which costs nothing to transmit and keeps tiles
independently decodable.

What actually pays is the prediction in front of the coder, not the coder:

- **DC** is a residual against a gradient-adjusted prediction (the median edge
  detector from JPEG-LS), under contexts split by how busy the neighbourhood is.
  An 8-bit field costs about 2.5 bits.
- **The domain index** is a displacement against the component-wise median of
  three neighbours' domains, carried along the range displacement — the same
  construction H.264 uses for motion vectors. A 12-bit index costs about 5.5.
- **DC and contrast are requantised at coding time**: with the contexts live,
  the coder knows what each neighbouring quantisation level would really cost,
  and a level one step further out is often much cheaper for almost the same
  error. Worth 0.05-0.15 dB at a matched rate across the whole range.

Against raw bit fields the stage is worth about 30%, and the predictors and
requantisation another 12% on top.

Three things were tried and measured *worse*, and are not in the default path:
conditioning contrast on the neighbouring block's contrast, or on local
activity, and conditioning the dihedral transform on the neighbour's. All three
dilute the models faster than the added information pays back, which says those
fields already sit close to their conditional entropy.

`fif bench --stats` prints the budget per symbol class.

## How it compares

Measured on an 8.3 MP photograph — a field of tulips at sunrise, busy
foliage over a smooth sky — with mozjpeg's `cjpeg` and libwebp's `cwebp`. All
three encode the same PPM and are measured against it, so nothing differs but
the codec. Quality is read off each curve at the other's file size.

| file size | FIF2 | JPEG | WebP |
|---|---|---|---|
| 180 KB | **28.3 dB / 0.905** | 24.58 / 0.766 | 29.07 / 0.919 |
| 250 KB | **29.0 / 0.917** | 27.57 / 0.868 | 30.4 / 0.939 |
| 325 KB | **29.99 / 0.941** | 28.98 / 0.910 | 31.2 / 0.949 |
| 510 KB | 31.1 / **0.962** | 31.11 / 0.9529 | 33.1 / 0.966 |
| 1000 KB | 32.06 / 0.979 | **34.1 / 0.980** | 35.9 / 0.981 |

The shape is the one the fractal literature predicts, and it is worth being
plain about both ends of it.

Below about half a megabyte on this picture FIF2 beats JPEG clearly — nearly
4 dB at 180 KB, and 0.14 of MS-SSIM, which is the larger number. DCT at that
rate has nothing left but block means; a fractal code still has real structure
to copy from elsewhere in the image. FIF2 also simply goes lower: 45 KB at
25.5 dB, a rate `cjpeg` will not produce at all.

Above that it loses, and the reason is structural rather than a missing
optimisation. A range block can only be as accurate as the best domain block in
the picture, and there is no residual channel to code the difference. The curve
therefore flattens — on this image it saturates near 32.5 dB whatever the rate —
where a transform codec keeps buying accuracy for bits all the way to lossless.

WebP is ahead throughout, by 0.7 to 2.5 dB. That is the honest position: this is
competitive with JPEG at low rates and better than it there, and it is not
competitive with a modern intra-prediction codec.

Decode throughput on the same picture is 80-120 Mpixel/s, the same order as
WebP and well short of libjpeg-turbo. Encoding 8.3 MP at `-q 50 -e 2` takes
0.45 s and produces the same file as `-e 5` does in 3.5 s: the per-tile strategy
probe picks the local-window search on this picture anyway, so the feature-space
tree the higher effort adds is work that buys nothing here.

## What did not work

Removed after measurement:

- **Variance-adaptive rate allocation**. The premise is that
  quantisation noise hides inside texture. It does not transfer here: in this
  codec the multiplier mainly controls *block size*, and coarse blocks produce
  structure rather than maskable noise. At a matched rate it cost 0.38 dB and
  0.008 SSIM and made measured blockiness 32% worse.
- **A second rate-distortion pass** using the symbol costs
  measured from the first. It moves what `-q` means without moving the curve —
  the two RD curves lie within 0.04 dB of each other — for 25% more encode time.

Removed outright:

- **Merge mode**, where a block inherits a neighbour's whole transform for one
  bin instead of coding thirteen bits. It fired on only 2-8% of blocks: the
  median predictor already codes a matching *displacement* for about a bit, so
  merge was left inheriting only the symmetry and the contrast, and those rarely
  match. The flag cost more than the hits returned, about 0.1 dB at a matched
  rate.
- **Overlapped reconstruction**, where each range block is rendered wider than
  its nominal size and the overlaps are blended, with the encoder refitting DC
  and contrast for the blend (after Øien and Lepsøy). At a matched file size on
  an 8.3 MP photograph: −0.03 dB and blockiness from 1.62 to 0.83, for about 80%
  more decode time and a whole second reconstruction path in the format. The
  non-normative deblocking filter removes most seams at no format cost.
- **Independent chroma quadtrees**. At 4:2:0 strictly worse than shared chroma;
  at 4:4:4 a rate/quality trade the quality setting already offers.

- **Bounding the search by feature distance.** The truncated-DCT key looks like
  it should cap how good a match a candidate can be, but the key is made
  invariant to the eight dihedral transforms by canonicalising the orientation,
  and the canonical alignment is not always the best of the eight. Using it to
  stop the candidate scan early cost 0.85 dB.

## Streaming

The transform section is laid out for it: a directory of every tile's two layer
lengths, then **every tile's base layer**, then every tile's detail layer. That
gives two independent axes. Tile geometry is not stored — it follows from the
image dimensions and the tile size — so the directory is two varints per tile
rather than twenty bytes, and there is no second version of the geometry for a
corrupt file to disagree with.

*By quality.* A prefix of the file is a valid stream. `fif info` reports where
the base layer ends — around half the file at quality 65 — and a reader with
only that has the whole picture at DC resolution rather than part of a sharp
one. Measured on the 8.3 MP photograph at `-q 65`:

| prefix | share | PSNR | MS-SSIM |
|---|---|---|---|
| base only | 48.4% | 27.32 | 0.9306 |
| | 55% | 27.48 | 0.9405 |
| | 65% | 28.02 | 0.9484 |
| | 75% | 28.86 | 0.9570 |
| | 90% | 30.84 | 0.9719 |
| complete | 100% | 32.06 | 0.9794 |

The property that makes this worth having: every prefix decodes, and a longer
prefix is never a worse picture.

The split costs nothing measurable: the two layers use disjoint context models,
so all it adds is one coder flush per stream.

*By region.* Tiles are independent, and the directory sits at the front, so a
viewer can compute the byte ranges of exactly the tiles its viewport needs and
request those without scanning the file.

Truncation anywhere is handled: tiles whose base layer did not arrive stay
mid-grey, and tiles that have a base layer but no detail decode at DC.

## Decoding artefacts

Fractal seams need more care than transform-codec blocking: neighbouring blocks
are filled from unrelated parts of the picture, so their errors do not cancel at
the boundary, and the decoder re-reads its own output as domain data, which
reproduces any seam left standing. The filter therefore runs inside the
iteration, and its correction is a ramp whose width grows with the blocks that
meet at the seam — patching the two samples either side does nothing for a step
between two 32-pixel blocks.

At quality 20 this takes measured blockiness from 5.0 to 1.9, where 1.0 is an
uncompressed picture, while PSNR and MS-SSIM both improve slightly.

Filtering from the first iteration would slow the whole trajectory, so the
decoder converges the unfiltered attractor first and only then switches the
filter on — the second contraction starts close to its own fixed point and two
filtered steps carry essentially all of the benefit. The iteration is also
seeded with the coded block means rather than flat grey, since the DC is carried
exactly in the stream. Together that is nine iterations where filtering
throughout from grey took sixteen.

`--deblock 0` gives the unfiltered reconstruction, `--deblock N` overrides the
strength the encoder recommended. The number of filtered iterations after
convergence is fixed at three.

## Context table guard

The trained probability table and the `Contexts` struct it seeds are tied
together by a `static_assert`: adding a context once grew the struct past the
table, and `reset()` copied 112 bytes beyond the array until a test caught it.
A stale table now fails the build.
