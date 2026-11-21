#include "packages.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <set>
#include <string_view>
#include <utility>

#include "lexer.h"
#include "term.h"
#include "toml.h"
#include "toolchain.h"

#ifndef _WIN32
#include <stdlib.h>
#endif

namespace calx {

namespace fs = std::filesystem;

int PackageSet::find(const std::string& name) const {
    for (std::size_t i = 0; i < packages.size(); i++) {
        if (packages[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

namespace {

std::string q(std::string_view s) { return "`" + std::string(s) + "`"; }

// A path from UTF-8 text, and back, so a folder with letters outside ASCII is the same folder on
// Windows as elsewhere.
fs::path to_path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string from_path(const fs::path& p) {
    std::u8string u = p.generic_u8string();
    std::string s(u.begin(), u.end());
    // The `\\?\` form the cache can be in, which generic text can't keep; long_path gives it
    // back where a path needs it.
    if (s.rfind("//?/UNC/", 0) == 0) return "//" + s.substr(8);
    if (s.rfind("//?/", 0) == 0) return s.substr(4);
    return s;
}

bool is_dir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(long_path(from_path(p)), ec);
}

bool is_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(long_path(from_path(p)), ec);
}

// `p` as messages show it: from the current folder when it's under it, and otherwise whole.
std::string display(const fs::path& p) {
    std::error_code ec;
    fs::path cwd = fs::current_path(ec);
    if (!ec) {
        fs::path rel = p.lexically_relative(cwd);
        if (!rel.empty() && rel.begin()->string() != "..") return from_path(rel);
    }
    return from_path(p);
}

// The same folder by any path: symbolic links followed, and on Windows its letters' case as the
// folder has it.
std::string identity(const fs::path& p) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(long_path(from_path(p)), ec);
    return from_path(ec ? p.lexically_normal() : c);
}

bool within(const fs::path& inner, const fs::path& outer) {
    std::string a = identity(inner), b = identity(outer);
    return a == b || (a.size() > b.size() && a.compare(0, b.size(), b) == 0 && (a[b.size()] == '/' || b.back() == '/'));
}

const std::set<std::string>& keywords() {
    static const std::set<std::string> k = {
#define X(name, text) text,
        CALX_KEYWORDS(X)
#undef X
    };
    return k;
}

// A package's name is how programs import it, so it's a name modules can have.
bool good_name(const std::string& s) {
    if (s.empty() || !(s[0] >= 'a' && s[0] <= 'z')) return false;
    for (char c : s) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return !keywords().count(s);
}

std::string name_rule() {
    return "a package's name is how programs import it, so it's lowercase letters, digits and `_`, starting with a "
           "letter, and not a keyword: \"spar\", \"game_util\"";
}

// `1.2.3`, with `-pre` and `+build` after it if wanted, as semver has.
bool good_version(const std::string& s) {
    std::size_t i = 0;
    for (int part = 0; part < 3; part++) {
        std::size_t start = i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
        if (i == start || (i - start > 1 && s[start] == '0')) return false;
        if (part < 2) {
            if (i >= s.size() || s[i] != '.') return false;
            i++;
        }
    }
    for (char lead : {'-', '+'}) {
        if (i < s.size() && s[i] == lead) {
            std::size_t start = ++i;
            while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '.' || s[i] == '-')) i++;
            if (i == start) return false;
        }
    }
    return i == s.size();
}

enum class RefKind { none, tag, rev, branch };

const char* ref_word(RefKind k) {
    switch (k) {
        case RefKind::tag: return "tag";
        case RefKind::rev: return "rev";
        case RefKind::branch: return "branch";
        case RefKind::none: break;
    }
    return "";
}

struct DepSpec {
    std::string name;
    Span key;
    bool git = false;
    std::string path;
    std::string url;
    RefKind ref_kind = RefKind::none;
    std::string ref;
    Span where;  // the path, or the git URL
    Span ref_at;
};

struct Manifest {
    std::uint32_t file = 0;
    std::string name, version;
    std::string description, company, copyright;  // optional, for a Windows program's version resource
    Span name_at;
    std::vector<DepSpec> deps;
};

struct LockEntry {
    std::string name, url, ref, commit;
    RefKind ref_kind = RefKind::none;
};

Span span(std::uint32_t file, const toml::Value& v) { return Span{file, v.begin, v.end > v.begin ? v.end : v.begin + 1}; }

Span key_span(std::uint32_t file, const toml::Value& v) {
    if (v.key_end > v.key_begin) return Span{file, v.key_begin, v.key_end};
    return span(file, v);
}

// Reads the TOML file `path` into `root`, as a source errors can point into. 0 if it can't be read
// or isn't TOML, which is then an error.
std::uint32_t read_toml(Sources& sources, Diagnostics& diags, const std::string& path, toml::Value& root, Span from) {
    std::uint32_t id = sources.load(path);
    if (id == 0) {
        diags.error(from, "can't read " + q(path));
        return 0;
    }
    toml::Error e;
    if (!toml::parse(sources.file(id).text, root, e)) {
        diags.error(Span{id, e.at, e.end}, e.message);
        return 0;
    }
    return id;
}

// The git URL's last name, for the cache folder's: `spar` for `https://host/x/spar.git`.
std::string slug(const std::string& url) {
    std::string s = url;
    while (!s.empty() && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    std::size_t cut = s.find_last_of("/\\:");
    if (cut != std::string::npos) s = s.substr(cut + 1);
    if (s.size() > 4 && s.compare(s.size() - 4, 4, ".git") == 0) s.resize(s.size() - 4);
    std::string out;
    for (char c : s) out += std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ? c : '_';
    if (out.size() > 24) out.resize(24);
    return out.empty() ? "repo" : out;
}

void remove_tree(const fs::path& p) {
    // Git makes its objects read-only, which Windows won't remove as they are.
    std::error_code ec;
    fs::path at = long_path(from_path(p));
    for (fs::recursive_directory_iterator it(at, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code pe;
        fs::permissions(it->path(), fs::perms::owner_write, fs::perm_options::add, pe);
    }
    fs::remove_all(at, ec);
}

// Sets an environment variable for the programs this starts, and puts it back as it was after.
class EnvFor {
public:
    EnvFor(const char* name, const char* value) : name_(name) {
        const char* old = std::getenv(name);
        had_ = old != nullptr;
        if (had_) old_ = old;
        set(value);
    }
    ~EnvFor() { set(had_ ? old_.c_str() : nullptr); }

private:
    void set(const char* value) {
#ifdef _WIN32
        _putenv_s(name_, value ? value : "");
#else
        if (value) setenv(name_, value, 1);
        else unsetenv(name_);
#endif
    }
    const char* name_;
    std::string old_;
    bool had_ = false;
};

// Git, run as `git` from PATH, on the clones the cache keeps of each repository a package comes
// from: `git/NAME-HASH/db`, a bare clone, and a checkout of each commit used, at
// `git/NAME-HASH/checkouts/COMMIT`.
class Git {
public:
    explicit Git(Diagnostics& diags) : diags_(diags), cache_(to_path(cache_dir()) / "git") {}

    // The commit `d` is pinned to, `locked` if calx.lock gives one, with its checkout in
    // `checkout`. "" after an error.
    std::string pin(const DepSpec& d, const std::string& locked, fs::path& checkout) {
        fs::path entry = cache_ / (slug(d.url) + "-" + hash_hex({d.url}).substr(0, 16));
        fs::path db = entry / "db";
        if (!locked.empty()) {
            checkout = entry / "checkouts" / locked;
            // Fetched and pinned: no git at all, so it builds offline.
            if (is_dir(checkout)) return locked;
            if (!found(d)) return "";
            bool fresh = false;
            if (!is_dir(db)) {
                if (!clone(d, entry, db)) return "";
                fresh = true;
            }
            if (!has_commit(db, locked)) {
                if (!fresh && !fetch(d, db)) return "";
                if (!has_commit(db, locked)) {
                    diags_.error(d.where, "calx.lock pins " + q(d.name) + " to the commit " + locked.substr(0, 12) + ", which " + d.url +
                                              " doesn't have; take " + q(d.name) + "'s entry out of calx.lock to pin it again");
                    return "";
                }
            }
            return check_out(d, entry, db, locked, checkout) ? locked : "";
        }
        if (!found(d)) return "";
        bool fresh = false;
        if (!is_dir(db)) {
            if (!clone(d, entry, db)) return "";
            fresh = true;
        } else if ((d.ref_kind == RefKind::branch || d.ref_kind == RefKind::none) && !fetch(d, db)) {
            return "";
        }
        std::string commit = rev_parse(db, d);
        if (commit.empty() && !fresh && (d.ref_kind == RefKind::tag || d.ref_kind == RefKind::rev)) {
            // A tag or a commit the clone in the cache doesn't have yet.
            if (!fetch(d, db)) return "";
            commit = rev_parse(db, d);
        }
        if (commit.empty()) {
            missing_ref(db, d);
            return "";
        }
        checkout = entry / "checkouts" / commit;
        return check_out(d, entry, db, commit, checkout) ? commit : "";
    }

private:
    bool found(const DepSpec& d) {
        if (exe_.empty()) exe_ = which("git");
        if (exe_.empty()) {
            diags_.error(d.where, q(d.name) + " comes from git, and isn't in the cache yet: fetching it needs `git`, which isn't on PATH");
            return false;
        }
        return true;
    }

    // Runs git with `args`, and keeps what it wrote. Its exit status, or -1 if it couldn't start.
    int run(const std::vector<std::string>& args, std::string& out, std::string& err) {
        std::error_code ec;
        fs::path tmp = cache_ / "tmp";
        fs::create_directories(long_path(from_path(tmp)), ec);
        std::string base = from_path(tmp / unique_stamp());
        std::string in = base + ".in", o = base + ".out", e = base + ".err";
        write_file_atomic(in, "");
        std::vector<std::string> argv = {exe_};
        argv.insert(argv.end(), args.begin(), args.end());
        // Never a prompt for a name or a password: with no terminal to answer it, it would wait.
        EnvFor prompt("GIT_TERMINAL_PROMPT", "0");
        EnvFor manager("GCM_INTERACTIVE", "never");
        int status = run_redirected(argv, in, o, e);
        read_file(o, out);
        read_file(e, err);
        for (const std::string& f : {in, o, e}) fs::remove(long_path(f), ec);
        return status;
    }

    // What git said went wrong, as a line or two.
    static std::string said(const std::string& err) {
        std::string out;
        std::size_t at = 0;
        while (at < err.size()) {
            std::size_t end = err.find('\n', at);
            if (end == std::string::npos) end = err.size();
            std::string line = err.substr(at, end - at);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) out += (out.empty() ? "" : "; ") + line;
            at = end + 1;
        }
        return out.empty() ? "git ended with an error" : out;
    }

    void say_fetching(const DepSpec& d) {
        term::quiet();
        const term::Caps& c = term::err();
        std::fprintf(stderr, "%s %s %s\n", c.color ? term::accent(c, "Fetching").c_str() : "Fetching", d.name.c_str(),
                     (c.color ? term::dim(c, "from " + d.url) : "from " + d.url).c_str());
        std::fflush(stderr);
    }

    bool clone(const DepSpec& d, const fs::path& entry, const fs::path& db) {
        std::error_code ec;
        fs::create_directories(long_path(from_path(entry)), ec);
        say_fetching(d);
        fs::path tmp = entry / ("db.tmp-" + unique_stamp());
        std::string out, err;
        int status = run({"clone", "--bare", "--quiet", "--", d.url, from_path(tmp)}, out, err);
        if (status != 0) {
            remove_tree(tmp);
            diags_.error(d.where, "can't fetch " + q(d.name) + " from " + d.url + ": " + (status < 0 ? "git didn't start" : said(err)));
            return false;
        }
        fs::rename(long_path(from_path(tmp)), long_path(from_path(db)), ec);
        if (ec) remove_tree(tmp);  // another build cloned it first
        return is_dir(db);
    }

    bool fetch(const DepSpec& d, const fs::path& db) {
        say_fetching(d);
        std::string out, err;
        int status = run({"--git-dir=" + from_path(db), "fetch", "--quiet", "--force", "--", d.url, "+refs/heads/*:refs/heads/*",
                          "+refs/tags/*:refs/tags/*"},
                         out, err);
        if (status != 0) {
            diags_.error(d.where, "can't fetch " + q(d.name) + " from " + d.url + ": " + (status < 0 ? "git didn't start" : said(err)));
            return false;
        }
        return true;
    }

    bool has_commit(const fs::path& db, const std::string& commit) {
        std::string out, err;
        return run({"--git-dir=" + from_path(db), "cat-file", "-e", commit + "^{commit}"}, out, err) == 0;
    }

    std::string rev_parse(const fs::path& db, const DepSpec& d) {
        std::string spec;
        switch (d.ref_kind) {
            case RefKind::tag: spec = "refs/tags/" + d.ref; break;
            case RefKind::branch: spec = "refs/heads/" + d.ref; break;
            case RefKind::rev: spec = d.ref; break;
            case RefKind::none: spec = "HEAD"; break;
        }
        std::string out, err;
        if (run({"--git-dir=" + from_path(db), "rev-parse", "--verify", "--quiet", spec + "^{commit}"}, out, err) != 0) return "";
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        return out.size() == 40 ? out : "";
    }

    // The error for a tag, branch or commit the repository doesn't have, with those it has.
    void missing_ref(const fs::path& db, const DepSpec& d) {
        std::string message;
        std::vector<std::string> have;
        if (d.ref_kind == RefKind::tag || d.ref_kind == RefKind::branch) {
            std::string out, err;
            run({"--git-dir=" + from_path(db), "for-each-ref", "--format=%(refname:short)",
                 d.ref_kind == RefKind::tag ? "refs/tags" : "refs/heads"},
                out, err);
            std::size_t at = 0;
            while (at < out.size()) {
                std::size_t end = out.find('\n', at);
                if (end == std::string::npos) end = out.size();
                std::string line = out.substr(at, end - at);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) have.push_back(line);
                at = end + 1;
            }
        }
        switch (d.ref_kind) {
            case RefKind::tag: message = d.url + " has no tag " + q(d.ref); break;
            case RefKind::branch: message = d.url + " has no branch " + q(d.ref); break;
            case RefKind::rev: message = d.url + " has no commit " + q(d.ref); break;
            case RefKind::none: message = d.url + " has no commits"; break;
        }
        if (!have.empty()) {
            std::string list;
            for (std::size_t i = 0; i < have.size() && i < 10; i++) list += (i ? ", " : "") + have[i];
            if (have.size() > 10) list += ", and " + std::to_string(have.size() - 10) + " more";
            message += std::string("; its ") + (d.ref_kind == RefKind::tag ? "tags" : "branches") + " are " + list;
        }
        diags_.error(d.ref_at.end > d.ref_at.begin ? d.ref_at : d.where, message);
    }

    bool check_out(const DepSpec& d, const fs::path& entry, const fs::path& db, const std::string& commit, const fs::path& to) {
        if (is_dir(to)) return true;
        std::error_code ec;
        fs::create_directories(long_path(from_path(entry / "checkouts")), ec);
        fs::path tmp = entry / "checkouts" / (commit + ".tmp-" + unique_stamp());
        std::string out, err;
        // The files exactly as committed, whatever this machine's git does with line ends.
        int status = run({"clone", "--quiet", "--shared", "--no-checkout", "--config", "core.autocrlf=false", "--", from_path(db),
                          from_path(tmp)},
                         out, err);
        if (status == 0) {
            status = run({"-C", from_path(tmp), "-c", "advice.detachedHead=false", "checkout", "--quiet", commit}, out, err);
        }
        if (status != 0) {
            remove_tree(tmp);
            diags_.error(d.where, "can't check out " + q(d.name) + " at " + commit.substr(0, 12) + ": " + said(err));
            return false;
        }
        fs::rename(long_path(from_path(tmp)), long_path(from_path(to)), ec);
        if (ec) remove_tree(tmp);  // another build checked it out first
        return is_dir(to);
    }

    Diagnostics& diags_;
    fs::path cache_;
    std::string exe_;
};

class Resolver {
public:
    Resolver(Sources& sources, Diagnostics& diags, PackageSet& out) : sources_(sources), diags_(diags), out_(out), git_(diags) {}

