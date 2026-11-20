#include "modules.h"
#include "toolchain.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace calx {

namespace fs = std::filesystem;

namespace {

std::string q(std::string_view s) { return "`" + std::string(s) + "`"; }

// `std.core`: the interfaces behind the derived properties, which a struct or an enum implements to
// write its own (D102). Every program has it, and nothing imports it: the checker finds these
// names here when a module doesn't declare them, and they're reserved, so none can.
//
// `Error` is here too, the error any error converts into (CLX-95): a module sees it unless it
// declares or imports an `Error` of its own, as `std.zlib` does. `try` converts with
// `error_from`, and `.context(...)` with `error_context` or `error_add_context`, which no module
// can name. Only the functions a program uses are compiled into it.
const char* const kCore = R"(module std.core

pub interface Equal {
    fn eq(self, other: Self) -> bool
}

pub interface Display {
    fn show(self)
}

pub interface Hash {
    fn hash(self, h: mut Hasher)
}

pub interface Order {
    fn cmp(self, other: Self) -> Ordering
}

pub struct Hasher {
    state: u64
}

pub interface From<T> {
    fn from(value: own T) -> Self
}

pub interface JavaHash {
    fn java_hash(self) -> i32
}

pub interface Iterator<T> {
    fn next(mut self) -> T?
}

pub interface Add<T> {
    fn add(self, other: T) -> Self
}

pub interface Sub<T> {
    fn sub(self, other: T) -> Self
}

pub interface Mul<T> {
    fn mul(self, other: T) -> Self
}

pub interface Div<T> {
    fn div(self, other: T) -> Self
}

pub interface Rem<T> {
    fn rem(self, other: T) -> Self
}

pub interface Neg {
    fn neg(self) -> Self
}

pub interface Index<K> {
    type Value

    fn index(self, key: K) -> Self.Value
}

pub interface SetIndex<K, V> {
    fn set_index(mut self, key: K, value: own V)
}

pub struct Error {
    text: String
    doing: List<String>
}

impl Error {
    pub fn new(message: own String) -> Error {
        return Error { text: message, doing: List<String>() }
    }

    pub fn message(self) -> String {
        return self.text.copy()
    }

    pub fn contexts(self) -> List<String> {
        var out = List<String>()
        out.reserve(self.doing.len())
        var i = self.doing.len()
        while i > 0 {
            i -= 1
            out.push(self.doing[i].copy())
        }
        return out
    }

    pub fn with_context(own self, text: own String) -> Error {
        var e = self
        e.doing.push(text)
        return e
    }
}

impl Display for Error {
    fn show(self) {
        var i = self.doing.len()
        while i > 0 {
            i -= 1
            print("{self.doing[i]}: ")
        }
        print(self.text)
    }
}

pub fn error_from<T: Display>(value: own T) -> Error {
    return Error { text: "{value}", doing: List<String>() }
}

pub fn error_context<T: Display>(value: own T, text: own String) -> Error {
    var doing = List<String>()
    doing.push(text)
    return Error { text: "{value}", doing: doing }
}

pub fn error_add_context(value: own Error, text: own String) -> Error {
    return value.with_context(text)
}
/// The place a `@track_caller` function was called from, as `caller()` gives it inside one.
pub struct Caller {
    /// The source file's path, as the program was built from it: `src/main.calx`.
    pub file: String
    pub line: u32
    pub column: u32
    /// The name of the function the call is in, such as `main`, or `<closure>` in a closure.
    pub function: String
}

impl Display for Caller {
    fn show(self) {
        print("{self.file}:{self.line}:{self.column}")
    }
}

// A `@track_caller` function's hidden parameter, `path:line:column`, a zero byte, then the
// function's name. The path can hold colons, as `C:/a.calx` does, so the numbers are the last two.
pub fn caller_from(text: str) -> Caller {
    var nul: usize = 0
    while text.byte_at(nul) != 0 {
        nul += 1
    }
    var at = nul
    var column: u32 = 0
    var scale: u32 = 1
    while text.byte_at(at - 1) != 58 {
        at -= 1
        column += u32(text.byte_at(at) - 48) * scale
        scale *= 10
    }
    at -= 1
    var line: u32 = 0
    scale = 1
    while text.byte_at(at - 1) != 58 {
        at -= 1
        line += u32(text.byte_at(at) - 48) * scale
        scale *= 10
    }
    return Caller { file: String(text[0..at - 1]), line: line, column: column, function: String(text[nul + 1..text.len()]) }
}
)";

}  // namespace

