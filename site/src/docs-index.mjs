// The page order lives in docs/README.md (D40), under "## Pages": a "### Section" heading for
// each part, then a numbered list of "[Title](path.md): what it covers". This reads that list,
// for the sidebar and for the /docs/ overview, so adding a page to the README adds it to the site.
import { readFileSync } from 'node:fs';
import { DOCS } from './paths.mjs';

/** A page's slug on the site, from its path under docs/: "reference/maps.md" is "reference/maps". */
export function slugOf(path) {
	return path === 'CALX_SPEC.md' ? 'spec' : path.replace(/\.md$/, '');
}

/** The sections in the README's page list: [{ title, id, intro, pages: [{ title, slug, about }] }]. */
export function docsIndex() {
	const text = readFileSync(new URL('README.md', DOCS), 'utf8').replace(/\r\n/g, '\n');
	const list = text.split(/^## Pages\n/m)[1]?.split(/^## /m)[0];
	if (!list) throw new Error('docs/README.md has no "## Pages" section');
	const sections = [];
	for (const block of list.split(/^### /m).slice(1)) {
		const [title, ...lines] = block.split('\n');
		const pages = [];
		const intro = [];
		for (const line of lines) {
			const page = line.match(/^\d+\. \[([^\]]+)\]\(([^)#]+\.md)\)(?::\s*(.*))?$/);
			if (page) pages.push({ title: page[1], slug: slugOf(page[2]), about: page[3] ?? '' });
			else if (line.trim() && !pages.length) intro.push(line.trim());
		}
		// The heading's id as GitHub and the site make it, so README.md#standard-library links work on both.
		const id = title.trim().toLowerCase().replace(/[^a-z0-9 -]/g, '').replace(/ /g, '-');
		sections.push({ title: title.trim(), id, intro: intro.join(' '), pages });
	}
	return sections;
}

/** The Starlight sidebar: one group per section. The standard library and the design pages start
 *  closed, unless the page is in one, so the guide and the reference come first. */
export function sidebar() {
	return docsIndex().map((section) => ({
		label: section.title,
		collapsed: section.id === 'standard-library' || section.id === 'design',
		items: section.pages.map((page) => ({ label: page.title, slug: page.slug })),
	}));
}
