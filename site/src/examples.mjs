// Code the custom pages show, taken from the docs at build time rather than copied, so the home
// page's examples are the docs' tested ones and change when they do.
import { readFileSync } from 'node:fs';
import { DOCS } from './paths.mjs';

/** Every fenced block in a docs page, in order: [{ lang, code, line }], line being the first code line's, from 1. */
export function fences(page) {
	const lines = readFileSync(new URL(page, DOCS), 'utf8').replace(/\r\n/g, '\n').split('\n');
	const out = [];
	for (let i = 0; i < lines.length; i++) {
		const open = lines[i].match(/^```(\w*)\s*$/);
		if (!open) continue;
		let j = i + 1;
		while (j < lines.length && lines[j].trim() !== '```') j++;
		out.push({ lang: open[1], code: lines.slice(i + 1, j).join('\n'), line: i + 2 });
		i = j;
	}
	return out;
}

/** The lines an example says it prints, from its `// out:` comments. */
export function outputOf(code) {
	return [...code.matchAll(/\/\/ out: (.*)$/gm)].map((m) => m[1]);
}

/** The error a failing example is marked with: its line and column in the example, the line's
 *  code without the comment, and the message. Columns count from 1, at the line's first token,
 *  which is where the checker points for the examples the site shows. */
export function errorOf(code) {
	const lines = code.split('\n');
	for (let i = 0; i < lines.length; i++) {
		const m = lines[i].match(/^(.*?)\s*\/\/ error: (.*)$/);
		if (!m) continue;
		const column = m[1].search(/\S/) + 1;
		const token = m[1].slice(column - 1).match(/^\w+/)?.[0] ?? m[1].trim();
		return { line: i + 1, column, text: m[1], token, message: m[2] };
	}
	return null;
}
