#include "winres.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace calx {

namespace {

// The manifest a program has: it runs with the rights of whoever starts it, never asking to be
// elevated, which Windows otherwise guesses at for a program whose name holds "setup", "install"
// or "update"; and it's written for Windows 10 and 11, so the system gives it their behavior
// rather than Windows 8's, and says which version it is. A library's has neither, which only
// a program's are read for, so it says just that it's one.
constexpr const char* kProgramManifest =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\n"
    "  <compatibility xmlns=\"urn:schemas-microsoft-com:compatibility.v1\">\n"
    "    <application>\n"
    "      <supportedOS Id=\"{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}\"/>\n"
    "    </application>\n"
    "  </compatibility>\n"
    "  <trustInfo xmlns=\"urn:schemas-microsoft-com:asm.v3\">\n"
    "    <security>\n"
    "      <requestedPrivileges>\n"
    "        <requestedExecutionLevel level=\"asInvoker\" uiAccess=\"false\"/>\n"
    "      </requestedPrivileges>\n"
    "    </security>\n"
    "  </trustInfo>\n"
    "</assembly>\n";
constexpr const char* kLibraryManifest =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\n"
    "</assembly>\n";

constexpr std::uint16_t kRtVersion = 16;
constexpr std::uint16_t kRtManifest = 24;
// US English, which Visual Studio's linker gives the manifest it embeds, and its rc the version
// resource of a new project.
constexpr std::uint16_t kLanguage = 0x0409;
constexpr std::uint16_t kCodePage = 1200;  // UTF-16

class Bytes {
public:
    void u16(std::uint32_t v) {
        out_ += static_cast<char>(v & 0xFF);
        out_ += static_cast<char>((v >> 8) & 0xFF);
    }
    void u32(std::uint32_t v) {
        u16(v & 0xFFFF);
        u16(v >> 16);
    }
    void raw(const std::string& bytes) { out_ += bytes; }
    // UTF-16, ending in a 0.
    void text(const std::u16string& s) {
        for (char16_t c : s) u16(c);
        u16(0);
    }
    void align() {
        while (out_.size() % 4) out_ += '\0';
    }
    std::size_t size() const { return out_.size(); }
    void put_u16(std::size_t at, std::size_t v) {
        out_[at] = static_cast<char>(v & 0xFF);
        out_[at + 1] = static_cast<char>((v >> 8) & 0xFF);
    }
    std::string take() { return std::move(out_); }

private:
    std::string out_;
};

// UTF-8 as UTF-16, with U+FFFD for each byte that isn't part of a character.
std::u16string utf16(const std::string& s) {
    std::u16string out;
    std::size_t i = 0;
    while (i < s.size()) {
        unsigned char b = static_cast<unsigned char>(s[i]);
        std::uint32_t c = 0xFFFD;
        std::size_t n = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 0;
        bool good = n > 0 && i + n <= s.size();
        if (good) {
            c = n == 1 ? b : n == 2 ? (b & 0x1F) : n == 3 ? (b & 0x0F) : (b & 0x07);
            for (std::size_t k = 1; k < n; k++) {
                unsigned char t = static_cast<unsigned char>(s[i + k]);
                if ((t >> 6) != 2) good = false;
                c = (c << 6) | (t & 0x3F);
            }
            static const std::uint32_t kLeast[] = {0, 0, 0x80, 0x800, 0x10000};
            if (!good || c < kLeast[n] || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) good = false;
        }
        if (!good) {
            out += u'�';
            i++;
            continue;
        }
        if (c >= 0x10000) {
            c -= 0x10000;
            out += static_cast<char16_t>(0xD800 + (c >> 10));
            out += static_cast<char16_t>(0xDC00 + (c & 0x3FF));
        } else {
            out += static_cast<char16_t>(c);
        }
        i += n;
    }
    return out;
}

// A version's first three numbers, "1.2.3-beta.1" giving 1, 2 and 3, each at most 65535.
void version_numbers(const std::string& v, std::uint32_t out[3]) {
    std::size_t at = 0;
    for (int k = 0; k < 3; k++) {
        std::uint32_t n = 0;
        while (at < v.size() && v[at] >= '0' && v[at] <= '9') {
            n = n * 10 + static_cast<std::uint32_t>(v[at] - '0');
            if (n > 0xFFFF) n = 0xFFFF;
            at++;
        }
        out[k] = n;
        if (at < v.size() && v[at] == '.') at++;
    }
}

// One node of the version resource's tree, as rc writes it: its length, its value's length, its
// type (1 for text, 0 for binary), its key, then its value and its children, each starting on a
// 4-byte boundary. The length covers everything but the padding after the last child.
struct Node {
    std::u16string key;
    std::string value;          // the value's bytes
    std::size_t value_length;   // in bytes for binary, in characters for text
    bool text;
    std::vector<Node> children;
};

void write_node(Bytes& b, const Node& n) {
    std::size_t start = b.size();
    b.u16(0);  // its length, filled in below
    b.u16(static_cast<std::uint32_t>(n.value_length));
    b.u16(n.text ? 1 : 0);
    b.text(n.key);
    b.align();
    b.raw(n.value);
    for (const Node& c : n.children) {
        b.align();
        write_node(b, c);
    }
    b.put_u16(start, b.size() - start);
}

Node string_node(const char* key, const std::string& value) {
    std::u16string v = utf16(value);
    Bytes bytes;
    bytes.text(v);
    return Node{utf16(key), bytes.take(), v.size() + 1, true, {}};
}

// One resource in the `.res` file: its header, with the type and name as numbers, then its data.
void write_resource(Bytes& b, std::uint16_t type, std::uint16_t name, const std::string& data) {
    b.u32(static_cast<std::uint32_t>(data.size()));
    b.u32(32);  // the header's size
    b.u16(0xFFFF);
    b.u16(type);
    b.u16(0xFFFF);
    b.u16(name);
    b.u32(0);       // data version
    b.u16(0x0030);  // moveable and pure, as rc marks them
    b.u16(kLanguage);
    b.u32(0);  // version
    b.u32(0);  // characteristics
    b.raw(data);
    b.align();
}

}  // namespace