    void run(const fs::path& toml) {
        std::size_t errors = diags_.error_count();
        fs::path dir = toml.parent_path();
        Node main;
        if (!manifest(display(toml), Span{}, main.m)) return;
        main.identity = "path:" + identity(dir);
        main.dir = dir;
        main.queued = true;
        Package p;
        p.name = main.m.name;
        p.version = main.m.version;
        p.description = main.m.description;
        p.company = main.m.company;
        p.copyright = main.m.copyright;
        p.dir = display(dir);
        p.source = p.dir;
        out_.packages.push_back(p);
        by_name_[p.name] = 0;
        by_identity_[main.identity] = 0;
        nodes_.push_back(std::move(main));
        out_.lock = display(dir / "calx.lock");
        if (!read_lock(out_.lock)) return;
        std::deque<std::size_t> queue{0};
        while (!queue.empty()) {
            std::size_t i = queue.front();
            queue.pop_front();
            for (std::size_t k = 0; k < nodes_[i].m.deps.size(); k++) {
                DepSpec d = nodes_[i].m.deps[k];
                int j = add(i, d);
                if (j < 0) continue;
                out_.packages[i].deps[d.name] = static_cast<std::size_t>(j);
                if (!nodes_[j].queued) {
                    nodes_[j].queued = true;
                    queue.push_back(static_cast<std::size_t>(j));
                }
            }
        }
        cycles();
        if (diags_.error_count() == errors) write_lock();
    }

private:
    struct Node {
        Manifest m;
        std::string identity;  // `path:FOLDER` or `git:URL@COMMIT`
        fs::path dir;
        fs::path repo;  // for a package from git, the checkout its path dependencies stay in
        std::string declared_by;
        Span declared;
        bool queued = false;
    };

