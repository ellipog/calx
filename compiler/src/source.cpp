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

void Diagnostics::add(Severity severity, Span span, std::string message) {
    if (severity == Severity::error) errors_++;
    list_.push_back(Diagnostic{severity, span, std::move(message), {}});
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

void Diagnostics::print(std::FILE* out, bool color) const {
    // In source order, whichever stage found them, so the lexer's errors don't jump ahead.
    std::vector<const Diagnostic*> sorted;
    for (const auto& d : list_) sorted.push_back(&d);
    std::stable_sort(sorted.begin(), sorted.end(), [](const Diagnostic* a, const Diagnostic* b) {
        if (a->span.file != b->span.file) return a->span.file < b->span.file;
        return a->span.begin < b->span.begin;
    });
    for (const Diagnostic* d : sorted) std::fputs(format(*d, color).c_str(), out);
}

}  // namespace calx
