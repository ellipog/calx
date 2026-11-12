// The measurements the site quotes: the tick loop's, the build times' and the scripting
// benchmarks', read from the "## Performance" section of the repo's README, and the language
// benchmarks', read from bench/results/suite-*.json. Each is recorded with the change that moves
// it, so the site never has numbers of its own to fall behind.
import { readFileSync } from 'node:fs';
import { REPO } from './paths.mjs';

const README = new URL('README.md', REPO);

function section() {
	const text = readFileSync(README, 'utf8').replace(/\r\n/g, '\n');
	const body = text.split(/^## Performance\n/m)[1]?.split(/^## /m)[0];
	if (!body) throw new Error('README.md has no "## Performance" section');
	return body;
}

/** The section's paragraphs and tables, as blocks of text. */
function blocks() {
	return section().split(/\n\n+/);
}

/** The index in blocks() of the table whose header row matches `head`, so the README can put its
 *  tables in any order. */
function tableAt(all, head, what) {
	const i = all.findIndex((b) => b.startsWith('|') && head.test(b.split('\n')[0]));
	if (i < 0) throw new Error(`README.md's performance section has no ${what} table`);
	return i;
}

/** A Markdown table's rows after the header, as arrays of cells without ** or backticks. */
function rows(table) {
	return table
		.split('\n')
		.filter((l) => l.startsWith('|'))
		.slice(2)
		.map((l) =>
			l
				.slice(1, -1)
				.split('|')
				.map((c) => c.trim().replace(/\*\*|`/g, ''))
		);
}

const num = (s) => Number(s.replace(/[^\d.]/g, ''));

/** The tick-loop benchmark: ms per tick for each platform, and how it was measured, which the
 *  paragraph before its table says. */
export function tickLoop() {
	const all = blocks();
	const at = tableAt(all, /^\| Platform \| Calx, 1 worker/, 'tick-loop');
	const prose = all[at - 1];
	const platforms = rows(all[at]).map(([platform, calx1, rust1, calx24, rust24]) => ({
		platform,
		calx1: num(calx1),
		rust1: num(rust1),
		calx24: num(calx24),
		rust24: num(rust24),
	}));
	const pick = (re, what) => {
		const m = prose.match(re);
		if (!m) throw new Error(`README.md's tick-loop paragraph doesn't give ${what}`);
		return m[1];
	};
	const ratios = platforms.flatMap((p) => [p.calx1 / p.rust1, p.calx24 / p.rust24]);
	return {
		platforms,
		machine: pick(/measured on an? ([^.]+)\./i, 'the machine'),
		runs: pick(/median of (\d+) runs/i, 'the number of runs'),
		ticks: pick(/runs of ([\d,]+) ticks/i, 'the number of ticks'),
		rustc: pick(/rustc ([\d.]+\d)/, 'the rustc version'),
		clang: pick(/Calx with clang (\d+)/, 'the clang version'),
		// "About 0.7× Rust's time": every ratio, to one decimal.
		ratio: (ratios.reduce((a, b) => a + b, 0) / ratios.length).toFixed(1),
	};
}

/** The build-time table's row whose label starts with `label`: { label, windows, linux }, the
 *  times as written, such as "32 ms". */
export function buildTime(label) {
	const row = buildTimes().find((r) => r.label.startsWith(label));
	if (!row) throw new Error(`README.md's build times have no row starting "${label}"`);
	return row;
}

/** Every row of the build-time table, in order. */
export function buildTimes() {
	const all = blocks();
	const list = rows(all[tableAt(all, /^\| *\| Windows 11 \| Linux/, 'build-time')]).map((r) => ({ label: r[0], windows: r[1], linux: r[2] }));
	if (list.length < 3 || list.some((r) => !/\d m?s$/.test(r.windows))) throw new Error("README.md's build-time table isn't label, Windows, Linux");
	return list;
}

/** The scripting benchmarks' table: each benchmark's summed best times in Calx, Rust and Python on
 *  each platform, as written ("6.50 ms") and as numbers. A row with no label is the platform below
 *  the one before it. */
export function scripting() {
	const all = blocks();
	const list = [];
	for (const [label, platform, ...times] of rows(all[tableAt(all, /^\| *\| Platform \| Calx \| Rust \| Python/, 'scripting')])) {
		if (label) list.push({ label, windows: null, linux: null });
		const row = list.at(-1);
		const [calx, rust, python] = times;
		const t = { calx, rust, python, ms: { calx: num(calx), rust: num(rust), python: num(python) } };
		if (platform.startsWith('Windows')) row.windows = t;
		else if (platform.startsWith('Linux')) row.linux = t;
		else throw new Error(`README.md's scripting table has a row for "${platform}", not Windows or Linux`);
	}
	if (list.length < 3 || list.some((r) => !r.windows || !r.linux)) throw new Error("README.md's scripting table isn't a Windows and a Linux row for each benchmark");
	return list;
}

/** The language suite's languages, in the order its tables have them. */
export const SUITE_LANGS = [
	{ id: 'calx', name: 'Calx' },
	{ id: 'c', name: 'C' },
	{ id: 'rust', name: 'Rust' },
	{ id: 'go', name: 'Go' },
	{ id: 'python', name: 'Python' },
];

/** The groups of the language suite, each a section of the language benchmarks page. */
export const SUITE_GROUPS = [
	{ id: 'compute', title: 'Classic compute', anchor: 'classic-compute' },
	{ id: 'realistic', title: 'Realistic programs', anchor: 'realistic-programs' },
	{ id: 'parallel', title: 'Parallel loops, on every processor', anchor: 'parallel-loops' },
];

/** The systems the suite is recorded on, each a file in bench/results. */
export const SUITE_OSES = ['windows', 'linux'];

const records = new Map();
function record(os) {
	if (!records.has(os)) records.set(os, JSON.parse(readFileSync(new URL(`bench/results/suite-${os}.json`, REPO), 'utf8')));
	return records.get(os);
}

/** Times within 5% of each other are even. */
export const EVEN = 1.05;

/** Calx's time against another language's: which was faster and how many times, or even. */
export function versus(calx, other) {
	const r = calx / other;
	if (r < EVEN && r > 1 / EVEN) return { kind: 'even', factor: 1 };
	return r < 1 ? { kind: 'calx', factor: 1 / r } : { kind: 'other', factor: r };
}

/** The system a record was made on, as the language benchmarks page names it: "Windows 11", or
 *  "Ubuntu 26.04 in WSL2". */
function systemName(machine) {
	if (machine.platform === 'windows') return machine.os.match(/Windows \d+/)?.[0] ?? 'Windows';
	const os = machine.os.replace(/ LTS$/, '');
	return /microsoft/i.test(machine.kernel ?? '') ? `${os} in WSL2` : os;
}

/** bench/suite.calx's recorded results on `os` (bench/results/suite-<os>.json): each group's
 *  benchmarks with each language's median wall time in ms, or why it has none. A parallel benchmark
 *  gives its run on the most threads. The language benchmarks page's tables are written from the
 *  same file, so the two can't disagree for long. */
export function suite(os = 'windows') {
	const doc = record(os);
	const groups = SUITE_GROUPS.map((g) => ({ ...g, rows: [] }));
	let threads = 0;
	let rounds = 0;
	for (const [name, r] of Object.entries(doc.results)) {
		const group = groups.find((g) => g.id === r.group);
		if (!group) continue;
		let byLang = r;
		if (r.group === 'parallel') {
			const counts = Object.keys(r.threads).map(Number);
			threads = Math.max(...counts);
			byLang = r.threads[String(threads)];
		}
		const ms = {};
		const why = {};
		for (const { id } of SUITE_LANGS) {
			const p = byLang[id];
			if (p?.status === 'ok') ms[id] = p.median_ms;
			else why[id] = p ? p.why ?? p.status : 'no program';
		}
		rounds = Math.max(rounds, byLang.calx?.wall_ms?.length ?? 0);
		// "n-body, 10 million steps" is "n-body" in the chart; the table says the rest.
		group.rows.push({ name, title: r.title, label: r.title.split(/[:,]/)[0], ms, why });
	}
	const tc = doc.toolchain;
	const cpu = doc.machine.cpu.replace(/^\d+th Gen /, '').replace('(R)', '').replace('(TM)', '');
	return {
		os,
		system: systemName(doc.machine),
		groups: groups.filter((g) => g.rows.length),
		threads,
		cpu,
		machine: `${cpu}, ${systemName(doc.machine)}`,
		rounds,
		recorded: doc.recorded.slice(0, 10),
		versions: {
			clang: tc.clang.match(/clang version ([\d.]+)/)?.[1],
			rustc: tc.rustc.match(/rustc ([\d.]+)/)?.[1],
			go: tc.go.match(/go([\d.]+)/)?.[1],
			python: tc.python.match(/Python ([\d.]+)/)?.[1],
		},
	};
}

/** For each language Calx is timed against on `os`: how many benchmarks Calx was faster on, even
 *  on and slower on, of those both have a time for, and the geometric mean of Calx's time over
 *  theirs. A mean under 1 is Calx taking less time. */
export function tally(os = 'windows') {
	const s = suite(os);
	return SUITE_LANGS.filter((l) => l.id !== 'calx').map((l) => {
		const t = { id: l.id, name: l.name, calx: 0, even: 0, other: 0, both: 0, mean: 1 };
		let logs = 0;
		for (const g of s.groups)
			for (const r of g.rows) {
				if (r.ms.calx === undefined || r.ms[l.id] === undefined) continue;
				t[versus(r.ms.calx, r.ms[l.id]).kind]++;
				t.both++;
				logs += Math.log(r.ms.calx / r.ms[l.id]);
			}
		t.mean = Math.exp(logs / t.both);
		return t;
	});
}

/** The languages the build benchmarks time: Python has no build. */
export const BUILD_LANGS = SUITE_LANGS.filter((l) => l.id !== 'python');

/** The suite's builds and startup on `os`: each build's median wall time per language, the
 *  programs' sizes, and how long hello, world takes to start. */
export function dx(os = 'windows') {
	const d = record(os).results.dx;
	const ms = (p) => (p?.status === 'ok' ? p.median_ms : undefined);
	return {
		modules: d.modules,
		lines: d.lines,
		rounds: Object.values(d.builds)[0].calx.wall_ms.length,
		builds: Object.entries(d.builds).map(([id, b]) => ({
			id,
			title: b.title,
			ms: Object.fromEntries(BUILD_LANGS.map((l) => [l.id, ms(b[l.id])])),
		})),
		sizes: d.sizes_bytes,
		startup: Object.fromEntries(SUITE_LANGS.map((l) => [l.id, ms(d.startup[l.id])])),
	};
}