    // Reads and checks the calx.toml at `path`.
    bool manifest(const std::string& path, Span from, Manifest& m) {
        toml::Value root;
        std::size_t errors = diags_.error_count();
        std::uint32_t id = read_toml(sources_, diags_, path, root, from);
        if (id == 0) return false;
        m.file = id;
        Span top{id, 0, 0};
        const toml::Value* package = nullptr;
        const toml::Value* deps = nullptr;
        for (const toml::Value& v : root.entries) {
            const std::string& k = v.key;
            if (k == "package" && v.kind == toml::Kind::table) {
                package = &v;
            } else if (k == "dependencies" && v.kind == toml::Kind::table) {
                deps = &v;
            } else if (k == "package" || k == "dependencies") {
                diags_.error(key_span(id, v), q(k) + " is a table: " + q("[" + k + "]"));
            } else {
                diags_.error(key_span(id, v), "calx.toml has no " + q(k) + "; it has the tables [package] and [dependencies]");
            }
        }
        if (!package) {
            diags_.error(top, "calx.toml needs a [package] table with the package's name and version:\n\n    [package]\n    name = \"" +
                                  suggest_name(path) + "\"\n    version = \"0.1.0\"");
            return false;
        }
        for (const toml::Value& v : package->entries) {
            const std::string& k = v.key;
            if (k == "name" || k == "version") {
                if (v.kind != toml::Kind::string) {
                    diags_.error(span(id, v), q(k) + " must be a string, in quotes, not " + toml::kind_name(v.kind));
                } else if (k == "name") {
                    m.name = v.string;
                    m.name_at = span(id, v);
                    if (!good_name(v.string)) diags_.error(span(id, v), q(v.string) + " can't be a package's name: " + name_rule());
                    else if (v.string == "std") diags_.error(span(id, v), "`std` is the standard library's name, so a package can't use it");
                } else {
                    m.version = v.string;
                    if (!good_version(v.string)) {
                        diags_.error(span(id, v), q(v.string) + " isn't a version: a version is three numbers, as \"0.1.0\", with `-pre` after "
                                                                 "them if wanted, as \"1.0.0-beta.1\"");
                    }
                }
            } else if (k == "description" || k == "company" || k == "copyright") {
                // What Windows shows in a program's Properties, from its version resource (CLX-414).
                if (v.kind != toml::Kind::string) {
                    diags_.error(span(id, v), q(k) + " must be a string, in quotes, not " + toml::kind_name(v.kind));
                } else {
                    (k == "description" ? m.description : k == "company" ? m.company : m.copyright) = v.string;
                }
            } else {
                diags_.error(key_span(id, v), "[package] has no " + q(k) +
                                                  "; it has `name` and `version`, and may have `description`, `company` and `copyright`");
            }
        }
        Span header = package->end > package->begin ? span(id, *package) : top;
        if (!package->get("name")) diags_.error(header, "[package] has no `name`: name = \"" + suggest_name(path) + "\"");
        if (!package->get("version")) diags_.error(header, "[package] has no `version`: version = \"0.1.0\"");
        if (deps) {
            for (const toml::Value& v : deps->entries) dependency(id, v.key, v, m);
        }
        return diags_.error_count() == errors;
    }

