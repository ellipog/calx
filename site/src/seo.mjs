// What search engines read about a page beyond its front matter: its title, where it sits, and
// structured data (schema.org, as JSON-LD). site/README.md's "Search engines" says why.
import { docsIndex, fileOf } from './docs-index.mjs';

export const ORIGIN = 'https://calxlang.com';

/** The site's name for the language, the home page's title, and the end of most titles. */
export const LANGUAGE = 'The Calx programming language';

// "Calx" alone is also a word in chemistry and in anatomy, so every title says which Calx this is,
// and the part of the docs it's in: a result for std.zlib reads "std.zlib | Calx standard library".
const ENDS = { reference: 'Calx language reference', std: 'Calx standard library' };

/** The part of a title after the page's own, by where the page is. */
export function titleEnd(slug) {
	return ENDS[slug.split('/')[0]] ?? LANGUAGE;
}

/** A page's <title>: "Parallel loops | Calx language reference". A section's own page is named by
 *  the end alone: /std/ is "Calx standard library". */
export function pageTitle(title, slug) {
	return slug in ENDS ? ENDS[slug] : `${title} | ${titleEnd(slug)}`;
}

/** The sections of docs/README.md's page list that have a page of their own, listing their pages
 *  (src/pages/[hub].astro). The rest are parts of /docs/. Java compatibility's pages are in the
 *  reference's folder, so they're on its page too. */
export const HUBS = [
	{
		path: 'guide',
		title: 'Guide',
		sections: ['guide'],
		description:
			'Learn Calx by building one small program, stash, a page at a time: installing Calx, the calx command, input, enums, errors, maps and modules.',
	},
	{
		path: 'reference',
		title: 'Reference',
		sections: ['reference', 'java-compatibility'],
		description:
			'What each feature of Calx does, one page per feature: syntax, types, ownership, generics, interfaces, collections, parallel loops, and the C each compiles to.',
	},
	{
		path: 'std',
		title: 'Standard library',
		sections: ['standard-library'],
		description:
			'The modules of the Calx standard library, one page each: input and output, math, files, processes, threads, networking, HTTP, hashing, compression and archives.',
	},
];

/** Where a section of the docs is listed: its own page, a part of one, or a part of /docs/. */
export function sectionPath(id) {
	const hub = HUBS.find((h) => h.sections.includes(id));
	if (!hub) return `/docs/#${id}`;
	return hub.sections[0] === id ? `/${hub.path}/` : `/${hub.path}/#${id}`;
}

/** The file in the repo a page is made from, by its path on the site: "/reference/parallel/" is
 *  docs/reference/parallel.md, and /docs/ and the section pages are made from docs/README.md's
 *  page list. Null for the home page, the roadmap, and the 404 page, which are made from more. */
export function sourceOf(path) {
	const slug = path.replace(/^\/|\/$/g, '');
	if (slug === 'docs' || HUBS.some((h) => h.path === slug)) return 'docs/README.md';
	const listed = docsIndex().some((s) => s.pages.some((p) => p.slug === slug));
	return listed && slug !== 'roadmap' ? `docs/${fileOf(slug)}` : null;
}

/** The section a docs page is in, from the page list, or undefined. */
export function sectionOf(slug) {
	return docsIndex().find((s) => s.pages.some((p) => p.slug === slug));
}

// The studio that makes Calx, as aaenz.no describes it, by the same @id, so the two sites name the
// same organization.
const STUDIO = {
	'@type': 'Organization',
	'@id': 'https://aaenz.no/#organization',
	name: 'Aaen Studios',
	url: 'https://aaenz.no/',
	logo: { '@type': 'ImageObject', url: 'https://aaenz.no/assets/logo-mark.png' },
};
const studio = { '@id': STUDIO['@id'], '@type': 'Organization', name: STUDIO.name, url: STUDIO.url };
const website = { '@id': `${ORIGIN}/#website`, '@type': 'WebSite', name: 'Calx', url: `${ORIGIN}/` };
const language = { '@id': `${ORIGIN}/#language`, '@type': 'ComputerLanguage', name: 'Calx', url: `${ORIGIN}/` };

/** The home page's structured data: the site, named Calx rather than by its domain (Google reads
 *  WebSite for a result's site name), the language it's about, and the studio. */
export function homeData(description) {
	return {
		'@context': 'https://schema.org',
		'@graph': [
			{
				...website,
				alternateName: ['Calx programming language', 'calxlang'],
				description,
				inLanguage: 'en',
				publisher: { '@id': STUDIO['@id'] },
				about: { '@id': language['@id'] },
			},
			{
				...language,
				alternateName: 'Calx programming language',
				description,
				sameAs: ['https://github.com/ellipog/calx'],
				publisher: { '@id': STUDIO['@id'] },
			},
			STUDIO,
		],
	};
}

/** A docs page's structured data: the page as a TechArticle with its dates, and its crumbs, which
 *  a result may show in place of its address ("Reference › Parallel loops"). */
export function docsData({ slug, title, description, dates }) {
	const url = `${ORIGIN}/${slug}/`;
	const section = sectionOf(slug);
	const graph = [
		{
			'@type': 'TechArticle',
			'@id': `${url}#article`,
			headline: title,
			description,
			url,
			mainEntityOfPage: url,
			inLanguage: 'en',
			image: `${ORIGIN}/social.png`,
			...(dates.published && { datePublished: dates.published }),
			...(dates.modified && { dateModified: dates.modified }),
			author: studio,
			publisher: studio,
			isPartOf: website,
			about: language,
		},
	];
	if (section)
		graph.push({
			'@type': 'BreadcrumbList',
			itemListElement: [
				{ '@type': 'ListItem', position: 1, name: section.title, item: ORIGIN + sectionPath(section.id) },
				{ '@type': 'ListItem', position: 2, name: title, item: url },
			],
		});
	return { '@context': 'https://schema.org', '@graph': graph };
}
