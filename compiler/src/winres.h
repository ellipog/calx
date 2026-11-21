// The resources every Windows program and library Calx builds carries (CLX-414): a version
// resource, which Explorer's Properties shows, and a manifest, as Visual Studio's and Rust's
// builds have. Without them, a console program is one Windows Defender's machine-learning models
// weigh as suspect, and some builds were quarantined as Trojan:Win32/Bearfoos.A!ml.
#pragma once

#include <string>

namespace calx {

// What a program says about itself: its package's, from calx.toml, or else its file's name.
struct ProgramInfo {
    std::string name;         // the package's name, or the main file's without `.calx`
    std::string version;      // three numbers, with `-pre` after them if wanted; "" is 0.0.0
    std::string description;  // what Windows shows as the file's description; "" is the name
    std::string company;      // "" is the name
    std::string copyright;    // "" leaves it out
};

// The bytes of a `.res` file, as llvm-rc and Visual Studio's rc write one, that holds the version
// resource and the manifest for a program, or for a library with `library`. The same info gives
// the same bytes: nothing in them is a time. lld links a `.res` as it would an object.
std::string windows_resources(const ProgramInfo& info, bool library);

}  // namespace calx
