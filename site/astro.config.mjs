// @ts-check
// The Calx website: the home page, the 404 page and a docs overview are Astro pages in src/pages,
// and Starlight renders every page of docs/ as it is (D40). site/README.md says how it fits.
import { defineConfig } from 'astro/config';
import { satteri } from '@astrojs/markdown-satteri';
import starlight from '@astrojs/starlight';
import { calxAssets } from './src/assets.mjs';
import { sidebar } from './src/docs-index.mjs';
import { calxDocs, GITHUB } from './src/markdown.mjs';

// The site's one address, for canonical links, link previews, the sitemap and robots.txt. Every
// build names it, previews and local ones too, so no page ever points search engines elsewhere.
const SITE = 'https://calxlang.com';

export default defineConfig({
	// The build goes to dist/, Astro's own place for it, where Vercel looks.
	site: SITE,
	// The dev server shows the pages as they'll ship, without Astro's toolbar over them.
	devToolbar: { enabled: false },
	markdown: {
		processor: satteri({ mdastPlugins: [calxDocs()] }),
	},
	integrations: [
		calxAssets(),
		starlight({
			title: 'Calx',
			description: 'A general-purpose compiled language: fast, memory-safe, and exact.',
			favicon: '/favicon.svg',
			social: [{ icon: 'github', label: 'GitHub', href: GITHUB }],
			sidebar: sidebar(),
			customCss: ['./src/styles/tokens.css', './src/styles/docs.css'],
			// Starlight's heading anchors and asides, for files in docs/ as well as its own folder.
			markdown: { processedDirs: ['../docs'] },
			// The site's own 404 page, with the crystal, is src/pages/404.astro.
			disable404Route: true,
			tableOfContents: { minHeadingLevel: 2, maxHeadingLevel: 3 },
			components: {
				Header: './src/components/Header.astro',
				ThemeSelect: './src/components/ThemeToggle.astro',
				PageTitle: './src/components/PageTitle.astro',
				Footer: './src/components/DocsFooter.astro',
			},
			head: [
				{ tag: 'meta', attrs: { property: 'og:image', content: `${SITE}/social.png` } },
				{ tag: 'meta', attrs: { property: 'og:image:width', content: '1200' } },
				{ tag: 'meta', attrs: { property: 'og:image:height', content: '630' } },
				{ tag: 'meta', attrs: { property: 'og:image:alt', content: 'The Calx logo beside a crystal drawn in dots' } },
				{ tag: 'meta', attrs: { name: 'twitter:card', content: 'summary_large_image' } },
			],
		}),
	],
	vite: {
		// The docs, the grammar and the art live outside site/.
		server: { fs: { allow: ['..'] } },
	},
});
