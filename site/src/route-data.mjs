// Starlight's route middleware: what it puts in each docs page's head, changed where the site
// says more than Starlight does (src/seo.mjs).
import { defineRouteMiddleware } from '@astrojs/starlight/route-data';
import { pageTitle } from './seo.mjs';

export const onRequest = defineRouteMiddleware(({ locals }) => {
	const route = locals.starlightRoute;
	const title = route.head.find((h) => h.tag === 'title');
	if (title) title.content = pageTitle(route.entry.data.title, route.id);
});
