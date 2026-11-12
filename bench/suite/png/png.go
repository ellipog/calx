// PNG: a 2,048 by 2,048 RGB image, 12 MB of pixels, encoded as a PNG at the library's default
// setting and decoded again, and the pixels checked against the original. The files' sizes differ
// by library, so they're printed as an `info` line. Calx uses std.image.png, Rust the png crate,
// and Go image/png; Python has no PNG in its standard library. Go has no RGB image type, so the
// pixels go into an opaque NRGBA image, which image/png writes as RGB, and come back as one.
package main

import (
	"bytes"
	"fmt"
	"image"
	"image/png"
	"os"
	"strconv"
)

func main() {
	pixels, err := os.ReadFile(os.Args[1])
	if err != nil {
		panic(err)
	}
	width, _ := strconv.Atoi(os.Args[2])
	height, _ := strconv.Atoi(os.Args[3])
	img := image.NewNRGBA(image.Rect(0, 0, width, height))
	for i, j := 0, 0; i < len(pixels); i, j = i+3, j+4 {
		img.Pix[j], img.Pix[j+1], img.Pix[j+2], img.Pix[j+3] = pixels[i], pixels[i+1], pixels[i+2], 255
	}
	var file bytes.Buffer
	if err := png.Encode(&file, img); err != nil {
		panic(err)
	}
	decoded, err := png.Decode(bytes.NewReader(file.Bytes()))
	if err != nil {
		panic(err)
	}
	back := make([]byte, 0, len(pixels))
	switch d := decoded.(type) {
	case *image.RGBA:
		for j := 0; j < len(d.Pix); j += 4 {
			back = append(back, d.Pix[j], d.Pix[j+1], d.Pix[j+2])
		}
	case *image.NRGBA:
		for j := 0; j < len(d.Pix); j += 4 {
			back = append(back, d.Pix[j], d.Pix[j+1], d.Pix[j+2])
		}
	}
	b := decoded.Bounds()
	fmt.Printf("png %dx%d %t\n", b.Dx(), b.Dy(), bytes.Equal(back, pixels))
	fmt.Printf("info png %d\n", file.Len())
}