    std::string suggest_name(const std::string& path) {
        std::string folder = to_path(path).parent_path().filename().string();
        std::string s;
        for (char c : folder) {
            char l = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            s += (l >= 'a' && l <= 'z') || (l >= '0' && l <= '9') ? l : '_';
        }
        return good_name(s) ? s : "app";
    }

    void dependency(std::uint32_t id, const std::string& name, const toml::Value& v, Manifest& m) {
        DepSpec d;
        d.name = name;
        d.key = key_span(id, v);
        if (!good_name(name)) {
            diags_.error(d.key, q(name) + " can't be a dependency's name: " + name_rule());
            return;
        }
        if (name == "std") {
            diags_.error(d.key, "`std` is the standard library, which every program has without listing it");
            return;
        }
        if (v.kind != toml::Kind::table) {
            diags_.error(span(id, v), "a dependency is a table with its path or git URL: " + name + " = { path = \"../" + name + "\" }");
            return;
        }
        if (name == m.name) {
            diags_.error(d.key, "a package can't depend on itself");
            return;
        }
        const toml::Value* path = nullptr;
        const toml::Value* git = nullptr;
        const toml::Value* ref = nullptr;
        bool ok = true;
        for (const toml::Value& x : v.entries) {
            const std::string& k = x.key;
            bool known = k == "path" || k == "git" || k == "tag" || k == "rev" || k == "branch";
            if (!known) {
                std::string why = k == "version" ? "versions come with the registry, Quarry; a dependency here is found by its `path` or `git` URL"
                                                 : "its keys are `path`, or `git` with `tag`, `rev` or `branch`";
                diags_.error(key_span(id, x), "a dependency has no " + q(k) + ": " + why);
                ok = false;
                continue;
            }
            if (x.kind != toml::Kind::string) {
                diags_.error(span(id, x), q(k) + " must be a string, in quotes, not " + toml::kind_name(x.kind));
                ok = false;
                continue;
            }
            if (x.string.empty()) {
                diags_.error(span(id, x), q(k) + " can't be empty");
                ok = false;
                continue;
            }
            if (k == "path") path = &x;
            else if (k == "git") git = &x;
            else if (ref) {
                diags_.error(key_span(id, x), q(name) + " gives both `" + ref_key_ + "` and " + q(k) + "; give one, to say which commit");
                ok = false;
            } else {
                ref = &x;
                ref_key_ = k;
            }
        }
        if (!ok) return;
        if (path && git) {
            diags_.error(d.key, q(name) + " gives both a `path` and a `git` URL; give one, to say where it is");
            return;
        }
        if (!path && !git) {
            diags_.error(d.key, q(name) + " needs a `path` or a `git` URL, to say where it is: " + name + " = { path = \"../" + name +
                                    "\" }, or " + name + " = { git = \"https://...\", tag = \"v1.0.0\" }");
            return;
        }
        if (path && ref) {
            diags_.error(key_span(id, *ref), q(ref_key_) + " is for a git dependency, and " + q(name) + " is found by its path");
            return;
        }
        if (path) {
            d.path = path->string;
            d.where = span(id, *path);
        } else {
            d.git = true;
            d.url = git->string;
            d.where = span(id, *git);
            if (d.url[0] == '-') {
                diags_.error(d.where, "a git URL can't start with `-`");
                return;
            }
            if (ref) {
                d.ref = ref->string;
                d.ref_at = span(id, *ref);
                d.ref_kind = ref_key_ == "tag" ? RefKind::tag : ref_key_ == "rev" ? RefKind::rev : RefKind::branch;
                if (d.ref_kind == RefKind::rev) {
                    bool hex = d.ref.size() >= 4 && d.ref.size() <= 40;
                    for (char& c : d.ref) {
                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        hex = hex && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
                    }
                    if (!hex) {
                        diags_.error(d.ref_at, "a `rev` is a commit's hash, or at least its first 4 digits; for a tag or a branch, "
                                               "give `tag` or `branch`");
                        return;
                    }
                } else {
                    bool good = d.ref[0] != '-';
                    for (char c : d.ref) good = good && static_cast<unsigned char>(c) > ' ' && c != 0x7f && c != '~' && c != '^' && c != ':';
                    if (!good) {
                        diags_.error(d.ref_at, q(d.ref) + " can't be a git " + ref_key_ + "'s name");
                        return;
                    }
                }
            }
        }
        m.deps.push_back(std::move(d));
    }