class Loader {
public:
    Loader(ParseContext ctx, Sources& sources, const std::string& home, const std::string& home_error)
        : ctx_(ctx), sources_(sources), home_(home), home_error_(home_error) {}

    std::vector<Module*> run(std::uint32_t main_file) {
        Module* core = parse_file(ctx_, sources_.add("<std.core>", kCore));
        core->builtin = true;
        order_.push_back(core);
        Module* m = parse_file(ctx_, main_file);
        fs::path path(sources_.file(main_file).path);
        std::string stem = path.stem().string();
        fs::path dir = path.parent_path();
        if (m->name.parts.empty()) {
            // Without a `module` line, the main module is named after its file. One that isn't a
            // name, such as `2d-test.calx`, is still a program; it just can't be imported.
            std::string name;
            for (char c : stem) name += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
            if (name.empty() || std::isdigit(static_cast<unsigned char>(name[0]))) name = "m" + name;
            std::vector<Name> parts{ctx_.names.intern(name)};
            m->name.parts = ctx_.arena.copy(parts);
        } else {
            // `module a.b.c` is `a/b/c.calx`, so the root is two folders up.
            const Slice<Name>& parts = m->name.parts;
            std::string_view last = text(parts[parts.size() - 1]);
            if (last != stem) {
                ctx_.diags.error(m->name.span, "module " + q(dotted(parts)) + " must be in a file named " +
                                                   q(std::string(last) + ".calx"));
            }
            for (std::size_t i = parts.size() - 1; i-- > 0;) {
                if (dir.filename().string() != text(parts[i])) {
                    ctx_.diags.error(m->name.span, "module " + q(dotted(parts)) + " must be in the folder " +
                                                       q(folder_of(parts)) + " under the source root");
                    break;
                }
                dir = dir.parent_path();
            }
        }
        root_ = dir;
        // `std` is the standard library's, so a program's own modules can't use the name.
        if (!m->name.parts.empty() && text(m->name.parts[0]) == "std" && !in_std(path)) {
            ctx_.diags.error(m->name.span.end > m->name.span.begin ? m->name.span : Span{main_file, 0, 0},
                             "`std` is the standard library's name, so a program's modules can't use it");
        }
        by_name_[dotted(m->name.parts)] = m;
        main_ = m;
        if (package_options_.enabled && !in_std(path)) {
            // The calx.toml nearest the main file, and the packages it depends on (CLX-370).
            packages_from_ = package_options_.from.empty() ? path.parent_path().generic_string() : package_options_.from;
            std::size_t errors = ctx_.diags.error_count();
            load_packages(sources_, ctx_.diags, packages_from_, packages_);
            if (ctx_.diags.error_count() != errors) return order_;
            if (packages_.any()) {
                std::string first(text(m->name.parts[0]));
                if (packages_.find(first) > 0) {
                    ctx_.diags.error(m->name.span.end > m->name.span.begin ? m->name.span : Span{main_file, 0, 0},
                                     q(first) + " is the name of a package this program uses, so this package's modules can't use it");
                }
            }
        }
        visit(m);
        return order_;
    }

    void use_packages(const PackageOptions& options) { package_options_ = options; }
    const PackageSet& packages() const { return packages_; }
    std::string packages_from() const { return packages_from_; }

    void add_items(Module* m, const std::vector<Item*>& items) {
        std::vector<Item*> all(m->items.begin(), m->items.end());
        all.insert(all.end(), items.begin(), items.end());
        m->items = ctx_.arena.copy(all);
        // Put every module in order again, from the main one. Their imports are found already,
        // and new ones are found as the walk meets them.
        Module* core = order_.front();
        order_.clear();
        state_.clear();
        stack_.clear();
        order_.push_back(core);
        visit(main_);
    }

    const std::vector<Module*>& modules() const { return order_; }
    std::string root() const { return root_.generic_string(); }

