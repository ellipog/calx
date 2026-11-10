---
title: std.image
description: Images read from PNG, JPEG and ICO files, resized, blurred, cropped, turned, drawn on, blended, quantized and written back, with Pillow's pixels and Pillow's bytes.
---

# `std.image`

Images in memory, 8 bits a channel: read from PNG, JPEG and ICO files, changed, and written back. It does what a script would do with Python's Pillow, and gives the same pixels:

- Resizing and blurring give Pillow's results exactly, and so does every other operation that Pillow also has.
- PNG and JPEG files are read as libpng and libjpeg-turbo read them.
- JPEG files are written byte for byte as Pillow writes them.

Nothing needs installing: it's all Calx, apart from the zlib that [std.zlib](zlib.md) uses.

```calx
import std.fs.{remove_dir_all, temp_dir}
import std.image.{Color, Filter, Image, Kind}

fn main() ! Error {
    var im = Image.new(64, 48, Kind.Rgb, Color.hex("#f8f8f5") ?? Color.gray(255))
    im.fill_rect(8, 8, 24, 16, Color.rgb(24, 56, 208))
    println("{im} {im.get(10, 10)} {im.get(40, 10)}")    // out: Image(64x48 Rgb) #1838d0 #f8f8f5

    let small = im.resize(32, 24, Filter.Lanczos).blur(1.0)
    println("{small} {small.get(4, 4)}")    // out: Image(32x24 Rgb) #8a9ae2

    let dir = try temp_dir()
    try small.save("{dir}/card.png")
    try small.save("{dir}/card.jpg")
    let back = try Image.open("{dir}/card.png")
    println("{back == small} {try Image.open("{dir}/card.jpg")}")    // out: true Image(32x24 Rgb)
    try remove_dir_all(dir)
}
```

## Images

An `Image` holds its size, its `Kind`, and `pixels: List<u8>`. The pixels run row by row from the top, with each pixel's channels in turn, so pixel (x, y) starts at `(y × width + x) × channels`. `pixels` is public, for loops of your own. Each method checks that its length is still `width × height × channels`, and panics if it isn't.

| `Kind` | Each pixel | Pillow's mode |
| --- | --- | --- |
| `Gray` | 1 byte, its brightness | `L` |
| `GrayAlpha` | brightness, then alpha | `LA` |
| `Rgb` | red, green, blue | `RGB` |
| `Rgba` | red, green, blue, alpha | `RGBA` |

`kind.channels()` gives the bytes a pixel takes, and `kind.has_alpha()` whether it has alpha.

A `Color` has public `r`, `g`, `b` and `a`, each a `u8`. An alpha of 255 is opaque and 0 is clear.

| Item | Gives |
| --- | --- |
| `Color.rgb(r, g, b)`, `Color.rgba(r, g, b, a)`, `Color.gray(v)` | A color; the first and last are opaque |
| `Color.hex(text: String) -> Color?` | The color a CSS hex code names, with or without its `#`: `#1838D0`, `#18d`, `#1838D080`, `#18d8`. `none` for anything else |
| `c.luma() -> u8` | Its brightness, as Pillow turns RGB into gray: ITU-R 601's weights in 16-bit fixed point |

A color prints as CSS writes it, `#1838d0`, with the alpha after only when it isn't 255: `#1838d080`. An image prints as `Image(640x480 Rgb)`. Both have `==`.

### Making, reading and writing

| Item | Gives |
| --- | --- |
| `Image.new(width, height, kind: Kind, color: Color) -> Image` | An image filled with `color` |
| `Image.from_pixels(width, height, kind: Kind, pixels: own List<u8>) -> Image` | An image of these pixels. Panics unless there are `width × height × channels` of them |
| `Image.open(path: String) -> Image ! Error` | The image in a PNG, JPEG or ICO file |
| `Image.decode(bytes: List<u8>) -> Image ! String` | The image in a file's bytes, told apart by how they start, or why it can't be read |
| `im.save(path: String) ! Error` | Writes a `.png`, a `.jpg` or `.jpeg` at quality 75, or an `.ico`, by the name's end |
| `im.encode_png(level: u32 = 6) -> List<u8>` | The image as a PNG, at zlib `level` 0 to 9 |
| `im.encode_jpeg(quality: u32 = 75, sampling: Sampling = Sampling.S420, optimize: bool = false) -> List<u8>` | The image as a JPEG, byte for byte as Pillow's `save(f, "JPEG", quality=, subsampling=, optimize=)` writes it. Panics if the image has alpha |
| `im.width()`, `im.height()`, `im.kind()`, `im.channels()` | Its size and what its pixels hold |
| `im.get(x, y) -> Color` | The color of pixel (x, y) from the top left. A gray pixel gives the same R, G and B; with no alpha, it gives 255 |
| `im.set(x, y, color: Color)` | Sets pixel (x, y): its brightness in a gray image, and without alpha in one that has none |
| `im.copy() -> Image` | A copy |

These rules decide what you get back:

