// Tests for the VS Code extension: the grammar's scopes on chosen lines, every Calx example in the
// repo tokenized without an `invalid` scope, and the helpers in compiler.js.
//
//     npm ci && npm test        (in tools/vscode)

import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const require = createRequire(import.meta.url);
const vsctm = require('vscode-textmate');
const oniguruma = require('vscode-oniguruma');
const { parseDiagnostics, samePath, diagnosticsFor, findCompiler } = require('../compiler.js');

const HERE = path.dirname(fileURLToPath(import.meta.url));
const EXT = path.resolve(HERE, '..');
const ROOT = path.resolve(EXT, '..', '..');

let failures = 0;
let passed = 0;
function test(name, body) {
    try {
        body();
        passed++;
    } catch (err) {
        failures++;
        console.error(`FAIL ${name}\n  ${String(err.message).split('\n').join('\n  ')}`);
    }
}

// --- the grammar ----------------------------------------------------------------------------------

const wasm = fs.readFileSync(require.resolve('vscode-oniguruma/release/onig.wasm')).buffer;
await oniguruma.loadWASM(wasm);
const registry = new vsctm.Registry({
    onigLib: Promise.resolve({
        createOnigScanner: (patterns) => new oniguruma.OnigScanner(patterns),
        createOnigString: (s) => new oniguruma.OnigString(s),
    }),
    loadGrammar: async (scope) => {
        const files = {
            'source.calx': 'calx.tmLanguage.json',
            'markdown.calx.codeblock': 'calx.markdown.tmLanguage.json',
        };
        if (!files[scope]) return null;
        const file = path.join(EXT, 'syntaxes', files[scope]);
        return vsctm.parseRawGrammar(fs.readFileSync(file, 'utf8'), file);
    },
});
const grammar = await registry.loadGrammar('source.calx');
await registry.loadGrammar('markdown.calx.codeblock');  // it must at least parse

// The tokens of each line of `source`, as [text, scopes] pairs.
function tokenize(source) {
    let state = vsctm.INITIAL;
    return source.split(/\r?\n/).map((line) => {
        const r = grammar.tokenizeLine(line, state);
        state = r.ruleStack;
        return r.tokens.map((t) => [line.slice(t.startIndex, t.endIndex), t.scopes]);
    });
}

// Checks that `text`, the `nth` time it's a whole token on the line, has a scope starting with `scope`.
function expectScope(line, text, scope, nth = 0) {
    const tokens = tokenize(line)[0];
    const matches = tokens.filter(([t]) => t === text);
    assert.ok(matches.length > nth, `no token ${JSON.stringify(text)} in ${JSON.stringify(line)}: ` +
        JSON.stringify(tokens.map(([t]) => t)));
    const scopes = matches[nth][1];
    assert.ok(scopes.some((s) => s.startsWith(scope)),
        `${JSON.stringify(text)} in ${JSON.stringify(line)} has ${scopes.join(' ')}, not ${scope}`);
}

