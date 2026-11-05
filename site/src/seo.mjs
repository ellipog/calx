// What search engines read about a page beyond its front matter. "Calx" alone is also a word in
// chemistry and in anatomy, so every title says which Calx this is, and the part of the docs it's
// in: a result for std.zlib reads "std.zlib | Calx standard library".

/** The site's name for the language, the home page's title, and the end of most titles. */
export const LANGUAGE = 'The Calx programming language';

/** The part of a title after the page's own, by where the page is. */
export function titleEnd(slug) {
	if (slug.startsWith('reference/')) return 'Calx language reference';
	if (slug.startsWith('std/')) return 'Calx standard library';
	return LANGUAGE;
}

/** A page's <title>: "Parallel loops | Calx language reference". */
export function pageTitle(title, slug) {
	return `${title} | ${titleEnd(slug)}`;
}
