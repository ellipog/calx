// What a docs page says about itself: the commit the site was built from, where to edit the page,
// and how many of its examples CI tests.
import { execFileSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { GITHUB } from './markdown.mjs';
import { SITE } from './paths.mjs';

function git(...args) {
	try {
		return execFileSync('git', args, { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] }).trim();
	} catch {
		return '';
	}
}

/** The short hash of the commit the site was built from, or '' outside a checkout. */
export const COMMIT = git('rev-parse', '--short', 'HEAD');

/** The page's path in the repo, such as docs/reference/parallel.md. Starlight gives the /docs/
 *  overview (src/pages/docs.astro) a path that isn't a file; that page is made from docs/README.md. */
export function repoPath(entry) {
	if (!entry.filePath || !existsSync(new URL(entry.filePath, SITE))) return 'docs/README.md';
	return entry.filePath.replace(/^(\.\.\/)+/, '');
}

export function editUrl(entry) {
	return `${GITHUB}/edit/main/${repoPath(entry)}`;
}

// tests/run.py tests every ```calx block in these folders, in debug and release, on Windows and
// Linux (D41). The rest of docs/, the spec and the benchmark notes, isn't tested.
const TESTED = ['docs/guide/', 'docs/reference/', 'docs/std/'];

/** How many of the page's examples CI tests: run.py's count, a line that is exactly ```calx. */
export function testedExamples(entry) {
	if (!TESTED.some((d) => repoPath(entry).startsWith(d))) return 0;
	return (entry.body ?? '').split(/\r?\n/).filter((l) => l.trim() === '```calx').length;
}
