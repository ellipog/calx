// The docs collection is docs/ itself, not a copy (D40). Its README is the index and the writing
// rules, so it isn't a page; the site's /docs/ overview is made from its page list instead.
import { defineCollection } from 'astro:content';
import { glob } from 'astro/loaders';
import { i18nLoader } from '@astrojs/starlight/loaders';
import { docsSchema, i18nSchema } from '@astrojs/starlight/schema';
// @ts-expect-error: a plain .mjs module, shared with astro.config.mjs.
import { slugOf } from './docs-index.mjs';

export const collections = {
	// Starlight's interface text, where the site words it differently: src/content/i18n/en.json.
	i18n: defineCollection({ loader: i18nLoader(), schema: i18nSchema() }),
	docs: defineCollection({
		loader: glob({
			base: '../docs',
			pattern: ['**/*.md', '!README.md'],
			generateId: ({ entry }) => slugOf(entry),
		}),
		schema: docsSchema(),
	}),
};