- **Every image is one of the four kinds.** Pillow keeps several more modes, but `std.image` does not:
  - A palette PNG loads as RGB, or as RGBA when it has transparency.
  - A gray or RGB PNG with a transparent color gets alpha.
  - A CMYK JPEG loads as RGB.
  - A 16-bit PNG keeps the high byte of each value, as libpng's `png_set_strip_16` does.
- **An ICO** gives its largest image. `.ico` saves the sizes Pillow writes, from 16 to 256 pixels, as many as fit in the image.
- **A JPEG has no alpha**, so `save` fails and `encode_jpeg` panics for an image that has alpha. Drop it first with `to(Kind.Rgb)`.
- **Supported formats are PNG, JPEG and ICO.** There is no GIF, WebP, TIFF or BMP. Metadata such as EXIF, ICC profiles and text chunks isn't kept, and EXIF rotation isn't applied, which is also Pillow's default.

`Sampling`, from [std.image.jpeg](image-jpeg.md), says how much color a JPEG keeps:

| `Sampling` | Chroma | Pillow's `subsampling` |
| --- | --- | --- |
| `S444` | full size | `0` |
| `S422` | half width | `1` |
| `S420` | half width and half height, the default | `2` |

A gray image is always written at full size, as Pillow writes it.

### Changing

Each of these returns a new image and leaves the original as it was.

| Item | Gives |
| --- | --- |
| `im.to(kind: Kind) -> Image` | The pixels as `kind` holds them, as Pillow's `convert` gives them: gray from RGB by ITU-R 601's weights, RGB from gray by repeating it, and alpha dropped, or 255 where there was none |
| `im.resize(width, height, filter: Filter = Filter.Bicubic) -> Image` | The image resized, Pillow's `resize` to the pixel |
| `im.thumbnail(width, height, filter: Filter = Filter.Bicubic) -> Image` | The image as big as fits in `width` by `height` without changing its proportions, and no bigger than it was. The size is the one Pillow's `thumbnail` picks |
| `im.crop(x, y, width, height) -> Image` | The rectangle whose top left is (x, y). Panics unless it's inside the image |
| `im.expand(left, top, right, bottom, color: Color) -> Image` | The image with a border of `color` around it |
| `im.blur(radius: f64) -> Image` | Pillow's `GaussianBlur(radius)`: three box blurs whose sizes make a Gaussian of that spread |
| `im.box_blur(radius: f64) -> Image` | Pillow's `BoxBlur(radius)`. A radius that isn't whole weights the farthest pixels by its fraction |
| `im.flip_horizontal()`, `im.flip_vertical()` | The image mirrored left to right, or upside down |
| `im.rotate_left()`, `im.rotate_right()`, `im.rotate_180()` | The image turned counterclockwise, clockwise, or half a turn: Pillow's `ROTATE_90`, `ROTATE_270` and `ROTATE_180` |
| `im.channel(index: usize) -> Image` | One channel as a gray image. 0 is red, or the brightness of a gray image, and the last is the alpha |
| `im.with_alpha(mask: Image) -> Image` | The image with its alpha taken from `mask`, a gray image the same size, as Pillow's `putalpha` |
| `im.invert() -> Image` | Each color channel turned into 255 minus itself, with alpha kept, as Pillow's `ImageOps.invert` |
| `im.quantize(colors: usize = 256) -> Paletted` | The image in at most `colors` colors, for a small PNG |

`Filter` chooses how `resize` works out each new pixel from the old ones near it. These are Pillow's filters:

| `Filter` | Weights | For |
| --- | --- | --- |
| `Nearest` | the nearest pixel | pixel art, and speed |
| `Box` | the average of what each new pixel covers | shrinking by a whole factor |
| `Bilinear` | a triangle over 2 pixels | fast, and soft |
| `Hamming` | a Hamming-windowed sinc over 2 | sharper than bilinear, as fast |
| `Bicubic` | cubic over 4 | the default |
| `Lanczos` | a Lanczos-windowed sinc over 6 | the sharpest, for shrinking photos |

Resizing makes a pass across and a pass down, with weights in 22-bit fixed point, as Pillow does. It works only on the rows the second pass reads. An image with alpha is resized with each color weighted by its alpha, also as Pillow does, so clear pixels don't bleed their color into the edges.

### Drawing

These change the image in place.

| Item | Does |
| --- | --- |
| `im.paste(other: Image, x: i64, y: i64)` | Copies `other` onto the image with its top left at (x, y). The pixels, alpha included, are replaced, as Pillow's `paste` without a mask replaces them. `other` is converted to the image's kind first |
| `im.overlay(other: Image, x: i64, y: i64)` | Draws `other` over the image, blended by its alpha as Pillow's `alpha_composite` blends it. An image without alpha gets alpha |
| `im.fill_rect(x: i64, y: i64, width, height, color: Color)` | Fills a rectangle with `color` |
| `im.fill(color: Color)` | Fills the whole image |
| `im.flood_fill(x, y, color: Color, threshold: u8 = 0)` | Fills the area around (x, y) that's the color it is there, as Pillow's `ImageDraw.floodfill` does. It takes the pixels joined to (x, y) up, down, left or right, and no more than `threshold` from its color in any channel |

