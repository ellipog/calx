#pragma once

#include <string>

namespace calx {

// The system and the processor a program is built for (D612): Windows, Linux or macOS, on x86-64
// or Arm64, and iOS on Arm64, for an iPhone or the simulator on a Mac (CLX-408). Calx builds for
// the machine it runs on unless `--target` names another it can build for from there (CLX-409),
// so what the toolchain passes for a build comes from here rather than from the compiler's own
// #ifdefs, which are about the system the compiler itself calls (CLX-273).
enum class Os { windows, linux, macos, ios };
enum class Arch { x86_64, aarch64 };

struct Target {
    Os os;
    Arch arch;
    // On iOS, the simulator, which runs on a Mac's processor with an SDK of its own, rather than
    // an iPhone or iPad.
    bool simulator = false;
};

constexpr bool operator==(const Target& a, const Target& b) {
    return a.os == b.os && a.arch == b.arch && a.simulator == b.simulator;
}
constexpr bool operator!=(const Target& a, const Target& b) { return !(a == b); }

constexpr Target host_target() {
#if defined(_WIN32)
    constexpr Os os = Os::windows;
#elif defined(__APPLE__)
    constexpr Os os = Os::macos;
#elif defined(__linux__)
    constexpr Os os = Os::linux;
#else
#error "Calx builds for Windows, Linux and macOS"
#endif
#if defined(__x86_64__) || defined(_M_X64)
    constexpr Arch arch = Arch::x86_64;
#elif defined(__aarch64__) || defined(_M_ARM64)
    constexpr Arch arch = Arch::aarch64;
#else
#error "Calx builds for x86-64 and Arm64"
#endif
    return {os, arch, false};
}

// The machine this calx runs on, and so what it builds for without `--target`.
inline constexpr Target kHost = host_target();

// Whether the target is one of Apple's, whose clang takes `-mcpu=` for a processor level and whose
// objects are Mach-O.
constexpr bool apple(Target t) { return t.os == Os::macos || t.os == Os::ios; }

// `windows`, `linux`, `macos` or `ios`, as Rust's `std::env::consts::OS` names them, and
// std.env.os().
constexpr const char* os_name(Os os) {
    return os == Os::windows ? "windows" : os == Os::macos ? "macos" : os == Os::ios ? "ios" : "linux";
}

// `x86_64` or `aarch64`, as Rust's `std::env::consts::ARCH` names them, and std.env.arch().
constexpr const char* arch_name(Arch arch) { return arch == Arch::x86_64 ? "x86_64" : "aarch64"; }

// `x86_64-windows` or `aarch64-ios-sim`, as CALX_SPEC.md names the targets.
inline std::string target_name(Target t) {
    return std::string(arch_name(t.arch)) + "-" + os_name(t.os) + (t.simulator ? "-sim" : "");
}

// `Windows, x64` or `iOS Simulator, Arm64`, for people: `calx version` and `calx doctor`.
inline std::string target_label(Target t) {
    std::string os = t.os == Os::windows ? "Windows" : t.os == Os::macos ? "macOS" : t.os == Os::ios ? "iOS" : "Linux";
    if (t.simulator) os += " Simulator";
    return os + (t.arch == Arch::x86_64 ? ", x64" : ", Arm64");
}

// The oldest iOS a build for iOS runs on, which every iPhone from the XS and XR on runs, with the
// A12 or later (CLX-408). It's the first with os_sync_wait_on_address, the public wait on an
// address the runtime's threads use, where an older one would take a private function.
inline constexpr const char* kIosMin = "18.0";

// clang's `--target` for a build that isn't for the host, "" for one that is: clang builds for
// the machine it runs on by default, and on Windows the toolchain decides between mingw's and
// Visual Studio's (CLX-409).
inline std::string clang_target_flag(Target t) {
    if (t == kHost) return "";
    if (t.os == Os::ios) return std::string("--target=arm64-apple-ios") + kIosMin + (t.simulator ? "-simulator" : "");
    return "";
}

// The targets `--target` takes, one per line, each with what it is: the Calx name first, then
// the triples Rust and LLVM give the same target, which it takes too (CLX-409).
struct TargetName {
    const char* name;
    Target target;
};
inline const TargetName* target_names() {
    static const TargetName names[] = {
        {"x86_64-windows", {Os::windows, Arch::x86_64}},       {"x86_64-pc-windows-msvc", {Os::windows, Arch::x86_64}},
        {"x86_64-pc-windows-gnu", {Os::windows, Arch::x86_64}},  {"aarch64-windows", {Os::windows, Arch::aarch64}},
        {"aarch64-pc-windows-msvc", {Os::windows, Arch::aarch64}}, {"aarch64-pc-windows-gnullvm", {Os::windows, Arch::aarch64}},
        {"x86_64-linux", {Os::linux, Arch::x86_64}},           {"x86_64-unknown-linux-gnu", {Os::linux, Arch::x86_64}},
        {"aarch64-linux", {Os::linux, Arch::aarch64}},         {"aarch64-unknown-linux-gnu", {Os::linux, Arch::aarch64}},
        {"aarch64-macos", {Os::macos, Arch::aarch64}},         {"aarch64-apple-darwin", {Os::macos, Arch::aarch64}},
        {"arm64-apple-macos", {Os::macos, Arch::aarch64}},     {"aarch64-ios", {Os::ios, Arch::aarch64}},
        {"aarch64-apple-ios", {Os::ios, Arch::aarch64}},       {"arm64-apple-ios", {Os::ios, Arch::aarch64}},
        {"aarch64-ios-sim", {Os::ios, Arch::aarch64, true}},   {"aarch64-apple-ios-sim", {Os::ios, Arch::aarch64, true}},
        {"arm64-apple-ios-simulator", {Os::ios, Arch::aarch64, true}},
        {nullptr, {Os::linux, Arch::x86_64}},
    };
    return names;
}

// Whether a calx on `host` builds for `t`, and if not, why not (CLX-409): every host builds for
// itself, and a Mac builds for iOS too, with Xcode's SDKs. Building for another system or
// processor than the host's needs that system's C library and SDK, which a Mac has for iOS and
// nothing has yet for the rest.
inline bool can_build(Target host, Target t, std::string& why) {
    if (t == host) return true;
    if (t.os == Os::ios && host.os == Os::macos) return true;
    why = "this calx runs on " + target_name(host) + ", and builds for " + target_name(host) +
          (host.os == Os::macos ? ", aarch64-ios and aarch64-ios-sim" : "") + "; building for " + target_name(t) +
          " from here isn't supported yet, so build on " + (t.os == Os::ios ? "a Mac" : "that system");
    return false;
}

// The target `name` names, or false, with the reason in `why`, if it's none of target_names().
inline bool parse_target(const std::string& name, Target& t, std::string& why) {
    for (const TargetName* n = target_names(); n->name; n++) {
        if (name == n->name) {
            t = n->target;
            return true;
        }
    }
    why = "`" + name + "` isn't a target; the targets are x86_64-windows, aarch64-windows, x86_64-linux, aarch64-linux, "
          "aarch64-macos, aarch64-ios and aarch64-ios-sim, or the triples Rust gives them, such as aarch64-apple-ios";
    return false;
}

// The processor a release build may count on (D612): x86-64-v3 on x86, with AVX2, as most made
// since 2015 have; every Apple Silicon Mac on macOS, and on the simulator, which runs on one; the
// A12 on iOS, which every iPhone that runs kIosMin has; and ARMv8.2 on Arm Linux and Windows,
// which Graviton 2, Ampere Altra, the Raspberry Pi 5 and every Snapdragon laptop have. Debug
// builds count on nothing past the architecture, so they run anywhere.
constexpr const char* release_cpu_flag(Target t) {
    return t.arch == Arch::x86_64                ? "-march=x86-64-v3"
           : t.os == Os::ios && !t.simulator     ? "-mcpu=apple-a12"
           : apple(t)                            ? "-mcpu=apple-m1"
                                                 : "-march=armv8.2-a";
}

// The default level's name, as `--cpu` takes it: `x86-64-v3`, `apple-m1`, `apple-a12` or `armv8.2-a`.
inline std::string default_cpu(Target t) {
    std::string flag = release_cpu_flag(t);
    return flag.substr(flag.find('=') + 1);
}

// The levels `--cpu` and CALX_CPU take for a release build on the target, the default among them
// (CLX-381). On x86 they're the psABI's: x86-64, any 64-bit processor; v2 adds SSE4.2 and POPCNT,
// as nearly every one since 2009 has, and old VPS and home servers without AVX2 do; v3 is the
// default; v4 adds AVX-512. On Arm Linux and Windows, ARMv8.0 is the Raspberry Pi 4's. The runtime
// counts on no more than each says: its AVX2 code checks for AVX2 as the program runs, or has an
// SSE2 path that a build without it uses, as debug builds do. Apple names its processors rather
// than levels: the M1 for a Mac and the simulator, and the A12 for an iPhone (CLX-408).
inline const char* const* cpu_levels(Target t) {
    static const char* const x86[] = {"x86-64", "x86-64-v2", "x86-64-v3", "x86-64-v4", nullptr};
    static const char* const arm[] = {"armv8-a", "armv8.2-a", nullptr};
    static const char* const mac[] = {"apple-m1", nullptr};
    static const char* const iphone[] = {"apple-a12", nullptr};
    return t.arch == Arch::x86_64 ? x86 : t.os == Os::ios && !t.simulator ? iphone : apple(t) ? mac : arm;
}

// clang's flag for the level `cpu` names, "" taking the default, or "" with the reason in `error`
// if the target has no such level.
inline std::string cpu_flag(Target t, const std::string& cpu, std::string& error) {
    if (cpu.empty()) return release_cpu_flag(t);
    std::string list;
    for (const char* const* l = cpu_levels(t); *l; l++) {
        if (cpu == *l) return (apple(t) ? "-mcpu=" : "-march=") + cpu;
        list += std::string(list.empty() ? "" : l[1] ? ", " : " or ") + *l;
    }
    bool x86_level = cpu.rfind("x86-64", 0) == 0;
    if (x86_level != (t.arch == Arch::x86_64) && (x86_level || cpu.rfind("arm", 0) == 0 || cpu.rfind("apple", 0) == 0)) {
        error = "`" + cpu + "` is a level of " + (x86_level ? "x86-64" : "Arm64") + " processors, and this calx builds for " +
                target_name(t) + ", whose release builds take " + list;
    } else {
        error = "`" + cpu + "` isn't a processor level; release builds for " + target_name(t) + " take " + list;
    }
    return "";
}

// llvm-mingw's folder for the target's headers and libraries, beside its bin, and the target clang
// builds for with them (CLX-110).
constexpr const char* mingw_dir(Arch arch) { return arch == Arch::x86_64 ? "x86_64-w64-mingw32" : "aarch64-w64-mingw32"; }
constexpr const char* mingw_target_flag(Arch arch) {
    return arch == Arch::x86_64 ? "--target=x86_64-w64-windows-gnu" : "--target=aarch64-w64-windows-gnu";
}

// The target for Visual Studio's C runtime and linker, which a static library for a host such as
// Rust's MSVC target is built for, whichever clang builds it (CLX-334).
constexpr const char* msvc_target_flag(Arch arch) {
    return arch == Arch::x86_64 ? "--target=x86_64-pc-windows-msvc" : "--target=aarch64-pc-windows-msvc";
}

// What a program's and a library's file names end in.
constexpr const char* exe_suffix(Os os) { return os == Os::windows ? ".exe" : ""; }
constexpr const char* lib_suffix(Os os) { return os == Os::windows ? ".dll" : os == Os::macos ? ".dylib" : ".so"; }

}  // namespace calx