const cases = [
    ['fn main() {', 'fn', 'storage.type.function'],
    ['fn main() {', 'main', 'entity.name.function.calx'],
    ['struct Point {', 'Point', 'entity.name.type'],
    ['enum Flag: u8 { A = 1 }', 'enum', 'storage.type.enum'],
    ['enum Flag: u8 { A = 1 }', 'u8', 'support.type.primitive'],
    ['impl Point {', 'impl', 'storage.type'],
    ['let x: i32 = 1', 'let', 'storage.type'],
    ['var total = 0', 'var', 'storage.type'],
    ['let x: i32 = 1', 'i32', 'support.type.primitive'],
    ['let p = Point { x: 1 }', 'Point', 'entity.name.type'],
    ['if a and not b { return }', 'if', 'keyword.control'],
    ['if a and not b { return }', 'and', 'keyword.operator.wordlike'],
    ['if a and not b { return }', 'not', 'keyword.operator.wordlike'],
    ['if a and not b { return }', 'return', 'keyword.control'],
    ['match shape { Circle(r) => r, _ => 0 }', 'match', 'keyword.control'],
    ['match shape { Circle(r) => r, _ => 0 }', 'Circle', 'entity.name.type'],
    ['match shape { Circle(r) => r, _ => 0 }', '=>', 'keyword.operator.arrow'],
    ['for i in 0..10 {', 'in', 'keyword.control'],
    ['for i in 0..10 {', '..', 'keyword.operator.range'],
    ['for i in 0..=10 {', '..=', 'keyword.operator.range'],
    ['for i in 0..10 {', '0', 'constant.numeric'],
    ['let n = 255u8', '255', 'constant.numeric'],
    ['let n = 255u8', 'u8', 'storage.type.numeric'],
    ['let n = 0xFF_FFi64', '0xFF_FF', 'constant.numeric'],
    ['let n = 0xFF_FFi64', 'i64', 'storage.type.numeric'],
    ['let f = 1.5e-3f32', '1.5e-3', 'constant.numeric'],
    ['let f = 1.5e-3f32', 'f32', 'storage.type.numeric'],
    ['let b = 0b1010_0101', '0b1010_0101', 'constant.numeric'],
    ['let x = t.0', '0', 'constant.numeric'],
    ['let c = \'a\'', 'a', 'string.quoted.single'],
    ['let c = \'\\u{1F600}\'', '\\u{1F600}', 'constant.character.escape'],
    ['let o: i32? = none', 'none', 'constant.language.none'],
    ['let o: i32? = none', '?', 'keyword.operator.optional'],
    ['let v = o ?? 0', '??', 'keyword.operator.optional'],
    ['let v = p?.x', '?.', 'keyword.operator.optional'],
    ['let v = o!', '!', 'keyword.operator.unwrap'],
    ['let ok = true', 'true', 'constant.language.boolean'],
    ['self.x = Self.make()', 'self', 'variable.language.self'],
    ['self.x = Self.make()', 'Self', 'variable.language.self'],
    ['self.x = Self.make()', 'make', 'entity.name.function.call'],
    ['println("x is {x + 1}")', 'println', 'support.function.builtin'],
    ['println("x is {x + 1}")', 'x is ', 'string.quoted.double'],
    ['println("x is {x + 1}")', '{', 'punctuation.section.interpolation.begin'],
    ['println("x is {x + 1}")', 'x', 'variable.other', 0],
    ['println("x is {x + 1}")', '1', 'constant.numeric'],
    ['println("tab\\t\\{brace\\}")', '\\t', 'constant.character.escape'],
    ['println("tab\\t\\{brace\\}")', '\\{', 'constant.character.escape'],
    ['println("bad \\q")', '\\q', 'invalid.illegal.escape'],
    ['println("{if a { 1 } else { 2 }}")', 'else', 'keyword.control'],
    ['let s = t"hello"', 't', 'storage.type.string'],
    ['let x = some(3)', 'some', 'support.function.builtin'],
    ['let y = i8.try_from(x)', 'i8', 'support.type.primitive'],
    ['let y = i8.try_from(x)', 'try_from', 'entity.name.function.call'],
    ['let m = i32.MAX', 'MAX', 'variable.other.constant'],
    ['const CHUNK_SIZE: i32 = 16', 'const', 'storage.type'],
    ['const CHUNK_SIZE: i32 = 16', 'CHUNK_SIZE', 'variable.other.constant'],
    ['x >>>= 3', '>>>=', 'keyword.operator.assignment.compound'],
    ['let y = x >>> 3', '>>>', 'keyword.operator.bitwise.shift'],
    ['if a <= b {', '<=', 'keyword.operator.comparison'],
    ['if a == b {', '==', 'keyword.operator.comparison'],
    ['fn f(mut p: Point, own q: Point) -> i32 {', 'mut', 'storage.modifier'],
    ['fn f(mut p: Point, own q: Point) -> i32 {', 'own', 'storage.modifier'],
    ['fn f(mut p: Point, own q: Point) -> i32 {', '->', 'keyword.operator.arrow'],
    ['pub fn area(self) -> f64 {', 'pub', 'storage.modifier'],
    ['import std.math.{sin, cos}', 'import', 'keyword.other.import'],
    ['import std.math.{sin, cos}', 'std.math', 'entity.name.namespace'],
    ['module example.basics', 'example.basics', 'entity.name.namespace'],
    ['@packet(id: 3)', 'packet', 'entity.name.function.decorator'],
    ['/// Doc with `code`.', '`code`', 'markup.inline.raw'],
    ['/// Doc with `code`.', '///', 'punctuation.definition.comment'],
    ['//// not a doc comment', '//', 'punctuation.definition.comment'],
    ['let x = 1 // a comment', ' a comment', 'comment.line.double-slash'],
];
for (const [line, text, scope, nth] of cases) {
    test(`grammar: ${text} in ${line}`, () => expectScope(line, text, scope, nth));
}

test('grammar: four slashes are an ordinary comment', () => {
    const scopes = tokenize('//// plain')[0].flatMap(([, s]) => s);
    assert.ok(!scopes.some((s) => s.startsWith('comment.line.documentation')));
});

test('grammar: a string ends at the end of its line', () => {
    const lines = tokenize('let s = "open\nlet t = 1');
    const scopes = lines[1].flatMap(([, s]) => s);
    assert.ok(!scopes.some((s) => s.startsWith('string')), scopes.join(' '));
});