    // Where `d`, which package `from` depends on, is, and its package's index: one already found
    // from the same place, or a new one. -1 after an error.
    int add(std::size_t from, const DepSpec& d) {
        Node n;
        std::string source;
        if (!d.git) {
            fs::path dir = (nodes_[from].dir / to_path(d.path)).lexically_normal();
            if (!nodes_[from].repo.empty() && !within(dir, nodes_[from].repo)) {
                diags_.error(d.where, q(d.name) + "'s path leaves the git repository " + q(out_.packages[from].name) +
                                          " comes from; a path dependency of a package from git is in its repository");
                return -1;
            }
            if (!is_dir(dir)) {
                diags_.error(d.where, q(d.path) + ", the path given for " + q(d.name) + ", isn't a folder" +
                                          (from_path(dir) != d.path ? ": it's " + q(display(dir)) : ""));
                return -1;
            }
            if (!is_file(dir / "calx.toml")) {
                diags_.error(d.where, q(display(dir)) + " has no calx.toml, so it isn't a package");
                return -1;
            }
            n.dir = dir;
            n.repo = nodes_[from].repo;
            n.identity = "path:" + identity(dir);
            source = display(dir);
        } else {
            n.identity.clear();
        }
        if (auto it = by_name_.find(d.name); it != by_name_.end() && !d.git) {
            if (nodes_[it->second].identity != n.identity) {
                conflict(from, d, it->second, source);
                return -1;
            }
            return static_cast<int>(it->second);
        }
        if (d.git) {
            // The commit to pin: what calx.lock says, when it says it for the same URL and ref.
            std::string locked;
            for (const LockEntry& e : lock_) {
                if (e.name == d.name && e.url == d.url && e.ref_kind == d.ref_kind && e.ref == d.ref) locked = e.commit;
            }
            fs::path checkout;
            std::string commit;
            // One already found from the same URL and ref is the same package, whose commit
            // was pinned then.
            for (const auto& [key, at] : pinned_) {
                if (key == d.url + "\n" + ref_word(d.ref_kind) + "\n" + d.ref) commit = at;
            }
            if (commit.empty()) {
                commit = git_.pin(d, locked, checkout);
                if (commit.empty()) return -1;
                pinned_.emplace_back(d.url + "\n" + ref_word(d.ref_kind) + "\n" + d.ref, commit);
                lock_out_.push_back(LockEntry{d.name, d.url, d.ref, commit, d.ref_kind});
            }
            n.identity = "git:" + d.url + "@" + commit;
            source = d.url + (d.ref_kind == RefKind::none ? "" : " at " + std::string(ref_word(d.ref_kind)) + " " + q(d.ref)) + " (" +
                     commit.substr(0, 12) + ")";
            if (auto it = by_name_.find(d.name); it != by_name_.end()) {
                if (nodes_[it->second].identity != n.identity) {
                    conflict(from, d, it->second, source);
                    return -1;
                }
                return static_cast<int>(it->second);
            }
            if (checkout.empty()) checkout = nodes_[by_identity_.at(n.identity)].dir;
            n.dir = checkout;
            n.repo = checkout;
            if (!is_file(checkout / "calx.toml")) {
                diags_.error(d.where, d.url + " has no calx.toml at its root, so it isn't a package");
                return -1;
            }
        }
        if (!manifest(display(n.dir / "calx.toml"), d.where, n.m)) return -1;
        if (n.m.name != d.name) {
            diags_.error(d.key, q(source) + " is the package " + q(n.m.name) + ", so its name here is " + q(n.m.name) +
                                    ": a package is imported by its own name");
            return -1;
        }
        n.declared_by = out_.packages[from].name;
        n.declared = d.key;
        Package p;
        p.name = n.m.name;
        p.version = n.m.version;
        p.dir = display(n.dir);
        p.source = source;
        std::size_t index = nodes_.size();
        by_name_[p.name] = index;
        by_identity_[n.identity] = index;
        out_.packages.push_back(std::move(p));
        nodes_.push_back(std::move(n));
        return static_cast<int>(index);
    }

