"""The reference side of tools/html/check.calx: Python's html.parser.

    python -I reference.py [DIR]

Each line of input is an HTML text as a JSON string, and each line of output is what
`html.parser.HTMLParser` reads in it, fed whole and then closed, as a JSON array of tokens:
`["start", name, [[name, value or null], ...], self_closing]`, `["end", name]`, `["text", text]`,
`["comment", text]`, `["decl", text]`, `["pi", text]` and `["cdata", text]`. Text next to text is
joined, and empty text left out, as std.html's `tokens` gives them.

std.html reads HTML as Python 3.14 does, whose parser changed a good deal in 2025. DIR, if given,
holds `parser.py` and `_markupbase.py` from CPython 3.14's Lib (Lib/html/parser.py and
Lib/_markupbase.py), which this loads in place of the installed ones, for a Python older than 3.14.
"""
import importlib.util
import json
import sys


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    with open(path, encoding="utf-8") as f:
        source = f.read()
    if sys.version_info < (3, 14):
        # 3.14's `re` has `\z`, the end of the text, which before it was only `\Z`.
        source = source.replace(r"re.compile(r'\z')", r"re.compile(r'\Z')")
    exec(compile(source, path, "exec"), module.__dict__)
    return module


if len(sys.argv) > 1:
    load("_markupbase", sys.argv[1] + "/_markupbase.py")
    HTMLParser = load("html_parser_314", sys.argv[1] + "/parser.py").HTMLParser
else:
    if sys.version_info < (3, 14):
        sys.stderr.write("reference.py: Python %d.%d's html.parser isn't 3.14's; give a folder with 3.14's\n" % sys.version_info[:2])
    from html.parser import HTMLParser


class Tokens(HTMLParser):
    def __init__(self):
        super().__init__()
        self.out = []

    def handle_starttag(self, tag, attrs):
        self.out.append(["start", tag, [[n, v] for n, v in attrs], False])

    def handle_startendtag(self, tag, attrs):
        self.out.append(["start", tag, [[n, v] for n, v in attrs], True])

    def handle_endtag(self, tag):
        self.out.append(["end", tag])

    def handle_data(self, data):
        if not data:
            return
        if self.out and self.out[-1][0] == "text":
            self.out[-1][1] += data
        else:
            self.out.append(["text", data])

    def handle_comment(self, data):
        self.out.append(["comment", data])

    def handle_decl(self, decl):
        self.out.append(["decl", decl])

    def handle_pi(self, data):
        self.out.append(["pi", data])

    def unknown_decl(self, data):
        # Only a CDATA section comes here, as "CDATA[" and what's in it.
        assert data.startswith("CDATA["), data
        self.out.append(["cdata", data[6:]])


for line in sys.stdin.buffer:
    p = Tokens()
    p.feed(json.loads(line.decode("utf-8")))
    p.close()
    sys.stdout.buffer.write(json.dumps(p.out, ensure_ascii=False, separators=(",", ":")).encode("utf-8") + b"\n")
