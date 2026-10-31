// What the site changes in docs/ as it renders it, so the files stay plain Markdown that reads
// right on GitHub (D40):
//
// - Links between pages are relative paths to .md files. They become the pages' URLs, so
//   "../reference/maps.md#hash" is "/reference/maps/#hash".
// - Links to the rest of the repo, such as "../../std/io.calx", go to the file on GitHub.
// - Each page starts with its title as a "# " heading, for GitHub. The site shows the title from
//   the front matter, so that first heading goes.
// - A decision the docs cite, "(D21)", is for the people working on Calx, who read the docs on
//   GitHub beside the decision log. The site leaves it out, so its readers don't meet ids that
//   mean nothing to them. The text of links keeps its words as written.
import { statSync } from 'node:fs';
import { dirname, relative, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { slugOf } from './docs-index.mjs';
import { DOCS } from './paths.mjs';

export const GITHUB = 'https://github.com/ellipog/calx';
const docsDir = fileURLToPath(DOCS);
const repoDir = resolve(docsDir, '..');

const inside = (dir, path) => {
	const rel = relative(dir, path);
	return !!rel && !rel.startsWith('..') && !rel.includes(':');
};

/** Where a link in the docs file at `from` goes on the site, or the link unchanged. */
export function siteUrl(url, from) {
	if (/^[a-z][a-z0-9+.-]*:/i.test(url) || url.startsWith('#') || url.startsWith('/')) return url;
	const [path, hash] = url.split('#');
	const target = resolve(dirname(from), decodeURI(path));
	const anchor = hash ? '#' + hash : '';
	if (inside(docsDir, target) && target.endsWith('.md')) {
		const rel = relative(docsDir, target).split(sep).join('/');
		return rel === 'README.md' ? '/docs/' + anchor : '/' + slugOf(rel) + '/' + anchor;
	}
	if (inside(repoDir, target)) {
		let kind = 'blob';
		try {
			if (statSync(target).isDirectory()) kind = 'tree';
		} catch {
			// A link to a file that isn't there: GitHub will say so, as the docs do.
		}
		return `${GITHUB}/${kind}/main/${relative(repoDir, target).split(sep).join('/')}${anchor}`;
	}
	return url;
}

/** A line of docs/README.md's page list as HTML: escaped, with its `code` spans and links, and a
 *  capital first letter. Inside something that is a link already, `links: false` keeps a link's
 *  text and drops the link. */
export function lineHtml(text, { links = true } = {}) {
	const esc = (s) => s.replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]);
	const readme = fileURLToPath(new URL('README.md', DOCS));
	return esc(text.charAt(0).toUpperCase() + text.slice(1))
		.replace(/`([^`]+)`/g, '<code>$1</code>')
		.replace(/\[([^\]]+)\]\(([^)]+)\)/g, (_, label, url) =>
			links ? `<a href="${esc(siteUrl(url, readme))}">${label}</a>` : label
		);
}

// The ways the docs cite decisions, and what each leaves on the site:
// "(D21)" or "(D59, D60)" goes with the space before it; "(ThinLTO, D99)" keeps "(ThinLTO)",
// which "([lists](lists.md), D159)" is too, its ", D159)" being a text node of its own after the
// link; and "(D41, run.py)" keeps "(run.py)". A decision named in a sentence, as in "until D89",
// is reworded in the docs instead, so nothing here guesses at the words around it.
const CITES = [
	[/\s*\(D\d+(?:,\s*D\d+)*\)/g, ''],
	[/,\s*D\d+(?:,\s*D\d+)*\)/g, ')'],
	[/\(D\d+(?:,\s*D\d+)*,\s*/g, '('],
];

/** The text without the decisions it cites. */
export function withoutDecisions(value) {
	return CITES.reduce((text, [re, by]) => text.replace(re, by), value);
}

/** The satteri plugin. It only touches files under docs/. */
export function calxDocs() {
	return ({ fileURL }) => {
		if (!fileURL) return null;
		const file = fileURLToPath(fileURL);
		if (!inside(docsDir, file)) return null;
		let titleGone = false;
		return {
			name: 'calx-docs',
			heading(node, ctx) {
				if (node.depth !== 1 || titleGone) return;
				titleGone = true;
				ctx.removeNode(node);
			},
			link(node, ctx) {
				const url = siteUrl(node.url, file);
				if (url !== node.url) ctx.setProperty(node, 'url', url);
			},
			definition(node, ctx) {
				const url = siteUrl(node.url, file);
				if (url !== node.url) ctx.setProperty(node, 'url', url);
			},
			text(node, ctx) {
				if (!/\bD\d+\b/.test(node.value)) return;
				for (let p = ctx.parent(node); p; p = ctx.parent(p)) {
					if (p.type === 'link' || p.type === 'linkReference') return;
				}
				const value = withoutDecisions(node.value);
				if (value !== node.value) ctx.setProperty(node, 'value', value);
			},
		};
	};
}