    std::vector<std::string> folders() const {
        std::vector<std::string> out;
        for (const auto& [dir, names] : listings_) out.push_back(dir);
        return out;
    }

private:
    std::string_view text(Name n) const { return ctx_.names.text(n); }

    std::string dotted(const Slice<Name>& parts) const {
        std::string s;
        for (std::size_t i = 0; i < parts.size(); i++) {
            if (i) s += '.';
            s += text(parts[i]);
        }
        return s;
    }

    std::string folder_of(const Slice<Name>& parts) const {
        std::string s;
        for (std::size_t i = 0; i + 1 < parts.size(); i++) {
            if (i) s += '/';
            s += text(parts[i]);
        }
        return s;
    }

    // Depth first, so each module comes after the ones it imports, and a module met again while
    // it's still being visited closes a cycle.
    void visit(Module* m) {
        state_[m] = 1;
        stack_.push_back(m);
        for (Item* it : m->items) {
            if (it->kind != ItemKind::import) continue;
            Module* target = it->imported ? it->imported : find(it, m);
            if (!target) continue;
            it->imported = target;
            int st = state_[target];
            if (st == 1) {
                cycle(it, target);
                it->imported = nullptr;
            } else if (st == 0) {
                visit(target);
            }
        }
        stack_.pop_back();
        state_[m] = 2;
        order_.push_back(m);
    }

    void cycle(Item* imp, Module* target) {
        if (target == stack_.back()) {
            ctx_.diags.error(imp->import_path.span, q(dotted(target->name.parts)) + " imports itself");
            return;
        }
        std::size_t k = 0;
        while (stack_[k] != target) k++;
        std::string chain = q(dotted(stack_[k]->name.parts));
        for (std::size_t i = k + 1; i < stack_.size(); i++) {
            chain += (i == k + 1 ? " imports " : ", which imports ") + q(dotted(stack_[i]->name.parts));
        }
        chain += ", which imports " + q(dotted(target->name.parts));
        ctx_.diags.error(imp->import_path.span, "imports can't form a cycle: " + chain);
    }

    std::string dotted(const std::vector<Name>& parts) const {
        std::string s;
        for (std::size_t i = 0; i < parts.size(); i++) {
            if (i) s += '.';
            s += text(parts[i]);
        }
        return s;
    }

    // The package a module is in: 0 for the main one, or for a program with no calx.toml.
    std::size_t package_of(Module* m) const {
        auto it = package_of_.find(m);
        return it == package_of_.end() ? 0 : it->second;
    }

    // Where package `p`'s module names start from.
    fs::path base_of(std::size_t p) const { return p == 0 ? root_ : fs::path(packages_.packages[p].dir); }

    // A module's whole name, which no other module of the program has: its name in its package,
    // after the package's name, but for a package's module of its own name, which is the
    // package's. The main package's modules have their names as they are.
    std::string whole_name(std::size_t p, const std::vector<Name>& rel) const {
        if (p == 0) return dotted(rel);
        const std::string& pkg = packages_.packages[p].name;
        if (rel.size() == 1 && text(rel[0]) == pkg) return pkg;
        return pkg + "." + dotted(rel);
    }