    void conflict(std::size_t from, const DepSpec& d, std::size_t other, const std::string& source) {
        const Package& o = out_.packages[other];
        std::string here = out_.packages[from].name;
        std::string there = other == 0 ? "this package itself" : q(o.source) + ", from " + q(nodes_[other].declared_by) + "'s calx.toml";
        diags_.error(d.key, "two packages are named " + q(d.name) + ": " + q(source) + ", from " + q(here) + "'s calx.toml, and " + there +
                                (o.version.empty() ? "" : ", version " + o.version) +
                                "; a program has one package of each name, so point both at the same one");
        if (other != 0 && nodes_[other].declared.file) diags_.note(nodes_[other].declared, "the other " + q(d.name) + " is listed here");
    }

    // A package that depends on itself through others is an error at the dependency that closes
    // the cycle.
    void cycles() {
        std::vector<int> state(nodes_.size(), 0);
        std::vector<std::size_t> stack;
        bool reported = false;
        auto visit = [&](auto& self, std::size_t i) -> void {
            state[i] = 1;
            stack.push_back(i);
            for (const DepSpec& d : nodes_[i].m.deps) {
                auto it = out_.packages[i].deps.find(d.name);
                if (it == out_.packages[i].deps.end()) continue;
                std::size_t j = it->second;
                if (state[j] == 1 && !reported) {
                    reported = true;
                    std::size_t k = 0;
                    while (stack[k] != j) k++;
                    std::string chain = q(out_.packages[stack[k]].name);
                    for (std::size_t s = k + 1; s < stack.size(); s++) {
                        chain += (s == k + 1 ? " depends on " : ", which depends on ") + q(out_.packages[stack[s]].name);
                    }
                    chain += (stack.size() - k == 1 ? " depends on " : ", which depends on ") + q(out_.packages[j].name);
                    diags_.error(d.key, "packages can't depend on each other in a cycle: " + chain);
                } else if (state[j] == 0) {
                    self(self, j);
                }
            }
            stack.pop_back();
            state[i] = 2;
        };
        visit(visit, 0);
    }

