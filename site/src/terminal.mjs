// What the install scripts draw in a terminal, on the install page: a recording of each, made by
// src/terminal/record.py, played back as text in the site's colors, with the mark's and the bars'
// shades as the site's dither. docs/guide/install.md asks for it with a comment of its own on a
// line, `<!-- terminal: install -->`, which GitHub doesn't show; src/markdown.mjs puts this in its
// place. Without JavaScript, or with reduced motion, it shows how each recording ends, and plays
// only when asked.
//
// One emulator, Term, does both: here, as the site builds, it draws how each recording ends, and
// in the browser it draws each frame as it plays. The browser gets its source as it is, from
// Function.prototype.toString, so there's one copy of it.
import { readFileSync } from 'node:fs';

/** A terminal as wide as it needs to be, which understands what the install scripts write: colors
 *  (SGR, with 24-bit ones), clearing a line (K), moving up (A), \r and \n. */
class Term {
	constructor() {
		this.lines = [[]];
		this.row = 0;
		this.col = 0;
		this.style = '';
		this.wide = 0;
		this.pending = '';
	}

	// The style of what's written next, as the class names the site's CSS gives each color, from
	// the SGR codes the scripts use, and inline colors for any others.
	sgr(params) {
		const p = params ? params.split(';').map(Number) : [0];
		const s = new Map(this.style ? this.style.split(' ').map((c) => [c.split('=')[0], c]) : []);
		const rgb = (r, g, b) => (r === 188 && g === 133 && b === 54 ? 'a' : r === 24 && g === 24 && b === 27 ? 'k' : `#${((1 << 24) | (r << 16) | (g << 8) | b).toString(16).slice(1)}`);
		const ANSI = { 31: 'r', 32: 'g', 33: 'y', 90: 'd', 37: 'w', 97: 'w' };
		for (let i = 0; i < p.length; i++) {
			const n = p[i];
			if (n === 0) s.clear();
			else if (n === 1) s.set('b', 'b');
			else if (n === 22) s.delete('b');
			else if (n === 39) s.delete('f');
			else if (n === 49) s.delete('g');
			else if (ANSI[n]) s.set('f', `f=${ANSI[n]}`);
			else if ((n === 38 || n === 48) && p[i + 1] === 2) {
				s.set(n === 38 ? 'f' : 'g', `${n === 38 ? 'f' : 'g'}=${rgb(p[i + 2], p[i + 3], p[i + 4])}`);
				i += 4;
			}
		}
		this.style = [...s.values()].join(' ');
	}

	put(ch) {
		const line = this.lines[this.row];
		while (line.length < this.col) line.push([' ', '']);
		line[this.col] = [ch, this.style];
		this.col++;
		this.wide = Math.max(this.wide, this.col);
	}

