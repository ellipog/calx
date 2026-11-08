// `calx fmt`: one fixed format for Calx source (CLX-89).
#pragma once

#include <string>
#include <vector>

namespace calx {

// Formats `text`, the source of the file `path`, into `out`. Returns false, with what's wrong in
// `error`, if the file has a syntax error, or if the formatted text wouldn't lex to the same
// tokens, or formatting it again would change it, which would be a bug in the formatter.
bool format_source(const std::string& path, const std::string& text, std::string& out, std::string& error);

// `calx fmt [--check] [--stdin] [PATH...]`. Returns the exit status.
int run_fmt(const std::vector<std::string>& args);

}  // namespace calx
