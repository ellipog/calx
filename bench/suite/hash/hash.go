// Hashing: 17 MB of access log fed 16 times, 270 MB in all, through SHA-256 and through CRC-32,
// each carried on from one pass to the next. Calx uses std.hash and std.zlib's crc32; Rust the sha2
// and crc32fast crates; Go crypto/sha256 and hash/crc32; Python hashlib and zlib.crc32.
package main

import (
	"crypto/sha256"
	"fmt"
	"hash/crc32"
	"os"
)

func main() {
	data, err := os.ReadFile(os.Args[1])
	if err != nil {
		panic(err)
	}
	d := sha256.New()
	for i := 0; i < 16; i++ {
		d.Write(data)
	}
	var crc uint32
	for i := 0; i < 16; i++ {
		crc = crc32.Update(crc, crc32.IEEETable, data)
	}
	fmt.Printf("sha256 %x\ncrc32 %08x\n", d.Sum(nil), crc)
}
