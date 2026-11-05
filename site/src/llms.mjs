// The docs as Markdown, for language models and the tools that read docs to them: /llms.txt, the
// llmstxt.org index of every page; /llms-full.txt, every page in one file; and each page's own
// Markdown beside it, /reference/parallel.md for /reference/parallel/. They're docs/ as it is, as
// the site shows it: the front matter's title and description at the top, no decisions cited,
// and every link a full address, so a page makes sense on its own.
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { docsIndex, fileOf } from './docs-index.mjs';
import { siteUrl, withoutDecisions } from './markdown.mjs';
import { DOCS } from './paths.mjs';
import { ABOUT, ORIGIN } from './seo.mjs';

/** Every page in the docs' page list, in reading order, with its section. */
export function listedPages() {
	return docsIndex().flatMap((section) => section.pages.map((page) => ({ ...page, section })));
}

// A front matter value, plain or in YAML's double quotes, which escape as JSON does for these.
const value = (front, key) => {
	const raw = front.match(new RegExp(`^${key}: (.*)$`, 'm'))?.[1].trim() ?? '';
	return raw.startsWith('"') ? JSON.parse(raw) : raw;
};

/** A docs page as Markdown, from its slug. */
export function pageMarkdown(slug) {
	const file = fileURLToPath(new URL(fileOf(slug), DOCS));
	const text = readFileSync(file, 'utf8').replace(/\r\n/g, '\n');
	const front = text.match(/^---\n([\s\S]*?)\n---\n/);
	if (!front) throw new Error(`docs/${fileOf(slug)} has no front matter`);
	const page = `${ORIGIN}/${slug}/`;
	// A link as the site has it, and then whole: "#hash" on this page, "/std/io/" on the site.
	const link = (url) => {
		const to = url.startsWith('#') ? page + url : siteUrl(url, file);
		return to.startsWith('/') ? ORIGIN + to : to;
	};
	let fence = null;
	let titleGone = false;
	const body = text
		.slice(front[0].length)
		.split('\n')
		.filter((line) => {
			// The page's "# Title" goes, as on the site: the front matter's title takes its place.
			if (!fence && !titleGone && line.startsWith('# ')) return !(titleGone = true);
			return true;
		})
		.map((line) => {
			const mark = line.match(/^\s*(`{3,}|~{3,})/)?.[1];
			if (mark && (!fence || (mark[0] === fence[0] && mark.length >= fence.length))) {
				fence = fence ? null : mark;
				return line;
			}
			if (fence) return line;
			return withoutDecisions(line).replace(/\]\(([^)\s]+)\)/g, (_, url) => `](${link(url)})`);
		})
		.join('\n')
		.trim();
	return `# ${value(front[1], 'title')}\n\n> ${value(front[1], 'description')}\n\n${body}\n`;
}

/** /llms.txt: what Calx is, and every docs page by section, with what it covers. */
export function llmsTxt() {
	const lines = [
		'# Calx',
		'',
		`> ${ABOUT}`,
		'',
		`Calx compiles to C. These are its docs, from ${ORIGIN}/docs/, in reading order: the guide teaches the language by building one program, the reference has a page for each feature, and the standard library a page for each module. Each link is the page on the site; its Markdown is the same address with .md in place of the closing slash. ${ORIGIN}/llms-full.txt has every page in one file.`,
	];
	for (const section of docsIndex()) {
		lines.push('', `## ${section.title}`, '');
		for (const page of section.pages) {
			// The page list's "what it covers", without its links: they're relative to docs/.
			const about = page.about.replace(/\[([^\]]+)\]\([^)]+\)/g, '$1');
			lines.push(`- [${page.title}](${ORIGIN}/${page.slug}/)${about ? `: ${about}` : ''}`);
		}
	}
	return lines.join('\n') + '\n';
}

/** /llms-full.txt: every docs page, in reading order, each with its address. */
export function llmsFullTxt() {
	const pages = listedPages().map(
		(p) => `${pageMarkdown(p.slug).replace(/\n/, `\n\nFrom ${ORIGIN}/${p.slug}/, in ${p.section.title}.\n`)}`
	);
	return [`# The Calx documentation\n\n> ${ABOUT}\n\nEvery page of ${ORIGIN}/docs/, in reading order.\n`, ...pages].join('\n\n---\n\n');
}
