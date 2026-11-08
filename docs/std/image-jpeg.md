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
| `encode(width, height, channels: usize, pixels: List<u8>, quality: u32 = 75, sampling: Sampling = Sampling.S420, optimize: bool = false) -> List<u8>` | The pixels as a baseline JPEG. `channels` is 1 for gray or 3 for RGB |

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

The file starts with a JFIF header, and its quantization and Huffman tables come in libjpeg's order. A gray image is always written at full size. `encode` panics if `pixels` is the wrong length, or if a side is longer than 65,535 pixels, which a JPEG can't be.

| `Sampling` | Chroma | Pillow's `subsampling` |
| --- | --- | --- |
| `S444` | full size: the sharpest color, and the biggest file | `0` |
| `S422` | half width | `1` |
| `S420` | half width and half height: the default, and what cameras write | `2` |

Progressive JPEGs aren't written yet.
