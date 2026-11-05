// /llms.txt: what Calx is, and every docs page, for language models (src/llms.mjs).
import type { APIRoute } from 'astro';
// @ts-expect-error: a plain .mjs module.
import { llmsTxt } from '../llms.mjs';

export const GET: APIRoute = () => new Response(llmsTxt(), { headers: { 'Content-Type': 'text/plain; charset=utf-8' } });
