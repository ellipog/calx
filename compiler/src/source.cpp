#include "source.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace calx {

SourceFile::SourceFile(std::string p, std::string t) : path(std::move(p)), text(std::move(t)) {
    line_starts.push_back(0);
    for (std::uint32_t i = 0; i < text.size(); i++) {
        if (text[i] == '\n') line_starts.push_back(i + 1);
    }
}

std::pair<std::uint32_t, std::uint32_t> SourceFile::position(std::uint32_t offset) const {
    auto it = std::upper_bound(line_starts.begin(), line_starts.end(), offset);
    auto line = static_cast<std::uint32_t>(it - line_starts.begin());
    return {line, offset - line_starts[line - 1] + 1};
}

std::pair<std::uint32_t, std::uint32_t> SourceFile::editor_position(std::uint32_t offset) const {
    auto [line, col] = position(offset);
    std::uint32_t units = 0;
    for (std::uint32_t i = line_starts[line - 1]; i < offset && i < text.size(); i++) {
        auto b = static_cast<unsigned char>(text[i]);
        if ((b & 0xC0) == 0x80) continue;  // a continuation byte
        units += b >= 0xF0 ? 2 : 1;        // outside the BMP, UTF-16 needs a surrogate pair
    }
    (void)col;
    return {line - 1, units};
}

std::string_view SourceFile::line_text(std::uint32_t line) const {
    std::uint32_t begin = line_starts[line - 1];
    std::uint32_t end = line < line_starts.size() ? line_starts[line] : static_cast<std::uint32_t>(text.size());
    while (end > begin && (text[end - 1] == '\n' || text[end - 1] == '\r')) end--;
    return std::string_view(text).substr(begin, end - begin);
}

std::uint32_t Sources::load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return 0;
    std::ostringstream buf;
    buf << in.rdbuf();
    return add(path, buf.str());
}

std::uint32_t Sources::add(std::string path, std::string text) {
    files_.push_back(std::make_unique<SourceFile>(std::move(path), std::move(text)));
    return static_cast<std::uint32_t>(files_.size());
}

std::uint32_t Sources::add_generated(std::string path, std::string text, Span at, std::string why) {
    std::uint32_t id = add(std::move(path), std::move(text));
    files_.back()->generated_at = at;
    files_.back()->generated_by = std::move(why);
    return id;
}

void Diagnostics::add(Severity severity, Span span, std::string message) {
    if (severity == Severity::error) errors_++;
    list_.push_back(Diagnostic{severity, span, std::move(message), {}});
    // Code an attribute generated says where it came from, back to code someone wrote.
    for (std::uint32_t file = span.file; file != 0;) {
        const SourceFile& f = sources_.file(file);
        if (f.generated_at.file == 0) break;
        list_.back().notes.emplace_back(f.generated_at, f.generated_by);
        file = f.generated_at.file;
    }
}

void Diagnostics::note(Span span, std::string message) {
    if (!list_.empty()) list_.back().notes.emplace_back(span, std::move(message));
}

std::string Diagnostics::excerpt(Span span, bool color) const {
    if (span.file == 0) return {};
    const SourceFile& f = sources_.file(span.file);
    auto [line, col] = f.position(span.begin);
    std::string_view text = f.line_text(line);
    std::string out = "  ";
    out += text;
    out += "\n  ";
    for (std::uint32_t i = 1; i < col && i <= text.size(); i++) out += text[i - 1] == '\t' ? '\t' : ' ';
    // Underline to the end of the span, but no further than the end of its first line.
    std::uint32_t width = 1;
    if (span.end > span.begin) {
        std::uint32_t line_end = f.line_starts[line - 1] + static_cast<std::uint32_t>(text.size());
        width = std::max<std::uint32_t>(1, std::min(span.end, line_end) - span.begin);
    }
    if (color) out += "\x1b[1;32m";
    out += '^';
    out.append(width - 1, '~');
    if (color) out += "\x1b[0m";
    out += '\n';
    return out;
}

static const char* label(Severity s) {
    switch (s) {
        case Severity::error: return "error";
        case Severity::warning: return "warning";
        case Severity::note: return "note";
    }
    return "error";
}

