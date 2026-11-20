#pragma once

#include <string>

namespace calx {

// The system and the processor a program is built for (D612): Windows, Linux or macOS, on x86-64
// or Arm64. Calx builds for the one it runs on, so the target is fixed when the compiler is
// built, but what the toolchain passes for it comes from here rather than from the compiler's own
// #ifdefs, which are about the system the compiler itself calls (CLX-273).
enum class Os { windows, linux, macos };
enum class Arch { x86_64, aarch64 };

struct Target {
    Os os;
    Arch arch;
};

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
    return {os, arch};
}

// What programs are built for.
inline constexpr Target kTarget = host_target();

// `windows`, `linux` or `macos`, as Rust's `std::env::consts::OS` names them, and std.env.os().
constexpr const char* os_name(Os os) { return os == Os::windows ? "windows" : os == Os::macos ? "macos" : "linux"; }

// `x86_64` or `aarch64`, as Rust's `std::env::consts::ARCH` names them, and std.env.arch().
constexpr const char* arch_name(Arch arch) { return arch == Arch::x86_64 ? "x86_64" : "aarch64"; }

// `x86_64-windows`, as CALX_SPEC.md names the targets.
inline std::string target_name(Target t) { return std::string(arch_name(t.arch)) + "-" + os_name(t.os); }

// `Windows, x64` or `macOS, Arm64`, for people: `calx version` and `calx doctor`.
inline std::string target_label(Target t) {
    std::string os = t.os == Os::windows ? "Windows" : t.os == Os::macos ? "macOS" : "Linux";
    return os + (t.arch == Arch::x86_64 ? ", x64" : ", Arm64");
}

// The processor a release build may count on (D612): x86-64-v3 on x86, with AVX2, as most made
// since 2015 have; every Apple Silicon Mac on macOS; and ARMv8.2 on Arm Linux and Windows, which
// Graviton 2, Ampere Altra, the Raspberry Pi 5 and every Snapdragon laptop have. Debug builds
// count on nothing past the architecture, so they run anywhere.
constexpr const char* release_cpu_flag(Target t) {
    return t.arch == Arch::x86_64 ? "-march=x86-64-v3" : t.os == Os::macos ? "-mcpu=apple-m1" : "-march=armv8.2-a";
}

// The default level's name, as `--cpu` takes it: `x86-64-v3`, `apple-m1` or `armv8.2-a`.
inline std::string default_cpu(Target t) {
    std::string flag = release_cpu_flag(t);
    return flag.substr(flag.find('=') + 1);
}

// The levels `--cpu` and CALX_CPU take for a release build on the target, the default among them
// (CLX-381). On x86 they're the psABI's: x86-64, any 64-bit processor; v2 adds SSE4.2 and POPCNT,
// as nearly every one since 2009 has, and old VPS and home servers without AVX2 do; v3 is the
// default; v4 adds AVX-512. On Arm Linux and Windows, ARMv8.0 is the Raspberry Pi 4's. The runtime
// counts on no more than each says: its AVX2 code checks for AVX2 as the program runs, or has an
// SSE2 path that a build without it uses, as debug builds do.
inline const char* const* cpu_levels(Target t) {
    static const char* const x86[] = {"x86-64", "x86-64-v2", "x86-64-v3", "x86-64-v4", nullptr};
    static const char* const arm[] = {"armv8-a", "armv8.2-a", nullptr};
    static const char* const mac[] = {"apple-m1", nullptr};
    return t.arch == Arch::x86_64 ? x86 : t.os == Os::macos ? mac : arm;
}

// clang's flag for the level `cpu` names, "" taking the default, or "" with the reason in `error`
// if the target has no such level.
inline std::string cpu_flag(Target t, const std::string& cpu, std::string& error) {
    if (cpu.empty()) return release_cpu_flag(t);
    std::string list;
    for (const char* const* l = cpu_levels(t); *l; l++) {
        if (cpu == *l) return (t.os == Os::macos ? "-mcpu=" : "-march=") + cpu;
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

// What a program's and a library's file names end in.
constexpr const char* exe_suffix(Os os) { return os == Os::windows ? ".exe" : ""; }
constexpr const char* lib_suffix(Os os) { return os == Os::windows ? ".dll" : os == Os::macos ? ".dylib" : ".so"; }

}  // namespace calx
