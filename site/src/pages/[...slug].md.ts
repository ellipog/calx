// Each docs page's Markdown beside it: /reference/parallel.md for /reference/parallel/, for
// language models and the tools that fetch docs for them (src/llms.mjs).
import type { APIRoute } from 'astro';
// @ts-expect-error: a plain .mjs module.
import { listedPages, pageMarkdown } from '../llms.mjs';

export function getStaticPaths() {
	return listedPages().map((p: { slug: string }) => ({ params: { slug: p.slug } }));
}

export const GET: APIRoute = ({ params }) =>
	new Response(pageMarkdown(params.slug), { headers: { 'Content-Type': 'text/markdown; charset=utf-8' } });
