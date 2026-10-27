#include "modules.h"

#include <cctype>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace calx {

namespace fs = std::filesystem;

namespace {

std::string q(std::string_view s) { return "`" + std::string(s) + "`"; }

class Loader {
public:
    Loader(ParseContext ctx, Sources& sources) : ctx_(ctx), sources_(sources) {}

    std::vector<Module*> run(std::uint32_t main_file) {
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
        by_name_[dotted(m->name.parts)] = m;
        visit(m);
        return order_;
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
            Module* target = find(it);
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

    Module* find(Item* imp) {
        const Slice<Name>& parts = imp->import_path.parts;
        std::string name = dotted(parts);
        if (auto it = by_name_.find(name); it != by_name_.end()) return it->second;
        fs::path file = root_;
        for (Name p : parts) file /= std::string(text(p));
        file += ".calx";
        std::uint32_t id = exact_case(parts) ? sources_.load(file.generic_string()) : 0;
        if (id == 0) {
            ctx_.diags.error(imp->import_path.span, "there's no module " + q(name) + "; it would be the file " +
                                                        q(file.generic_string()));
            by_name_[name] = nullptr;
            return nullptr;
        }
        Module* m = parse_file(ctx_, id);
        // A module in a folder says so, so that any file can be checked by itself, as editors
        // do, and still find the root.
        if (m->name.parts.empty() && parts.size() > 1) {
            Span at{id, 0, 0};
            if (!m->items.empty()) at = m->items[0]->name_span.end > m->items[0]->name_span.begin ? m->items[0]->name_span : m->items[0]->span;
            ctx_.diags.error(at, "a module in a folder starts with its `module` line: " + q("module " + name));
        }
        if (!m->name.parts.empty() && dotted(m->name.parts) != name) {
            ctx_.diags.error(m->name.span, "this file is module " + q(name) + " by its place, so its `module` line must say " +
                                               q("module " + name) + ", or be left out");
        }
        m->name.parts = parts;
        by_name_[name] = m;
        return m;
    }

    // Whether the folders and file a module names exist with exactly those letters, so `import
    // Util` finds `util.calx` on neither Windows nor Linux.
    bool exact_case(const Slice<Name>& parts) const {
        fs::path dir = root_.empty() ? fs::path(".") : root_;
        for (std::size_t i = 0; i < parts.size(); i++) {
            std::string want = std::string(text(parts[i])) + (i + 1 == parts.size() ? ".calx" : "");
            bool found = false;
            std::error_code ec;
            for (fs::directory_iterator d(dir, ec), end; !ec && d != end; d.increment(ec)) {
                if (d->path().filename().string() == want) {
                    found = true;
                    break;
                }
            }
            if (!found) return false;
            dir /= want;
        }
        return true;
    }

    ParseContext ctx_;
    Sources& sources_;
    fs::path root_;
    std::unordered_map<std::string, Module*> by_name_;
    std::unordered_map<Module*, int> state_;  // 1 while visiting, 2 once done
    std::vector<Module*> stack_;
    std::vector<Module*> order_;
};

}  // namespace

std::vector<Module*> load_program(ParseContext ctx, Sources& sources, std::uint32_t main_file) {
    return Loader(ctx, sources).run(main_file);
}

}  // namespace calx
