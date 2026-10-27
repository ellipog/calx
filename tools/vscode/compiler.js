// Talking to the calx compiler: finding it, and reading the diagnostics `calx check --json`
// prints. Nothing here uses the VS Code API, so test/run.mjs tests it with plain Node.
'use strict';

const fs = require('fs');
const path = require('path');

// One diagnostic per line, as docs/reference/command.md describes. Anything else is skipped.
function parseDiagnostics(stdout) {
    const out = [];
    for (const line of stdout.split(/\r?\n/)) {
        if (!line.startsWith('{')) continue;
        let d;
        try {
            d = JSON.parse(line);
        } catch {
            continue;
        }
        if (typeof d.message === 'string' && d.range && d.range.start && d.range.end) out.push(d);
    }
    return out;
}

// Whether two paths name the same file. On Windows, case and slash direction don't matter.
function samePath(a, b, platform = process.platform) {
    const p = platform === 'win32' ? path.win32 : path.posix;
    const norm = (s) => {
        const r = p.resolve(s);
        return platform === 'win32' ? r.toLowerCase() : r;
    };
    return norm(a) === norm(b);
}

// The diagnostics that belong to `file`. Those without a file are about the whole check, so they
// go on it too.
function diagnosticsFor(diags, file, platform = process.platform) {
    return diags.filter((d) => d.file === null || d.file === undefined || samePath(d.file, file, platform));
}

// Where the compiler is. A configured path wins. Otherwise it's the one tests/run.py builds in a
// checkout of the calx repo: one containing a start folder, or next to it as D38 lays the repos
// out. Then `$CALX_HOME`, then `calx` on PATH.
function findCompiler({ configured, starts, env = process.env, platform = process.platform, exists = fs.existsSync }) {
    const p = platform === 'win32' ? path.win32 : path.posix;
    if (configured) return configured;
    const exe = platform === 'win32' ? 'calx.exe' : 'calx';
    const tail = p.join('build', 'compiler', platform === 'win32' ? 'windows' : 'linux', exe);
    for (const start of starts) {
        for (let dir = start; ; dir = p.dirname(dir)) {
            for (const candidate of [p.join(dir, tail), p.join(dir, 'calx', tail)]) {
                if (exists(candidate)) return candidate;
            }
            if (p.dirname(dir) === dir) break;
        }
    }
    if (env.CALX_HOME) {
        const candidate = p.join(env.CALX_HOME, tail);
        if (exists(candidate)) return candidate;
    }
    return 'calx';
}

module.exports = { parseDiagnostics, samePath, diagnosticsFor, findCompiler };
