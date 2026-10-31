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
