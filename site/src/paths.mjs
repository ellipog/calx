// Where the site finds the rest of the repo. While developing, these modules run from src/; in a
// build, Astro bundles them into chunks under .astro/, so a path relative to the module would
// point somewhere else. Paths start from the site's folder instead, found by walking up from the
// module to the package.json named calx-site, which is the same place in both.
import { existsSync, readFileSync } from 'node:fs';

function siteDir() {
	let dir = new URL('./', import.meta.url);
	for (;;) {
		const pkg = new URL('package.json', dir);
		if (existsSync(pkg) && JSON.parse(readFileSync(pkg, 'utf8')).name === 'calx-site') return dir;
		const up = new URL('../', dir);
		if (up.href === dir.href) throw new Error(`no calx-site package.json above ${import.meta.url}`);
		dir = up;
	}
}

/** site/ */
export const SITE = siteDir();
/** The repo's root, which holds docs/, README.md and assets/. */
export const REPO = new URL('../', SITE);
export const DOCS = new URL('docs/', REPO);
