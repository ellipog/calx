// Starlight's route middleware: what it puts in each docs page's head, changed where the site
// says more than Starlight does (src/seo.mjs): the title, and for a page of docs/, its dates,
// structured data, and a link to its Markdown.
import { defineRouteMiddleware } from '@astrojs/starlight/route-data';
import { datesOf, isDocsFile, repoPath } from './facts.mjs';
import { docsData, pageTitle } from './seo.mjs';

export const onRequest = defineRouteMiddleware(({ locals }) => {
	const route = locals.starlightRoute;
	const { title, description } = route.entry.data;
	const head = route.head.find((h) => h.tag === 'title');
	if (head) head.content = pageTitle(title, route.id);
	if (!isDocsFile(route.entry)) return;

	// The page's Markdown, for tools that read docs to language models (src/llms.mjs).
	route.head.push({ tag: 'link', attrs: { rel: 'alternate', type: 'text/markdown', href: `/${route.id}.md` } });
	const dates = datesOf(repoPath(route.entry));
	if (dates.published) route.head.push({ tag: 'meta', attrs: { property: 'article:published_time', content: dates.published } });
	if (dates.modified) route.head.push({ tag: 'meta', attrs: { property: 'article:modified_time', content: dates.modified } });
	route.head.push({
		tag: 'script',
		attrs: { type: 'application/ld+json' },
		content: JSON.stringify(docsData({ slug: route.id, title, description, dates })).replace(/</g, '\\u003c'),
	});
});