// Every example in the repo tokenizes without an `invalid` scope, except on lines whose test
// expects an error.
function* calxSources() {
    function* walk(dir) {
        for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
            const p = path.join(dir, entry.name);
            if (entry.isDirectory()) yield* walk(p);
            else yield p;
        }
    }
    for (const file of walk(path.join(ROOT, 'tests'))) {
        if (file.endsWith('.calx')) yield [path.relative(ROOT, file), fs.readFileSync(file, 'utf8')];
    }
    for (const dir of ['reference', 'guide']) {
        const docs = path.join(ROOT, 'docs', dir);
        if (!fs.existsSync(docs)) continue;
        for (const file of walk(docs)) {
            if (!file.endsWith('.md')) continue;
            const text = fs.readFileSync(file, 'utf8');
            let n = 0;
            for (const m of text.matchAll(/^```calx[^\n]*\n([\s\S]*?)^```/gm)) {
                yield [`${path.relative(ROOT, file)} block ${++n}`, m[1]];
            }
        }
    }
}

let sources = 0;
for (const [name, source] of calxSources()) {
    sources++;
    test(`grammar: ${name} has no invalid tokens`, () => {
        const lines = source.split(/\r?\n/);
        tokenize(source).forEach((tokens, i) => {
            if (/\/\/\s*error:/.test(lines[i])) return;
            for (const [text, scopes] of tokens) {
                const bad = scopes.find((s) => s.startsWith('invalid'));
                assert.ok(!bad, `line ${i + 1}: ${JSON.stringify(text)} is ${bad}`);
            }
        });
    });
}
assert.ok(sources > 10, 'found too few examples; is ROOT right?');

// --- compiler.js ----------------------------------------------------------------------------------

test('parseDiagnostics reads JSON lines and skips the rest', () => {
    const out = [
        '{"severity":"error","file":"a.calx","range":{"start":{"line":1,"character":4},"end":{"line":1,"character":9}},"message":"unknown name `y`","notes":[]}',
        'not json',
        '{"broken": ',
        '{"severity":"error"}',
        '',
    ].join('\r\n');
    const d = parseDiagnostics(out);
    assert.equal(d.length, 1);
    assert.equal(d[0].message, 'unknown name `y`');
    assert.equal(d[0].range.start.character, 4);
});

test('samePath ignores case and slashes on Windows only', () => {
    assert.ok(samePath('C:\\Work\\a.calx', 'c:/work/A.calx', 'win32'));
    assert.ok(!samePath('/work/a.calx', '/work/A.calx', 'linux'));
    assert.ok(samePath('/work/x/../a.calx', '/work/a.calx', 'linux'));
});

test('diagnosticsFor keeps the file\'s diagnostics and those without a file', () => {
    const range = { start: { line: 0, character: 0 }, end: { line: 0, character: 1 } };
    const diags = [
        { file: '/w/a.calx', range, message: 'here' },
        { file: '/w/b.calx', range, message: 'elsewhere' },
        { file: null, range, message: 'anywhere' },
    ];
    assert.deepEqual(diagnosticsFor(diags, '/w/a.calx', 'linux').map((d) => d.message), ['here', 'anywhere']);
});

test('findCompiler prefers the setting, then a checkout, then CALX_HOME, then PATH', () => {
    const have = new Set();
    const exists = (p) => have.has(p);
    const opts = { starts: ['/home/e/mc/calcite/src'], env: {}, platform: 'linux', exists };
    assert.equal(findCompiler({ ...opts, configured: '/opt/calx' }), '/opt/calx');
    assert.equal(findCompiler(opts), 'calx');
    have.add('/home/e/calx-home/build/compiler/linux/calx');
    assert.equal(findCompiler({ ...opts, env: { CALX_HOME: '/home/e/calx-home' } }), '/home/e/calx-home/build/compiler/linux/calx');
    // Next to the calcite repo, as D38 lays them out.
    have.add('/home/e/mc/calx/build/compiler/linux/calx');
    assert.equal(findCompiler(opts), '/home/e/mc/calx/build/compiler/linux/calx');
    // Inside the calx repo itself.
    have.add('/home/e/mc/calcite/build/compiler/linux/calx');
    assert.equal(findCompiler(opts), '/home/e/mc/calcite/build/compiler/linux/calx');
});

test('findCompiler uses calx.exe and the windows folder on Windows', () => {
    const want = 'C:\\mc\\calx\\build\\compiler\\windows\\calx.exe';
    const found = findCompiler({ starts: ['C:\\mc\\calx\\tests'], env: {}, platform: 'win32', exists: (p) => p === want });
    assert.equal(found, want);
});

console.log(`${passed} passed, ${failures} failed (${sources} Calx examples tokenized)`);
process.exit(failures ? 1 : 0);
