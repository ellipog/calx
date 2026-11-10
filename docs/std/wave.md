---
title: std.wave
description: WAV files read as Python's wave module reads them and written byte for byte as it writes them, floats too, with their samples as numbers instead of struct.unpack.
---

# `std.wave`

WAV files: a sound's channels, rate and samples. They're read as Python's `wave` module reads them, and written byte for byte as it writes them. The samples come out as numbers, so there's no `struct.unpack` to write.

```calx
import std.fs.{remove_dir_all, temp_dir}
import std.math.{PI, sin}
import std.wave.{Wave}

fn main() ! Error {
    // A second of a 440 Hz tone at half volume, made as 16-bit mono at 8000 frames a second.
    var tone: List<f32> = []
    for i in 0..8000 {
        tone.push(f32.nearest(0.5 * sin(2.0 * PI * 440.0 * f64(i) / 8000.0)))
    }
    let dir = try temp_dir()
    try Wave.from_f32(1, 8000, tone).save("{dir}/tone.wav")

    // Read back, and checked as a script checks a recording: how loud it gets.
    let w = try Wave.open("{dir}/tone.wav")
    var peak: i32 = 0
    var sum = 0.0
    for s in w.samples() {
        peak = peak.max(s.abs())
        sum += f64(s) * f64(s)
    }
    let rms = (sum / f64.nearest(w.frame_count())).sqrt()
    println("{w.channels} channel, {w.rate} Hz, {w.width * 8}-bit, {w.duration()} s")    // out: 1 channel, 8000 Hz, 16-bit, 1.0 s
    println("peak {peak}, rms {rms.rint()}")    // out: peak 16384, rms 11585.0
    try remove_dir_all(dir)
}
```

## A `Wave`

A `Wave` has public fields:

| Field | Holds | Python's `wave` |
| --- | --- | --- |
| `channels: u32` | Samples a frame, one for each channel: 1 for mono, 2 for stereo | `getnchannels()` |
| `rate: u32` | Frames a second | `getframerate()` |
| `width: u32` | Bytes a sample: 1 to 4 for integers, 4 or 8 for floats | `getsampwidth()` |
| `float: bool` | Whether the samples are IEEE floats | Not read |
| `frames: List<u8>` | The frames as the file holds them: each frame's samples in channel order, little-endian. 8-bit integers are unsigned, 128 being silence, and wider ones signed | `readframes(getnframes())` |

| Item | Gives |
| --- | --- |
| `Wave.open(path: String) -> Wave ! Error` | The sound in a WAV file |
| `Wave.decode(bytes: List<u8>) -> Wave ! String` | The sound in a WAV file's bytes, or why it can't be read |
| `Wave.new(channels, rate, width, frames: own List<u8>) -> Wave` | Integer samples of `width` bytes. Panics where Python's `wave` raises: no channels, a rate of 0, a width outside 1 to 4, or frames that aren't whole |
| `Wave.from_i16(channels, rate, samples: List<i16>) -> Wave` | 16-bit samples, each frame's in channel order |
| `Wave.from_f32(channels, rate, samples: List<f32>, width: u32 = 2) -> Wave` | Samples from -1 to 1 as integers of `width` bytes: clipped to that range, multiplied by the largest value the width holds (32767 for 2 bytes) and rounded to the nearest, as libsndfile writes floats to PCM |
| `w.frame_count() -> usize`, `w.duration() -> f64` | How many frames, Python's `getnframes()`, and how many seconds they play |
| `w.samples() -> List<i32>` | Each integer sample, each frame's in channel order: `struct.unpack("<{n}h", frames)` for 16 bits, and the same for 24 and 32. 8-bit samples are centered, from -128 to 127. Panics for floats |
| `w.samples_f32() -> List<f32>` | Each sample from -1 to 1: floats as they are, and integers divided by 2 to the power of one less than their bits (32768 for 16), as libsndfile and Python's soundfile read them |
| `w.encode() -> List<u8>`, `w.save(path: String) ! Error` | The WAV file's bytes, and writing them |

## What it reads

A WAV file is a RIFF file of chunks. `decode` walks them as Python's `wave` does:

- **Chunks** are read within the size that the RIFF header gives, and within the file. Each one starts after the one before and its pad byte, if its size is odd. Chunks it doesn't need, such as `LIST`, `fact` and `junk`, are skipped.
- **The format** is the `fmt ` chunk's: PCM, IEEE float, or WAVE_FORMAT_EXTENSIBLE with a PCM or float sub-format, as ffmpeg writes for more than 2 channels or more than 16 bits. Bits that aren't a whole number of bytes round up, so 12-bit samples take 2 bytes.
- **The frames** are the `data` chunk's. A file cut off partway, or a data size larger than the file, gives the frames that are there. A frame left partly written at the end is dropped.

It refuses a file for each of the reasons Python's `wave` refuses one, in the same order:
- it isn't RIFF, or the RIFF form isn't WAVE;
- the `data` chunk comes before `fmt `, or either is missing;
- the format or extended sub-format is unknown, the samples are 0 bits, or there are no channels;
- the `fmt ` chunk is cut short.

`encode` writes integer samples as Python's `wave` writes them: a 44-byte header, then the frames, with no pad byte after an odd number. It writes floats as WAVE_FORMAT_IEEE_FLOAT with the `fact` chunk that the format asks for.

It was checked against Python 3.14's `wave` on 23,000 generated files, good and broken. They had every width from 1 to 5 bytes, up to 6 channels, extensible formats, extra chunks of odd sizes, wrong RIFF and data sizes, and files cut off anywhere. Every file Python read gave the same frames, every file it refused was refused for the same reason, and every file written was the same bytes. Windows' 70 system sounds read the same too. For ffmpeg's files in every sample format, from 8-bit to 64-bit float, the frames and samples were ffmpeg's own raw output of the same sound.

## Unlike Python's `wave`

- **Floats are read and written.** Python's `wave` refuses them as an unknown format.
- **`frame_count` is the frames there are.** Python's `getnframes` is the count the `data` chunk's size gives, even when the file ends sooner.
- **Bad sizes don't raise.** A chunk whose size runs past the RIFF chunk ends the reading, and the file is refused for missing what it needs. Python raises `RuntimeError` there.

## Speed

A minute of 48 kHz stereo 16-bit sound, 11.5 MB, against Python 3.14's `wave` and `struct`, on the performance cores of an i9-13900K under Windows, run 3 times in turns, best of 15 runs (5 for the slowest Python):

| Work | Calx | Python |
| --- | --- | --- |
| Opening the file and reading its frames | 2.2 ms | 3.0 ms |
| The samples as integers, `struct.unpack` in Python | 2.8 ms | 78.6 ms |
| The samples from -1 to 1, a list comprehension dividing by 32768 in Python | 5.7 ms | 228 ms |
| Opening it and working out the peak and RMS, as a check of a recording does | 5.4 ms | 356 ms |
| Writing it from 16-bit samples, with `struct.pack` in Python | 2.8 ms | 101 ms |

## See also

- [std.fs](fs.md): the files
- [std.bytes](bytes.md): reading numbers out of bytes, for other formats
