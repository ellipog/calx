// What the site changes in docs/ as it renders it, so the files stay plain Markdown that reads
// right on GitHub (D40):
//
// - Links between pages are relative paths to .md files. They become the pages' URLs, so
//   "../reference/maps.md#hash" is "/reference/maps/#hash".
// - Links to the rest of the repo, such as "../../std/io.calx", go to the file on GitHub.
// - Each page starts with its title as a "# " heading, for GitHub. The site shows the title from
//   the front matter, so that first heading goes.
// - A decision the docs cite, "(D21)", links to its row in the decision log, as the mockup draws
//   it: the number alone, small and quiet. Headings and the text of links keep theirs as written.
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

/** The decision log the docs cite. Its rows have no anchors, so a link finds the row by its text. */
export const DECISIONS = 'https://github.com/ellipog/calcite/blob/main/docs/decisions.md';
// "(D21)" or "(D59, D60)", whose parentheses go, or a number on its own, as in "until D89".
const DREF = /\((D\d+(?:,\s*D\d+)*)\)|\bD\d+\b/g;

/** The text with each decision it cites as a link, as Markdown nodes, or null if it cites none. */
export function decisionNodes(value) {
	const out = [];
	let last = 0;
	for (const m of value.matchAll(DREF)) {
		if (m.index > last) out.push({ type: 'text', value: value.slice(last, m.index) });
		(m[1] ?? m[0]).split(/,\s*/).forEach((id, i) => {
			if (i) out.push({ type: 'text', value: ', ' });
			out.push({
				type: 'html',
				value: `<a class="dref" href="${DECISIONS}#:~:text=${id}" title="Decision ${id} in the decision log">${id}</a>`,
			});
		});
		last = m.index + m[0].length;
	}
	if (!out.length) return null;
	if (last < value.length) out.push({ type: 'text', value: value.slice(last) });
	return out;
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
					if (p.type === 'heading' || p.type === 'link' || p.type === 'linkReference') return;
				}
				const nodes = decisionNodes(node.value);
				if (nodes) ctx.replaceNode(node, nodes);
			},
		};
	};
}