    Module* find(Item* imp, Module* from) {
        const Slice<Name>& parts = imp->import_path.parts;
        std::string written = dotted(parts);
        bool is_std = text(parts[0]) == "std";
        std::string first(text(parts[0]));
        std::size_t pkg = package_of(from);
        std::size_t target = pkg;
        std::vector<Name> rel(parts.begin(), parts.end());
        // `import spar.window` in a package whose calx.toml lists `spar` is spar's `window`.
        if (!is_std && packages_.any()) {
            const Package& here = packages_.packages[pkg];
            int dep = -1;
            if (auto d = here.deps.find(first); d != here.deps.end()) dep = static_cast<int>(d->second);
            else if (package_options_.any && pkg == 0) dep = packages_.find(first);
            if (dep > 0) {
                if (local(pkg, parts[0])) {
                    ctx_.diags.error(imp->import_path.span, q(first) + " is both a package " + where_listed(pkg) +
                                                                " and a module of " + this_package(pkg) + "; rename the module");
                    return nullptr;
                }
                target = static_cast<std::size_t>(dep);
                rel.erase(rel.begin());
                if (rel.empty()) rel.push_back(parts[0]);
            }
        }
        std::string name = is_std ? written : whole_name(target, rel);
        if (auto it = by_name_.find(name); it != by_name_.end()) return it->second;
        if (is_std && home_.empty()) {
            ctx_.diags.error(imp->import_path.span, "can't import " + q(name) + ": " + home_error_);
            by_name_[name] = nullptr;
            return nullptr;
        }
        fs::path base = is_std ? fs::path(home_) : base_of(target);
        fs::path file = base;
        for (Name p : rel) file /= std::string(text(p));
        file += ".calx";
        std::uint32_t id = exact_case(base, rel) ? sources_.load(file.generic_string()) : 0;
        if (id == 0 && is_std) {
            ctx_.diags.error(imp->import_path.span, "the standard library has no module " + q(name));
            by_name_[name] = nullptr;
            return nullptr;
        }
        if (id == 0) {
            const std::string& pname = packages_.any() ? packages_.packages[target].name : std::string();
            if (target != pkg && rel.size() == 1 && text(rel[0]) == pname) {
                ctx_.diags.error(imp->import_path.span, "the package " + q(pname) + " has no module of its own name, the file " +
                                                            q(file.generic_string()) + "; import one of its modules, as " +
                                                            q("import " + pname + ".NAME"));
            } else if (target != pkg) {
                ctx_.diags.error(imp->import_path.span, "the package " + q(pname) + " has no module " + q(dotted(rel)) +
                                                            "; it would be the file " + q(file.generic_string()));
            } else if (int other = packages_.any() ? packages_.find(first) : -1; other > 0 && static_cast<std::size_t>(other) != pkg) {
                ctx_.diags.error(imp->import_path.span, q(first) + " is a package this program uses, but " +
                                                            (pkg == 0 ? std::string("this package's") : q(packages_.packages[pkg].name) + "'s") +
                                                            " calx.toml doesn't list it; add it under [dependencies] to import it here");
            } else {
                ctx_.diags.error(imp->import_path.span, "there's no module " + q(written) + "; it would be the file " +
                                                            q(file.generic_string()));
            }
            by_name_[name] = nullptr;
            return nullptr;
        }
        Module* m = parse_file(ctx_, id);
        std::string in_package = target == 0 ? "" : " of the package " + q(packages_.packages[target].name);
        std::string rel_name = dotted(rel);
        // A module in a folder says so, so that any file can be checked by itself, as editors
        // do, and still find the root.
        if (m->name.parts.empty() && rel.size() > 1) {
            Span at{id, 0, 0};
            if (!m->items.empty()) at = m->items[0]->name_span.end > m->items[0]->name_span.begin ? m->items[0]->name_span : m->items[0]->span;
            ctx_.diags.error(at, "a module in a folder starts with its `module` line: " + q("module " + rel_name));
        }
        if (!m->name.parts.empty() && dotted(m->name.parts) != rel_name) {
            ctx_.diags.error(m->name.span, "this file is module " + q(rel_name) + in_package + " by its place, so its `module` line must say " +
                                               q("module " + rel_name) + ", or be left out");
        }
        // The main package's own module can't have a name a package of the program has, which
        // would be two modules of one name.
        if (target == 0 && !is_std && packages_.any() && packages_.find(first) > 0) {
            ctx_.diags.error(imp->import_path.span, q(first) + " is the name of a package this program uses, so this package's modules can't use it; rename the module");
        }
        if (is_std || target == 0) {
            m->name.parts = parts;
        } else {
            std::vector<Name> whole;
            whole.push_back(ctx_.names.intern(packages_.packages[target].name));
            if (!(rel.size() == 1 && text(rel[0]) == packages_.packages[target].name)) whole.insert(whole.end(), rel.begin(), rel.end());
            m->name.parts = ctx_.arena.copy(whole);
        }
        package_of_[m] = is_std ? 0 : target;
        by_name_[name] = m;
        return m;
    }

