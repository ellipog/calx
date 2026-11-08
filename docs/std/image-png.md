---
title: std.image.png
description: PNG files read and written as libpng does it - every color type and bit depth, interlacing, palettes and transparency, CRCs checked - with pixels 8 bits a channel.
---

# `std.image.png`

PNG files, read and written as libpng reads and writes them. Pixels come out 8 bits a channel, and go in the same way. [std.image](image.md) reads and writes PNGs with these, and most programs want it instead. Use this module for raw pixels without an `Image`.

```calx
import std.image.png

fn main() {
    // A 2 by 1 RGBA image: red, then clear blue.
    let file = png.encode(2, 1, 4, [255, 0, 0, 255, 0, 0, 255, 0])
    let d = png.decode(file)!
    println("{d.width}x{d.height}x{d.channels} {d.pixels}")    // out: 2x1x4 [255, 0, 0, 255, 0, 0, 255, 0]

    // Two colors in a palette take a bit a pixel.
    let small = png.encode_indexed(16, 1, [0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1], [0, 0, 0, 255, 255, 255, 255, 255])
    println("{png.decode(small)!.channels}")    // out: 3
    match png.decode([137, 80, 78, 71, 13, 10, 26, 10]) {
        ok(_) => println("read"),
        err(e) => println(e),    // out: the PNG file has no IHDR chunk
    }
}
```

| Item | Gives |
| --- | --- |
| `decode(bytes: List<u8>) -> Decoded ! String` | The pixels of a PNG file, or why they can't be read |
| `encode(width, height, channels: usize, pixels: List<u8>, level: u32 = 6) -> List<u8>` | The pixels as a PNG. `channels` is 1 for gray, 2 for gray and alpha, 3 for RGB and 4 for RGBA |
| `encode_indexed(width, height, indexes: List<u8>, palette: List<u8>, level: u32 = 6) -> List<u8>` | Palette indexes as a PNG. `palette` holds each color's red, green, blue and alpha, for 1 to 256 colors |

`Decoded` has `width`, `height`, `channels` (1 to 4, as above) and `pixels: List<u8>`. The pixels run row by row from the top, with each pixel's channels in turn.

## Reading

Every color type and bit depth is read, interlaced (Adam7) or not:

- **A palette** becomes RGB. It becomes RGBA when its tRNS chunk makes any color transparent.
- **A gray or RGB image** whose tRNS names a transparent color gets alpha.
- **Gray of 1, 2 or 4 bits** is scaled up to 8, so its white is 255.
- **16 bits** keeps the high byte of each value, as libpng's `png_set_strip_16` does.
- **CRCs** are checked. A broken chunk that the image needs is an error. One it doesn't need, such as a text chunk, is skipped, as libpng skips it.
- **Ancillary chunks** such as gamma, color profiles and text are not applied or kept.

## Writing

`encode` filters each row with whichever of PNG's five filters leaves the smallest sum of its bytes taken as signed, as libpng chooses. It then compresses the rows with zlib at `level`:

- 0 stores them.
- 1 is the fastest.
- 9 is the smallest.
- 6, the default, is libpng's and Pillow's.

`encode_indexed` writes each index in as few bits as the palette's size allows: 1, 2, 4 or 8, as Pillow writes them. Rows aren't filtered, as libpng advises for palettes. The alpha goes in a tRNS chunk, trimmed after the last color that isn't opaque, and is left out when every color is opaque.

Both panic if the pixels or indexes are the wrong length for the size, or if an index is past the palette's end.