    bool read_lock(const std::string& path) {
        if (!is_file(to_path(path))) return true;
        toml::Value root;
        std::uint32_t id = read_toml(sources_, diags_, path, root, Span{});
        std::size_t errors = diags_.error_count();
        if (id == 0) {
            diags_.note(Span{}, "calx.lock is written by calx, so delete it to have it written again");
            return false;
        }
        lock_text_ = sources_.file(id).text;
        lock_had_ = true;
        auto bad = [&](Span at, const std::string& what) {
            diags_.error(at, what + "; calx.lock is written by calx, so delete it to have it written again");
        };
        const toml::Value* version = root.get("version");
        if (!version || version->kind != toml::Kind::integer || version->integer != 1) {
            bad(version ? span(id, *version) : Span{id, 0, 0}, "calx.lock needs `version = 1`, the form this calx reads");
            return false;
        }
        for (const toml::Value& v : root.entries) {
            const std::string& k = v.key;
            if (k == "version") continue;
            if (k != "git" || v.kind != toml::Kind::array) {
                bad(key_span(id, v), "calx.lock has no " + q(k));
                continue;
            }
            for (const toml::Value& e : v.items) {
                if (e.kind != toml::Kind::table) {
                    bad(span(id, e), "each [[git]] in calx.lock is a table");
                    continue;
                }
                LockEntry l;
                for (const toml::Value& ev : e.entries) {
                    const std::string& ek = ev.key;
                    bool known = ek == "name" || ek == "url" || ek == "commit" || ek == "tag" || ek == "rev" || ek == "branch";
                    if (!known || ev.kind != toml::Kind::string) {
                        bad(key_span(id, ev), known ? q(ek) + " must be a string" : "a [[git]] in calx.lock has no " + q(ek));
                        continue;
                    }
                    if (ek == "name") l.name = ev.string;
                    else if (ek == "url") l.url = ev.string;
                    else if (ek == "commit") l.commit = ev.string;
                    else {
                        l.ref = ev.string;
                        l.ref_kind = ek == "tag" ? RefKind::tag : ek == "rev" ? RefKind::rev : RefKind::branch;
                    }
                }
                bool hex = l.commit.size() == 40;
                for (char c : l.commit) hex = hex && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
                if (l.name.empty() || l.url.empty() || !hex) {
                    bad(span(id, e), "each [[git]] in calx.lock has a `name`, a `url` and a `commit` of 40 hex digits");
                    continue;
                }
                lock_.push_back(std::move(l));
            }
        }
        return diags_.error_count() == errors;
    }