`paste`, `overlay` and `fill_rect` take positions that can be negative or past the edge. Whatever falls outside the image is cut off.

```calx
import std.image.{Color, Image, Kind}

fn main() {
    var card = Image.new(6, 4, Kind.Rgb, Color.rgb(200, 0, 0))
    card.overlay(Image.new(3, 3, Kind.Rgba, Color.rgba(0, 0, 255, 128)), 4, -1)
    println("{card} {card.get(5, 0)} {card.get(5, 3)}")    // out: Image(6x4 Rgba) #640080 #c80000

    var m = Image.new(5, 3, Kind.Gray, Color.gray(0))
    m.fill_rect(2, 0, 1, 3, Color.gray(255))
    m.flood_fill(0, 0, Color.gray(100))
    println("{m.get(1, 2)} {m.get(2, 1)} {m.get(4, 0)}")    // out: #646464 #ffffff #000000
}
```

## Palettes

`quantize` gives a `Paletted` image. Each pixel is a byte, `indexes`, into `palette: List<Color>` of up to 256 colors. Saved as a PNG, its indexes take 1, 2, 4 or 8 bits, as few as the palette allows, so a flat-colored image comes out far smaller.

- If the image has no more than `colors` colors, the palette keeps them exactly.
- Otherwise the colors are chosen by median cut and refined by four rounds of k-means, and each pixel takes the nearest.
- Alpha counts as a channel like the others, so clear and opaque pixels never share a color.

| Item | Gives |
| --- | --- |
| `p.width()`, `p.height()` | Its size |
| `p.indexes: List<u8>`, `p.palette: List<Color>` | Each pixel's index, row by row, and the colors |
| `p.to_image() -> Image` | Each pixel as its color: RGBA if any color isn't opaque, and RGB otherwise |
| `p.encode_png(level: u32 = 6) -> List<u8>` | A palette PNG, with a tRNS chunk for any alpha |
| `p.save(path: String) ! Error` | Writes it to a `.png` |

Pillow's own `quantize` picks colors another way, so the palette is not Pillow's.

## Icons

| Item | Gives |
| --- | --- |
| `encode_ico(images: List<Image>) -> List<u8>` | An ICO file holding each image as a PNG, smallest first, as Pillow writes one. Each side is at most 256 pixels |
| `decode_ico(bytes: List<u8>) -> List<Image> ! String` | Every image in an ICO file. PNG entries are read as `Image.decode` reads them; bitmap entries of 1, 4, 8, 24 or 32 bits are read with their transparency mask, as RGBA |

## Unlike Pillow

- **`thumbnail` filters in one step.** Pillow first shrinks by a whole factor with `reduce`, and then filters. The size is the same, but the pixels are a little sharper.
- **A PNG chunk with a broken CRC** fails only if the image needs that chunk; otherwise it's skipped, as libpng does. Pillow refuses any.
- **PNG files are written with libpng's filter choice.** Each row gets the filter that leaves the smallest sum of its bytes, taken as signed. On our photo the files came out within about 1% of Pillow's size.

## Speed

Times for a 1600 × 900 photo, the README's banner scaled up with Lanczos and saved at quality 90, against Pillow 12.3 with libjpeg-turbo, on an i9-13900K under Windows with a little other work running. Each program ran on the performance cores, 4 times in turns, and each row is the best of 60 runs (20 for writing a PNG).

| Work | Calx | Pillow |
| --- | --- | --- |
| Reading a baseline JPEG | 3.1 ms | 3.6 ms |
| Reading a progressive JPEG, the banner itself, 1376 × 768 | 5.8 ms | 6.9 ms |
| Writing a JPEG at quality 75 | 1.4 ms | 2.3 ms |
| Reading a PNG | 12.2 ms | 17.5 ms |
| Writing a PNG at level 6 | 58 ms | 95 ms |
| Lanczos to 800 × 450 | 8.5 ms | 10.7 ms |
| Bicubic to 400 × 225 | 4.8 ms | 5.3 ms |
| Bilinear up to 3200 × 1800 | 21.4 ms | 24.3 ms |
| Gaussian blur, radius 6 | 12.4 ms | 23.0 ms |
| Box blur, radius 2 | 4.3 ms | 9.5 ms |
| To gray | 0.61 ms | 1.02 ms |

Writing a JPEG does its pixel work with AVX2, as libjpeg-turbo's hand-written SIMD does: [std.image.jpeg](image-jpeg.md#how-its-built) has how, and its other settings' times.

## See also

- [std.image.png](image-png.md) and [std.image.jpeg](image-jpeg.md): the codecs on their own, for raw pixels without an `Image`
- [std.zlib](zlib.md): the compression inside PNG files
- [std.fs](fs.md): reading and writing the files
