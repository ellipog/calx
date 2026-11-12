// PNG: a 2,048 by 2,048 RGB image, 12 MB of pixels, encoded as a PNG with zlib at its default
// level and each row's filter chosen as libpng chooses it, then decoded again, and the pixels
// checked against the original. The files' sizes differ by library, so they're printed as an
// `info` line. Calx uses std.image.png, Rust the png crate, and Go image/png. Python has no PNG in
// its standard library. The png crate's encoder starts at its fast setting with one filter for
// every row, which writes a file nearly twice the size, so this sets its default compression and
// its adaptive filter, the work Calx's and Go's encoders do.
fn main() {
    let mut args = std::env::args().skip(1);
    let pixels = std::fs::read(args.next().expect("the pixels' path")).unwrap();
    let width: u32 = args.next().unwrap().parse().unwrap();
    let height: u32 = args.next().unwrap().parse().unwrap();

    let mut file = Vec::new();
    {
        let mut enc = png::Encoder::new(&mut file, width, height);
        enc.set_color(png::ColorType::Rgb);
        enc.set_depth(png::BitDepth::Eight);
        enc.set_compression(png::Compression::Default);
        enc.set_adaptive_filter(png::AdaptiveFilterType::Adaptive);
        let mut w = enc.write_header().unwrap();
        w.write_image_data(&pixels).unwrap();
    }

    let mut dec = png::Decoder::new(std::io::Cursor::new(&file)).read_info().unwrap();
    let mut back = vec![0; dec.output_buffer_size()];
    let info = dec.next_frame(&mut back).unwrap();
    back.truncate(info.buffer_size());
    println!("png {}x{} {}", info.width, info.height, back == pixels);
    println!("info png {}", file.len());
}