    void write_lock() {
        if (lock_out_.empty() && !lock_had_) return;
        std::sort(lock_out_.begin(), lock_out_.end(), [](const LockEntry& a, const LockEntry& b) { return a.name < b.name; });
        std::string text =
            "# calx.lock: the commit each git dependency is pinned to, which calx writes. Keep it with\n"
            "# calx.toml, so each build of this package uses the same code. To move a dependency to\n"
            "# another commit, change its tag, rev or branch in calx.toml, or take its entry out of here.\n"
            "version = 1\n";
        for (const LockEntry& e : lock_out_) {
            text += "\n[[git]]\nname = " + toml::quote(e.name) + "\nurl = " + toml::quote(e.url) + "\n";
            if (e.ref_kind != RefKind::none) text += std::string(ref_word(e.ref_kind)) + " = " + toml::quote(e.ref) + "\n";
            text += "commit = " + toml::quote(e.commit) + "\n";
        }
        // Each line as the file has them, `\r\n` if it was written so, so a checkout on Windows
        // isn't changed by a build.
        if (lock_text_.find("\r\n") != std::string::npos) {
            std::string crlf;
            for (char c : text) crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
            text = std::move(crlf);
        }
        if (lock_had_ && text == lock_text_) return;
        if (!write_file_atomic(out_.lock, text)) {
            diags_.warning(Span{}, "can't write " + q(out_.lock));
            return;
        }
        out_.written.push_back(out_.lock);
    }

    Sources& sources_;
    Diagnostics& diags_;
    PackageSet& out_;
    Git git_;
    std::vector<Node> nodes_;
    std::map<std::string, std::size_t> by_name_;
    std::map<std::string, std::size_t> by_identity_;
    std::vector<std::pair<std::string, std::string>> pinned_;  // `URL\nkind\nref`, and its commit
    std::vector<LockEntry> lock_;
    std::vector<LockEntry> lock_out_;
    std::string lock_text_;
    bool lock_had_ = false;
    std::string ref_key_;
};

}  // namespace

void load_packages(Sources& sources, Diagnostics& diags, const std::string& from, PackageSet& out) {
    out = PackageSet{};
    std::error_code ec;
    fs::path dir = fs::absolute(to_path(from.empty() ? "." : from), ec).lexically_normal();
    if (ec) return;
    for (;;) {
        fs::path toml = dir / "calx.toml";
        if (is_file(toml)) {
            Resolver(sources, diags, out).run(toml);
            return;
        }
        out.missing.push_back(from_path(toml));
        fs::path up = dir.parent_path();
        if (up == dir || up.empty()) return;
        dir = up;
    }
}

}  // namespace calx
