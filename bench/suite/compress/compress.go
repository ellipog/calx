// Compression: 17 MB of access log compressed and decompressed with gzip at level 6 and zstd at
// level 3, and its first 4 MiB with xz at preset 6, each checked against the original. The sizes
// differ by library, so they're printed as `info` lines, which the runner keeps but doesn't
// compare. Calx uses std.zlib, std.zstd and std.xz; Rust flate2, the zstd crate and xz2; Go
// compress/gzip, klauspost/compress and ulikunitz/xz; Python gzip, compression.zstd and lzma.
// ulikunitz/xz has no presets; its defaults are closest to preset 6's 8 MiB dictionary.
package main

import (
	"bytes"
	"compress/gzip"
	"fmt"
	"io"
	"os"

	"github.com/klauspost/compress/zstd"
	"github.com/ulikunitz/xz"
)

func check(err error) {
	if err != nil {
		panic(err)
	}
}

func main() {
	data, err := os.ReadFile(os.Args[1])
	check(err)

	var gz bytes.Buffer
	w, err := gzip.NewWriterLevel(&gz, 6)
	check(err)
	_, err = w.Write(data)
	check(err)
	check(w.Close())
	r, err := gzip.NewReader(bytes.NewReader(gz.Bytes()))
	check(err)
	gzBack, err := io.ReadAll(r)
	check(err)

	enc, err := zstd.NewWriter(nil, zstd.WithEncoderLevel(zstd.SpeedDefault))
	check(err)
	zs := enc.EncodeAll(data, nil)
	dec, err := zstd.NewReader(nil)
	check(err)
	zsBack, err := dec.DecodeAll(zs, nil)
	check(err)

	part := data[:min(len(data), 4194304)]
	var x bytes.Buffer
	xw, err := xz.NewWriter(&x)
	check(err)
	_, err = xw.Write(part)
	check(err)
	check(xw.Close())
	xr, err := xz.NewReader(bytes.NewReader(x.Bytes()))
	check(err)
	xBack, err := io.ReadAll(xr)
	check(err)

	fmt.Printf("gzip %t\nzstd %t\nxz %t\n", bytes.Equal(gzBack, data), bytes.Equal(zsBack, data), bytes.Equal(xBack, part))
	fmt.Printf("info gzip %d\ninfo zstd %d\ninfo xz %d\n", gz.Len(), len(zs), x.Len())
}