std::string windows_resources(const ProgramInfo& given, bool library) {
    ProgramInfo info = given;
    if (info.name.empty()) info.name = library ? "library" : "program";
    std::string version = info.version.empty() ? "0.0.0" : info.version;
    std::uint32_t numbers[3];
    version_numbers(version, numbers);

    // VS_FIXEDFILEINFO: the version as numbers, twice, for the file and the product.
    Bytes fixed;
    fixed.u32(0xFEEF04BD);  // its signature
    fixed.u32(0x00010000);  // its layout's version
    for (int twice = 0; twice < 2; twice++) {
        fixed.u32(numbers[0] << 16 | numbers[1]);
        fixed.u32(numbers[2] << 16);
    }
    fixed.u32(0x3F);         // which flags it can have
    fixed.u32(0);            // and has: not a debug build, a prerelease or patched
    fixed.u32(0x00040004);   // VOS_NT_WINDOWS32
    fixed.u32(library ? 2 : 1);  // VFT_DLL or VFT_APP
    fixed.u32(0);            // no subtype
    fixed.u32(0);            // no date, so a build's bytes are the same each time
    fixed.u32(0);
    std::string fixed_bytes = fixed.take();

    std::vector<Node> strings;
    strings.push_back(string_node("CompanyName", info.company.empty() ? info.name : info.company));
    strings.push_back(string_node("FileDescription", info.description.empty() ? info.name : info.description));
    strings.push_back(string_node("FileVersion", version));
    strings.push_back(string_node("InternalName", info.name));
    if (!info.copyright.empty()) strings.push_back(string_node("LegalCopyright", info.copyright));
    strings.push_back(string_node("OriginalFilename", info.name + (library ? ".dll" : ".exe")));
    strings.push_back(string_node("ProductName", info.name));
    strings.push_back(string_node("ProductVersion", version));

    Bytes translation;
    translation.u16(kLanguage);
    translation.u16(kCodePage);
    // The table's key is its language and code page in hex: 040904b0.
    Node table{u"040904b0", "", 0, true, std::move(strings)};
    Node string_info{u"StringFileInfo", "", 0, true, {std::move(table)}};
    Node var{u"Translation", translation.take(), 4, false, {}};
    Node var_info{u"VarFileInfo", "", 0, true, {std::move(var)}};
    Node root{u"VS_VERSION_INFO", fixed_bytes, fixed_bytes.size(), false, {}};
    root.children.push_back(std::move(string_info));
    root.children.push_back(std::move(var_info));
    Bytes version_info;
    write_node(version_info, root);

    Bytes res;
    // A `.res` file starts with an empty resource, which says what it is.
    res.u32(0);
    res.u32(32);
    res.u16(0xFFFF);
    res.u16(0);
    res.u16(0xFFFF);
    res.u16(0);
    res.u32(0);
    res.u16(0);
    res.u16(0);
    res.u32(0);
    res.u32(0);
    write_resource(res, kRtVersion, 1, version_info.take());
    // A program's manifest is resource 1, which Windows reads as it starts the program, and a
    // library's resource 2, which it reads as it loads the library.
    write_resource(res, kRtManifest, library ? 2 : 1, library ? kLibraryManifest : kProgramManifest);
    return res.take();
}

}  // namespace calx