	feed(text) {
		const s = this.pending + text;
		this.pending = '';
		for (let i = 0; i < s.length; i++) {
			const c = s[i];
			if (c === '\x1b') {
				const m = /^\x1b\[([0-9;?]*)([A-Za-z])/.exec(s.slice(i, i + 64));
				if (!m) {
					this.pending = s.slice(i);
					return;
				}
				const n = parseInt(m[1], 10);
				if (m[2] === 'm') this.sgr(m[1]);
				else if (m[2] === 'K') this.lines[this.row] = m[1] === '2' || m[1] === '1' ? [] : this.lines[this.row].slice(0, this.col);
				else if (m[2] === 'A') this.row = Math.max(0, this.row - (n || 1));
				i += m[0].length - 1;
			} else if (c === '\r') this.col = 0;
			else if (c === '\n') {
				this.row++;
				this.col = 0;
				while (this.lines.length <= this.row) this.lines.push([]);
			} else this.put(c);
		}
	}

	/** The lines with anything on them, and those between. */
	get rows() {
		let n = this.lines.length;
		while (n > 0 && this.lines[n - 1].length === 0) n--;
		return n;
	}

	/** The screen as HTML: a block for each line, so an empty one keeps its height, and a span for
	 *  each run of one style. A run of the scripts' shades, ░ ▒ ▓ and █, is a span the CSS fills
	 *  with the dither of that shade, the character kept for copying. */
	html() {
		const esc = (t) => t.replace(/&/g, '&amp;').replace(/</g, '&lt;');
		const shade = (ch) => '░▒▓█'.indexOf(ch) + 1;
		const out = [];
		for (let r = 0; r < this.rows; r++) {
			const line = this.lines[r];
			let html = '';
			for (let i = 0; i < line.length; ) {
				const [ch, st] = line[i];
				const q = shade(ch);
				let text = '';
				while (i < line.length && line[i][1] === st && shade(line[i][0]) === q) text += line[i++][0];
				const cls = st
					.split(' ')
					.filter(Boolean)
					.map((c) => (c.includes('=#') ? '' : c.replace('=', '-')))
					.filter(Boolean);
				if (q) cls.push('q', `q${q}`);
				const colors = st
					.split(' ')
					.filter((c) => c.includes('=#'))
					.map((c) => (c[0] === 'f' ? `--fg:${c.slice(2)}` : `--bg:${c.slice(2)}`))
					.join(';');
				html += cls.length || colors ? `<span${cls.length ? ` class="${cls.join(' ')}"` : ''}${colors ? ` style="${colors}"` : ''}>${esc(text)}</span>` : esc(text);
			}
			out.push(`<span class="l">${html || ' '}</span>`);
		}
		return out.join('');
	}
}

// The player, in the browser: each figure plays its recording when it first comes into view, as
// it would in a terminal, then again from its replay button or a tab.
function player(Term) {
	const still = matchMedia('(prefers-reduced-motion: reduce)').matches;
	for (const fig of document.querySelectorAll('.calx-term:not([data-ready])')) {
		fig.dataset.ready = '';
		const recs = JSON.parse(fig.querySelector('script[type="application/json"]').textContent);
		const screens = [...fig.querySelectorAll('.calx-screen')];
		const tabs = [...fig.querySelectorAll('.calx-tab')];
		let shown = 0;
		let timer = 0;
		const play = (k) => {
			cancelAnimationFrame(timer);
			const { title, prompt, frames } = recs[k];
			const t = new Term();
			// The command is typed first, a character each 18 ms, and what it wrote plays 250 ms
			// after, at the times it came.
			const typing = title.length * 18 + 250;
			let typed = -1;
			let next = -1;
			let begin = 0;
			const step = (now) => {
				begin ||= now;
				const at = now - begin;
				let changed = false;
				const want = Math.min(title.length, Math.floor(at / 18));
				if (want !== typed) {
					typed = want;
					t.feed(`\r\x1b[2K\x1b[90m${prompt}\x1b[0m${title.slice(0, typed)}`);
					changed = true;
				}
				if (at >= typing && next < 0) {
					t.feed('\n');
					next = 0;
				}
				while (next >= 0 && next < frames.length && frames[next][0] <= at - typing) {
					t.feed(frames[next++][1]);
					changed = true;
				}
				if (changed) screens[k].innerHTML = t.html();
				if (next < frames.length) timer = requestAnimationFrame(step);
			};
			timer = requestAnimationFrame(step);
		};
		const show = (k) => {
			shown = k;
			tabs.forEach((tab, j) => tab.setAttribute('aria-selected', String(j === k)));
			screens.forEach((s, j) => (s.hidden = j !== k));
		};
		tabs.forEach((tab, k) =>
			tab.addEventListener('click', () => {
				if (k === shown) return;
				cancelAnimationFrame(timer);
				screens[shown].innerHTML = screens[shown].dataset.end;
				show(k);
				play(k);
			}),
		);
		screens.forEach((s) => (s.dataset.end = s.innerHTML));
		fig.querySelector('.calx-replay').addEventListener('click', () => play(shown));
		if (still) continue;
		const seen = new IntersectionObserver(
			(entries) => {
				if (!entries.some((e) => e.isIntersecting)) return;
				seen.disconnect();
				play(shown);
			},
			{ threshold: 0.6 },
		);
		seen.observe(fig);
	}
}

// Which recordings each comment names, in its tabs' order.
const SETS = {
	install: [
		['Windows', 'install-windows.json'],
		['Linux', 'install-linux.json'],
	],
};

const attr = (t) => t.replace(/&/g, '&amp;').replace(/"/g, '&quot;').replace(/</g, '&lt;');

/** The figure that takes the place of `<!-- terminal: NAME -->`, as one line of HTML, since
 *  Markdown ends an HTML block at an empty line. */
export function terminalHtml(name) {
	const set = SETS[name];
	if (!set) throw new Error(`docs ask for the terminal "${name}", which src/terminal.mjs doesn't have`);
	const recs = set.map(([label, file]) => ({ label, ...JSON.parse(readFileSync(new URL(`terminal/${file}`, import.meta.url), 'utf8')) }));
	// Each recording's end, as the site draws it, and how wide and tall the screen has to be for all
	// of them, so it doesn't change size as one plays or another tab shows.
	let cols = 0;
	let rows = 0;
	const ends = recs.map((rec) => {
		const t = new Term();
		t.feed(`\x1b[90m${rec.prompt}\x1b[0m${rec.title}\n`);
		for (const [, text] of rec.frames) t.feed(text);
		cols = Math.max(cols, t.wide);
		rows = Math.max(rows, t.rows);
		return t.html();
	});
	const data = JSON.stringify(recs.map(({ title, prompt, frames }) => ({ title, prompt, frames }))).replace(/</g, '\\u003c');
	const tabs = recs
		.map((r, k) => `<button class="calx-tab" type="button" role="tab" aria-selected="${k === 0}">${attr(r.label)}</button>`)
		.join('');
	const screens = recs
		.map((r, k) => `<pre class="calx-screen" aria-label="${attr(`What the install script prints on ${r.label}`)}"${k ? ' hidden' : ''}>${ends[k]}</pre>`)
		.join('');
	// The player's source, without its empty lines, which would end the HTML block.
	const code = `(${player})(${Term})`.replace(/\n\s*(?=\n)/g, '');
	const when = recs[0].recorded;
	const caption = `The install scripts in a terminal, recorded with the nightly on ${new Date(`${when}T12:00:00Z`).toLocaleDateString('en-GB', { day: 'numeric', month: 'long', year: 'numeric', timeZone: 'UTC' })}. A step that takes more than a few seconds plays faster.`;
	return (
		`<figure class="calx-term not-content" style="--cols:${cols};--rows:${rows}">` +
		`<div class="calx-head"><div class="calx-tabs" role="tablist" aria-label="Show">${tabs}</div>` +
		`<div class="calx-right"><button class="calx-replay" type="button">Replay</button></div></div>` +
		screens +
		`<figcaption>${attr(caption)}</figcaption>` +
		`<script type="application/json">${data}</script>` +
		`<script type="module">${code}</script>` +
		`</figure>`
	);
}
