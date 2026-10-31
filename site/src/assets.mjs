// The site's art comes from assets/logo, where banner.py makes it, rather than copies in site/.
// This serves each file at a fixed URL while developing and writes it into the build, so links
// to it (the og:image, the favicon) stay the same from build to build.
import { readFileSync } from 'node:fs';
import { mkdir, writeFile } from 'node:fs/promises';
import { REPO } from './paths.mjs';

const LOGO = new URL('assets/logo/', REPO);

// The mark takes the text color, so as a favicon it gets the theme's ink by the browser's scheme.
const favicon = () =>
	readFileSync(new URL('calx-mark.svg', LOGO), 'utf8').replace(
		/(<svg[^>]*>)/,
		'$1<style>svg{color:#1E1F24}@media (prefers-color-scheme:dark){svg{color:#ECE6DA}}</style>',
	);

export const FILES = {
	'/favicon.svg': { type: 'image/svg+xml', read: favicon },
	'/social.png': { type: 'image/png', read: () => readFileSync(new URL('calx-social.png', LOGO)) },
	'/crystal-tones.jpg': { type: 'image/jpeg', read: () => readFileSync(new URL('calx-tones.jpg', LOGO)) },
};

export function calxAssets() {
	return {
		name: 'calx-assets',
		hooks: {
			'astro:server:setup': ({ server }) => {
				server.middlewares.use((req, res, next) => {
					const file = FILES[(req.url ?? '').split('?')[0]];
					if (!file) return next();
					res.setHeader('Content-Type', file.type);
					res.setHeader('Cache-Control', 'no-cache');
					res.end(file.read());
				});
			},
			'astro:build:done': async ({ dir }) => {
				await mkdir(dir, { recursive: true });
				for (const [path, file] of Object.entries(FILES)) await writeFile(new URL('.' + path, dir), file.read());
			},
		},
	};
}
