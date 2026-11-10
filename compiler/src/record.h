// What a build read, kept in the cache, so a later build that finds all of it as it was goes
// straight to the program it made, without reading, checking or emitting anything (CLX-242).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace calx {

struct BuildRecord {
    std::string exe;                   // the program or library in the cache
    std::vector<std::string> tests;    // the program's tests, for `calx test`
    std::vector<std::string> files;    // each file the build read
    std::vector<std::string> missing;  // each path it looked for a file at and found none
    std::vector<std::string> folders;  // each folder an import listed
};

// A file's or folder's size and modified time, as the system gives them, or `ok` false if
// nothing's there.
struct Stamp {
    std::uint64_t size = 0;
    std::int64_t time = 0;
    bool ok = false;
};
Stamp stamp_of(const std::string& path);

// The time now, in the units a Stamp's time is in.
std::int64_t stamp_now();

// `path` made absolute, as UTF-8.
std::string absolute_path(const std::string& path);

// Where the record is kept for a build whose every input but the files it reads is in `key`.
std::string record_path(const std::string& cache, const std::vector<std::string_view>& key);

// Reads the record at `path` into `r` if there is one, everything it names is as it was then,
// and the program it names is still in the cache, which it then marks as used.
bool load_record(const std::string& path, BuildRecord& r);

// Saves `r` at `path`, each file and folder with its size and time now. Not if any of them
// changed after `start`, when the build began, or in the two seconds before, since a file changed
// then could have been read as it was before and stamped as it is after, and some file systems
// only keep times to two seconds.
void save_record(const std::string& path, const BuildRecord& r, std::int64_t start);

}  // namespace calx