    // Whether package `p` has a module or a folder of modules named `n`.
    bool local(std::size_t p, Name n) const {
        std::vector<Name> one{n};
        if (exact_case(base_of(p), one)) return true;
        fs::path dir = base_of(p);
        auto it = listings_.find((dir.empty() ? fs::path(".") : dir).string());
        std::error_code ec;
        return it != listings_.end() && it->second.count(std::string(text(n))) &&
               fs::is_directory(long_path(((dir.empty() ? fs::path(".") : dir) / std::string(text(n))).string()), ec);
    }

    std::string this_package(std::size_t p) const { return p == 0 ? "this package" : "the package " + q(packages_.packages[p].name); }

    std::string where_listed(std::size_t p) const {
        return p == 0 ? "this package's calx.toml lists" : q(packages_.packages[p].name) + "'s calx.toml lists";
    }

    // Whether the folders and file a module names exist with exactly those letters, so `import
    // Util` finds `util.calx` on neither Windows nor Linux.
    // Each folder is listed once in a build, where listing std's for each import took a program
    // that uses std.regex a millisecond (CLX-240).
    bool exact_case(const fs::path& base, const std::vector<Name>& parts) const {
        fs::path dir = base.empty() ? fs::path(".") : base;
        for (std::size_t i = 0; i < parts.size(); i++) {
            std::string want = std::string(text(parts[i])) + (i + 1 == parts.size() ? ".calx" : "");
            auto [listing, fresh] = listings_.try_emplace(dir.string());
            if (fresh) {
                std::error_code ec;
                for (fs::directory_iterator d(long_path(dir.string()), ec), end; !ec && d != end; d.increment(ec)) {
                    listing->second.insert(d->path().filename().string());
                }
            }
            if (!listing->second.count(want)) return false;
            dir /= want;
        }
        return true;
    }

    // Whether `file` is in the standard library's folder.
    bool in_std(const fs::path& file) const {
        if (home_.empty()) return false;
        std::error_code ec;
        fs::path std_dir = fs::weakly_canonical(fs::path(home_) / "std", ec);
        fs::path f = fs::weakly_canonical(file, ec);
        auto [a, b] = std::mismatch(std_dir.begin(), std_dir.end(), f.begin(), f.end());
        return a == std_dir.end();
    }

    ParseContext ctx_;
    Sources& sources_;
    std::string home_, home_error_;
    fs::path root_;
    std::unordered_map<std::string, Module*> by_name_;
    mutable std::unordered_map<std::string, std::unordered_set<std::string>> listings_;  // each folder's names
    std::unordered_map<Module*, int> state_;  // 1 while visiting, 2 once done
    std::vector<Module*> stack_;
    std::vector<Module*> order_;
    Module* main_ = nullptr;
    PackageOptions package_options_;
    PackageSet packages_;
    std::string packages_from_;
    std::unordered_map<Module*, std::size_t> package_of_;
};

ModuleLoader::ModuleLoader(ParseContext ctx, Sources& sources, const std::string& home, const std::string& home_error)
    : impl_(std::make_unique<Loader>(ctx, sources, home, home_error)) {}

ModuleLoader::~ModuleLoader() = default;

const std::vector<Module*>& ModuleLoader::load(std::uint32_t main_file) {
    impl_->run(main_file);
    return impl_->modules();
}

void ModuleLoader::add_items(Module* m, const std::vector<Item*>& items) { impl_->add_items(m, items); }

const std::vector<Module*>& ModuleLoader::modules() const { return impl_->modules(); }

std::string ModuleLoader::root() const { return impl_->root(); }

std::vector<std::string> ModuleLoader::folders() const { return impl_->folders(); }

void ModuleLoader::use_packages(const PackageOptions& options) { impl_->use_packages(options); }

const PackageSet& ModuleLoader::packages() const { return impl_->packages(); }

std::string ModuleLoader::packages_from() const { return impl_->packages_from(); }

std::vector<Module*> load_program(ParseContext ctx, Sources& sources, std::uint32_t main_file, const std::string& home,
                                  const std::string& home_error, const PackageOptions& packages) {
    Loader loader(ctx, sources, home, home_error);
    loader.use_packages(packages);
    return loader.run(main_file);
}

}  // namespace calx
