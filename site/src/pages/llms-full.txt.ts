// /llms-full.txt: every docs page in one file, for language models (src/llms.mjs).
import type { APIRoute } from 'astro';
// @ts-expect-error: a plain .mjs module.
import { llmsFullTxt } from '../llms.mjs';

export const GET: APIRoute = () => new Response(llmsFullTxt(), { headers: { 'Content-Type': 'text/plain; charset=utf-8' } });
