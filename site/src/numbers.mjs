// The measurements the home page quotes, read from the "## Performance" section of the repo's
// README, where they're recorded with each change that moves them. So the site never has numbers
// of its own to fall behind.
import { readFileSync } from 'node:fs';
import { REPO } from './paths.mjs';

const README = new URL('README.md', REPO);

function section() {
	const text = readFileSync(README, 'utf8').replace(/\r\n/g, '\n');
	const body = text.split(/^## Performance\n/m)[1]?.split(/^## /m)[0];
	if (!body) throw new Error('README.md has no "## Performance" section');
	return body;
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

/** The tick-loop benchmark: ms per tick for each platform, and how it was measured. */
export function tickLoop() {
	const body = section();
	const tables = body.split(/\n\n+/).filter((b) => b.startsWith('|'));
	const platforms = rows(tables[0]).map(([platform, calx1, rust1, calx24, rust24]) => ({
		platform,
		calx1: num(calx1),
		rust1: num(rust1),
		calx24: num(calx24),
		rust24: num(rust24),
	}));
	const pick = (re, what) => {
		const m = body.match(re);
		if (!m) throw new Error(`README.md's performance section doesn't give ${what}`);
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
	const tables = section()
		.split(/\n\n+/)
		.filter((b) => b.startsWith('|'));
	const all = rows(tables[1]).map((r) => ({ label: r[0], windows: r[1], linux: r[2] }));
	if (all.length < 3 || all.some((r) => !/\d m?s$/.test(r.windows))) throw new Error("README.md's build-time table isn't label, Windows, Linux");
	return all;
}

/** The language suite's languages, in the order its tables have them. */
export const SUITE_LANGS = [
	{ id: 'calx', name: 'Calx' },
	{ id: 'c', name: 'C' },
	{ id: 'rust', name: 'Rust' },
	{ id: 'go', name: 'Go' },
	{ id: 'python', name: 'Python' },
];

const SUITE_GROUPS = [
	{ id: 'compute', title: 'Classic compute' },
	{ id: 'realistic', title: 'Realistic programs' },
	{ id: 'parallel', title: 'Parallel loops, on every processor' },
];

/** bench/suite.calx's recorded results on Windows (bench/results/suite-windows.json), for the
 *  chart on the language benchmarks page: each group's benchmarks with each language's median
 *  wall time in ms, or why it has none. A parallel benchmark gives its run on the most threads.
 *  The page's tables are written from the same file, so the two can't disagree for long. */
export function suite() {
	const doc = JSON.parse(readFileSync(new URL('bench/results/suite-windows.json', REPO), 'utf8'));
	const groups = SUITE_GROUPS.map((g) => ({ ...g, rows: [] }));
	let threads = 0;
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
		// "n-body, 10 million steps" is "n-body" in the chart; the table says the rest.
		group.rows.push({ name, title: r.title, label: r.title.split(/[:,]/)[0], ms, why });
	}
	const tc = doc.toolchain;
	return {
		groups: groups.filter((g) => g.rows.length),
		threads,
		machine: `${doc.machine.cpu.replace(/^\d+th Gen /, '').replace('(R)', '').replace('(TM)', '')}, ${doc.machine.os.replace('Microsoft ', '')}`,
		rounds: doc.rounds,
		versions: {
			clang: tc.clang.match(/clang version ([\d.]+)/)?.[1],
			rustc: tc.rustc.match(/rustc ([\d.]+)/)?.[1],
			go: tc.go.match(/go([\d.]+)/)?.[1],
			python: tc.python.match(/Python ([\d.]+)/)?.[1],
		},
	};
}
