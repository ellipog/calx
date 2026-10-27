// Shared basics: the arena every compiler structure lives in, interned names, and source spans.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace calx {

// A bump allocator. Everything the compiler builds for one run (tokens, syntax trees, types) is
// freed at once when the run ends, so nothing is freed one by one.
class Arena {
public:
    Arena() = default;
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    void* allocate(std::size_t size, std::size_t align) {
        std::size_t at = (used_ + align - 1) & ~(align - 1);
        if (blocks_.empty() || at + size > capacity_) {
            std::size_t want = size + align > kBlock ? size + align : kBlock;
            blocks_.push_back(std::make_unique<std::byte[]>(want));
            capacity_ = want;
            at = (reinterpret_cast<std::uintptr_t>(blocks_.back().get()) % align == 0)
                     ? 0
                     : align - reinterpret_cast<std::uintptr_t>(blocks_.back().get()) % align;
        }
        used_ = at + size;
        return blocks_.back().get() + at;
    }

    template <typename T, typename... Args>
    T* make(Args&&... args) {
        return new (allocate(sizeof(T), alignof(T))) T(std::forward<Args>(args)...);
    }

    // A copy of `items` in the arena, as a span that lives as long as the arena.
    template <typename T>
    std::pair<T*, std::size_t> copy(const std::vector<T>& items) {
        if (items.empty()) return {nullptr, 0};
        T* out = static_cast<T*>(allocate(sizeof(T) * items.size(), alignof(T)));
        for (std::size_t i = 0; i < items.size(); i++) new (out + i) T(items[i]);
        return {out, items.size()};
    }

    std::string_view copy(std::string_view s) {
        char* out = static_cast<char*>(allocate(s.size() + 1, 1));
        std::memcpy(out, s.data(), s.size());
        out[s.size()] = '\0';
        return {out, s.size()};
    }

private:
    static constexpr std::size_t kBlock = 1 << 20;
    std::vector<std::unique_ptr<std::byte[]>> blocks_;
    std::size_t used_ = 0;
    std::size_t capacity_ = 0;
};

// A read-only view of `count` items in the arena.
template <typename T>
struct Slice {
    T* data = nullptr;
    std::size_t count = 0;

    Slice() = default;
    Slice(std::pair<T*, std::size_t> p) : data(p.first), count(p.second) {}
    T* begin() const { return data; }
    T* end() const { return data + count; }
    std::size_t size() const { return count; }
    bool empty() const { return count == 0; }
    T& operator[](std::size_t i) const { return data[i]; }
};

// An interned name. Equal names have equal ids, so comparing names is comparing integers.
struct Name {
    std::uint32_t id = 0;  // 0 is "no name"
    bool operator==(const Name&) const = default;
    explicit operator bool() const { return id != 0; }
};

class Names {
public:
    Names() { text_.push_back({}); }

    Name intern(std::string_view s) {
        auto it = ids_.find(s);
        if (it != ids_.end()) return Name{it->second};
        std::string_view owned = arena_.copy(s);
        auto id = static_cast<std::uint32_t>(text_.size());
        text_.push_back(owned);
        ids_.emplace(owned, id);
        return Name{id};
    }

    std::string_view text(Name n) const { return text_[n.id]; }

private:
    Arena arena_;
    std::vector<std::string_view> text_;
    std::unordered_map<std::string_view, std::uint32_t> ids_;
};

// A byte range in one source file.
struct Span {
    std::uint32_t file = 0;
    std::uint32_t begin = 0;
    std::uint32_t end = 0;

    static Span join(Span a, Span b) { return Span{a.file, a.begin, b.end > a.end ? b.end : a.end}; }
};

}  // namespace calx

template <>
struct std::hash<calx::Name> {
    std::size_t operator()(calx::Name n) const noexcept { return n.id; }
};
