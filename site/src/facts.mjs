// What a docs page says about itself: the commit the site was built from, where to edit the page,
// and how many of its examples CI tests.
import { execFileSync } from 'node:child_process';
import { existsSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { GITHUB } from './markdown.mjs';
import { REPO, SITE } from './paths.mjs';

function git(...args) {
	try {
		return execFileSync('git', args, {
			cwd: fileURLToPath(REPO),
			encoding: 'utf8',
			stdio: ['ignore', 'pipe', 'ignore'],
			maxBuffer: 64 << 20,
			timeout: 60_000,
		}).trim();
	} catch {
		return '';
	}
}

const shallow = () => git('rev-parse', '--is-shallow-repository') === 'true';

/** The short hash of the commit the site was built from, or '' outside a checkout. */
export const COMMIT = git('rev-parse', '--short', 'HEAD');

/** The page's path in the repo, such as docs/reference/parallel.md. Starlight gives the /docs/
 *  overview (src/pages/docs.astro) a path that isn't a file; that page is made from docs/README.md. */
export function repoPath(entry) {
	if (!entry.filePath || !existsSync(new URL(entry.filePath, SITE))) return 'docs/README.md';
	return entry.filePath.replace(/^(\.\.\/)+/, '');
}

/** Whether the entry is a page of docs/, rather than one of the site's own pages Starlight renders
 *  (the /docs/ overview and the section pages). */
export function isDocsFile(entry) {
	return repoPath(entry) !== 'docs/README.md';
}

// When each file in docs/ was first committed and last changed, from one `git log` over the folder.
// A shallow clone has only recent history, so the build fetches the rest first, from the public
// repo, which takes a few seconds. If it can't, a clone with only recent history doesn't know the
// dates before it begins: a file whose oldest commit there is where the history is cut off has no
// known first date, and one last changed there has no known last date either, so neither is guessed.
let DATES = null;
function allDates() {
	if (DATES) return DATES;
	DATES = new Map();
	if (shallow()) git('fetch', '--unshallow', '--quiet', `${GITHUB}.git`);
	let cut = new Set();
	if (shallow()) {
		try {
			cut = new Set(readFileSync(git('rev-parse', '--path-format=absolute', '--git-path', 'shallow'), 'utf8').split(/\s+/).filter(Boolean));
		} catch {}
	}
	// Newest first: the first commit seen for a file is its last change, the last one its first.
	for (const block of git('log', '--format=%x00%H %cI', '--name-only', '--', 'docs').split('\0').slice(1)) {
		const [head, ...files] = block.trim().split('\n');
		const [sha, date] = head.split(' ');
		const known = cut.has(sha) ? null : date;
		for (const file of files) {
			if (!file) continue;
			const d = DATES.get(file);
			if (d) d.published = known;
			else DATES.set(file, { modified: known, published: known });
		}
	}
	return DATES;
}

/** When a file in docs/ was first committed and last changed, as ISO dates, each null if unknown. */
export function datesOf(path) {
	return allDates().get(path) ?? { modified: null, published: null };
}

export function editUrl(entry) {
	return `${GITHUB}/edit/main/${repoPath(entry)}`;
}

// tests/run.calx tests every ```calx block in these folders, in debug and release, on Windows and
// Linux (D41). The rest of docs/, the spec and the benchmark notes, isn't tested.
const TESTED = ['docs/guide/', 'docs/reference/', 'docs/std/'];

/** How many of the page's examples CI tests: run.calx's count, a line that is exactly ```calx. */
export function testedExamples(entry) {
	if (!TESTED.some((d) => repoPath(entry).startsWith(d))) return 0;
	return (entry.body ?? '').split(/\r?\n/).filter((l) => l.trim() === '```calx').length;
}
