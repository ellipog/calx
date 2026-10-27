// Source files, and the diagnostics that point into them.
#pragma once

#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common.h"

namespace calx {

struct SourceFile {
    std::string path;       // as given on the command line or found by the module loader
    std::string text;       // the whole file, with a NUL after it
    std::vector<std::uint32_t> line_starts;

    SourceFile(std::string path, std::string text);

    // 1-based line and column (in bytes) of an offset.
    std::pair<std::uint32_t, std::uint32_t> position(std::uint32_t offset) const;
    std::string_view line_text(std::uint32_t line) const;
};

class Sources {
public:
    // Reads a file. Returns its id, or 0 if it can't be read.
    std::uint32_t load(const std::string& path);
    std::uint32_t add(std::string path, std::string text);
    const SourceFile& file(std::uint32_t id) const { return *files_[id - 1]; }

private:
    std::vector<std::unique_ptr<SourceFile>> files_;
};

enum class Severity { error, warning, note };

struct Diagnostic {
    Severity severity;
    Span span;
    std::string message;
    std::vector<std::pair<Span, std::string>> notes;
};

// Collects diagnostics and prints them in the form `path:line:col: error: message`, followed by
// the source line and a caret under the span, the way clang does.
class Diagnostics {
public:
    explicit Diagnostics(const Sources& sources) : sources_(sources) {}

    void error(Span span, std::string message) { add(Severity::error, span, std::move(message)); }
    void warning(Span span, std::string message) { add(Severity::warning, span, std::move(message)); }
    void note(Span span, std::string message);  // attached to the last diagnostic

    bool has_errors() const { return errors_ > 0; }
    std::size_t error_count() const { return errors_; }
    const std::vector<Diagnostic>& all() const { return list_; }

    // Prints everything collected so far, in the order it was reported.
    void print(std::FILE* out, bool color) const;
    std::string format(const Diagnostic& d, bool color) const;

private:
    void add(Severity severity, Span span, std::string message);
    std::string excerpt(Span span, bool color) const;

    const Sources& sources_;
    std::vector<Diagnostic> list_;
    std::size_t errors_ = 0;
};

}  // namespace calx