std::string Diagnostics::format(const Diagnostic& d, bool color) const {
    auto where = [&](Span span) {
        if (span.file == 0) return std::string("calx: ");
        const SourceFile& f = sources_.file(span.file);
        auto [line, col] = f.position(span.begin);
        return f.path + ":" + std::to_string(line) + ":" + std::to_string(col) + ": ";
    };
    auto head = [&](Severity s) {
        std::string h;
        if (color) h += s == Severity::error ? "\x1b[1;31m" : s == Severity::warning ? "\x1b[1;35m" : "\x1b[1;36m";
        h += label(s);
        h += ':';
        if (color) h += "\x1b[0m";
        return h;
    };
    std::string out = where(d.span) + head(d.severity) + " " + d.message + "\n" + excerpt(d.span, color);
    for (const auto& [span, msg] : d.notes) {
        out += where(span) + head(Severity::note) + " " + msg + "\n" + excerpt(span, color);
    }
    return out;
}

// In source order, whichever stage found them, so the lexer's errors don't jump ahead.
std::vector<const Diagnostic*> Diagnostics::sorted() const {
    std::vector<const Diagnostic*> out;
    for (const auto& d : list_) out.push_back(&d);
    std::stable_sort(out.begin(), out.end(), [](const Diagnostic* a, const Diagnostic* b) {
        if (a->span.file != b->span.file) return a->span.file < b->span.file;
        return a->span.begin < b->span.begin;
    });
    return out;
}

void Diagnostics::print(std::FILE* out, bool color) const {
    for (const Diagnostic* d : sorted()) std::fputs(format(*d, color).c_str(), out);
}

static std::string json_string(std::string_view s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out + "\"";
}

// `"file": ..., "range": ...` for a span. An empty span covers the character it's at, so an
// editor has something to underline.
std::string Diagnostics::json_location(Span span) const {
    if (span.file == 0) {
        return "\"file\":null,\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":0,\"character\":0}}";
    }
    const SourceFile& f = sources_.file(span.file);
    std::uint32_t end = span.end;
    if (end <= span.begin) {
        end = span.begin;
        if (end < f.text.size() && f.text[end] != '\n' && f.text[end] != '\r') {
            end++;
            while (end < f.text.size() && (static_cast<unsigned char>(f.text[end]) & 0xC0) == 0x80) end++;
        }
    }
    auto [l0, c0] = f.editor_position(span.begin);
    auto [l1, c1] = f.editor_position(end);
    auto pos = [](std::uint32_t l, std::uint32_t c) {
        return "{\"line\":" + std::to_string(l) + ",\"character\":" + std::to_string(c) + "}";
    };
    return "\"file\":" + json_string(f.path) + ",\"range\":{\"start\":" + pos(l0, c0) + ",\"end\":" + pos(l1, c1) + "}";
}

void Diagnostics::print_json(std::FILE* out) const {
    for (const Diagnostic* d : sorted()) {
        // An editor shows a diagnostic in a file someone wrote, so one in code an attribute
        // generated is put on the attribute, with the place in the generated code in its message.
        Span at = d->span;
        std::string message = d->message;
        if (at.file != 0 && sources_.file(at.file).generated_at.file != 0) {
            const SourceFile& g = sources_.file(at.file);
            auto [line, col] = g.position(at.begin);
            message = g.generated_by + ", at " + g.path + ":" + std::to_string(line) + ":" + std::to_string(col) + ": " +
                      message;
            while (sources_.file(at.file).generated_at.file != 0) at = sources_.file(at.file).generated_at;
        }
        std::string s = "{\"severity\":\"" + std::string(label(d->severity)) + "\"," + json_location(at) +
                        ",\"message\":" + json_string(message) + ",\"notes\":[";
        for (std::size_t i = 0; i < d->notes.size(); i++) {
            if (i) s += ',';
            s += "{" + json_location(d->notes[i].first) + ",\"message\":" + json_string(d->notes[i].second) + "}";
        }
        s += "]}\n";
        std::fputs(s.c_str(), out);
    }
}

}  // namespace calx
