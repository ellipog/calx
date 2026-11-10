---
title: std.image.jpeg
description: JPEG files read as libjpeg-turbo reads them, baseline and progressive, and written byte for byte as Pillow writes them, with pixels 8 bits a channel.
---

# `std.image.jpeg`

JPEG files read as libjpeg-turbo reads them, so the pixels are the ones Pillow and Java's ImageIO give. They're written byte for byte as Pillow writes them. [std.image](image.md) reads and writes JPEGs with these, and most programs want it instead. Use this module for raw pixels without an `Image`.

```calx
import std.image.jpeg
import std.image.jpeg.{Sampling}

fn main() {
    // An 8 by 8 gray ramp at quality 90. Gray is always full size, whatever the sampling.
    var pixels: List<u8> = []
    for i in 0u32..64 {
        pixels.push(u8.wrap(i * 4))
    }
    let file = jpeg.encode(8, 8, 1, pixels, 90, Sampling.S444)
    let d = jpeg.decode(file)!
    println("{d.width}x{d.height}x{d.channels} {file.len()}")    // out: 8x8x1 344
    match jpeg.decode(file[..100]) {
        ok(_) => println("read"),
        err(e) => println(e),    // out: the JPEG file ends in the middle of its frame header
    }
}
```

| Item | Gives |
| --- | --- |
| `decode(data: List<u8>) -> Decoded ! String` | The pixels of a JPEG file, or why they can't be read |
| `encode(width, height, channels: usize, pixels: List<u8>, quality: u32 = 75, sampling: Sampling = Sampling.S420, optimize: bool = false, progressive: bool = false) -> List<u8>` | The pixels as a JPEG, baseline unless `progressive`. `channels` is 1 for gray or 3 for RGB |

`Decoded` has `width`, `height`, `channels` (1 for gray, 3 for RGB) and `pixels: List<u8>`. The pixels run row by row from the top, with each pixel's channels in turn.

## Reading

`decode` reads these:

- baseline and progressive JPEGs;
- any of the usual chroma subsamplings;
- restart markers;
- gray, YCbCr, RGB, YCCK and CMYK, including Adobe's inverted CMYK.

One component comes out gray, and three or four come out RGB, converted as the file's JFIF and Adobe markers say, as libjpeg decides. The pixels are libjpeg-turbo's with its default settings, which are what Pillow and Java use:

- the accurate integer inverse DCT;
- smooth ("fancy") upsampling of subsampled chroma;
- libjpeg's fixed-point YCbCr to RGB.

Arithmetic coding, 12-bit samples, and lossless and hierarchical JPEGs aren't read, and most decoders don't read them either. A file that ends before its image does is an error. Markers such as EXIF and ICC profiles are skipped.

## Writing

`encode` writes the file libjpeg writes with Pillow's settings, so the bytes are the same as Pillow's:

- `quality` runs from 1 to 100, with libjpeg's scaling of the Annex K tables. 75, the default, is libjpeg's and Pillow's.
- `sampling` says how much color to keep.
- `optimize` builds Huffman tables for this image, as Pillow's `optimize=True` does. The file comes out a few percent smaller, and takes longer to write.
- `progressive` writes a progressive JPEG, as Pillow's `progressive=True` does. A browser can show it blurred while it loads, then sharper as the rest comes. It comes out about the size of an `optimize` file, and takes four to five times as long to write as a baseline one.

The file starts with a JFIF header, and its quantization and Huffman tables come in libjpeg's order. A gray image is always written at full size. `encode` panics if `pixels` is the wrong length, or if a side is longer than 65,535 pixels, which a JPEG can't be.

| `Sampling` | Chroma | Pillow's `subsampling` |
| --- | --- | --- |
| `S444` | full size: the sharpest color, and the biggest file | `0` |
| `S422` | half width | `1` |
| `S420` | half width and half height: the default, and what cameras write | `2` |

A progressive file is libjpeg's `jpeg_simple_progression`, the scans Pillow writes:

- For color: the three DCs to all but their last bit, together.
- Then each component's AC coefficients on their own: luma's first five to all but two bits, both chromas' to all but one, and luma's others to all but two.
- Then luma's next bit, the DCs' last bit, and the last bit of each.
- A gray image has the same scans for its one component, six in all.
- Each scan gets Huffman tables made for it, so `optimize` changes nothing.

## How it's built

The reader and the file's layout are Calx, in [std/image/jpeg.calx](../../std/image/jpeg.calx). The writer's pixel work and Huffman coding are C, in [runtime/calx_jpeg.c](../../runtime/calx_jpeg.c), which is linked only into programs that write JPEGs. Both use libjpeg's arithmetic, so the bytes are Pillow's:

- RGB to YCbCr in 16-bit fixed point.
- The chroma averaged with its alternating rounding.
- The accurate integer forward DCT, each coefficient divided by its quantizer times 8, a half rounded away from zero.

The image is worked through one row of MCUs at a time, as libjpeg does, so the samples stay in the cache. On processors with AVX2, which release builds target, two blocks go through the DCT at once in 16-bit lanes. Every sum the DCT adds in 16 bits fits, and the division is two 16-bit multiplies that are exact for every quantizer and coefficient. The results are the same integers as the plain C, which other builds use. Both match Pillow's files byte for byte on 525 cases: images from 1 × 1 to 1600 × 900, at several qualities, all three samplings, both kinds of tables, and progressive. The tests check some of them in debug and release builds.

Writing the 1600 × 900 photo of [std.image's timings](image.md#speed), in the same conditions, best of 60 runs, against Pillow 12.3:

| Settings | Calx | Pillow |
| --- | --- | --- |
| Quality 75, `S420` | 1.26 ms | 1.97 ms |
| Quality 90, `S444` | 2.40 ms | 3.40 ms |
| Quality 75, `S422` | 1.45 ms | 2.30 ms |
| Quality 75, `S420`, `optimize` | 1.62 ms | 5.03 ms |
| Quality 75, gray | 0.71 ms | 1.65 ms |
| Quality 75, `S420`, `progressive` | 5.64 ms | 10.12 ms |
| Quality 90, `S444`, `progressive` | 10.71 ms | 18.38 ms |
| Quality 75, gray, `progressive` | 4.71 ms | 8.33 ms |

A progressive file's coefficients are kept for the whole image, then each scan goes through them twice: once to count its codes for its tables, then to write them. Before the scans, each block's DC and its nonzero AC coefficients in zigzag order are packed together, so a scan reads those rather than every block's 128 bytes. In an AC scan the mask of a block's nonzero coefficients takes it straight to them: a coefficient that's zero is zero at every bit.
