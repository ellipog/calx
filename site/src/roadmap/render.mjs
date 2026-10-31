// The roadmap's pieces as HTML. The build renders the page with these, and the browser renders them
// again as the board changes, so a card looks the same either way. Everything from Acta is text
// someone typed, so all of it is escaped, and only links to web pages and to this site survive.

const ESC = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' };
export const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ESC[c]);

const dayFmt = new Intl.DateTimeFormat('en', { month: 'short', day: 'numeric', year: 'numeric', timeZone: 'UTC' });
/** "Oct 28, 2026", from "2026-10-28" or a timestamp. */
export function day(value) {
	if (!value) return '';
	const d = new Date(/^\d{4}-\d{2}-\d{2}$/.test(value) ? `${value}T00:00:00Z` : value);
	return Number.isNaN(d.getTime()) ? '' : dayFmt.format(d);
}

const plural = (n, one, many = `${one}s`) => `${n} ${n === 1 ? one : many}`;

// ---------- links ----------

const DOCS_ON_GITHUB = /^https:\/\/github\.com\/ellipog\/calx\/blob\/main\/docs\/([\w./-]+)\.md(#[\w-]*)?$/;
const ACTA_BOARD = /^https:\/\/acta\.aaenz\.no\/aaen-studios\/clx(?:\/?#|\/?\?task=)([A-Z]+-\d+)$/i;

/**
 * Where a link in a task goes on this site. A link to the docs on GitHub goes to the docs here. The
 * board itself is private, so a link to one of its tasks opens the task here if it's on the roadmap,
 * and is left as plain text if it isn't, as is any other link into Acta's app.
 */
export function taskLink(url, ids) {
	const docs = url.match(DOCS_ON_GITHUB);
	if (docs) {
		const path = docs[1] === 'CALX_SPEC' ? 'spec' : docs[1] === 'README' ? 'docs' : docs[1];
		return `/${path}/${docs[2] ?? ''}`;
	}
	const task = url.match(ACTA_BOARD);
	if (task) return ids.has(task[1].toUpperCase()) ? `#${task[1].toUpperCase()}` : null;
	if (/^https:\/\/acta\.aaenz\.no\/(?!p\/|docs)/i.test(url)) return null;
	return url;
}

function safeHref(url) {
	const u = url.trim();
	if (/^(https?:\/\/|mailto:)/i.test(u) || /^[#/](?!\/)/.test(u)) return u;
	return null;
}

const external = (href) => /^https?:\/\//i.test(href);

// ---------- Markdown ----------

/**
 * Markdown as Acta's tasks use it: paragraphs, headings, lists and task lists, quotes, code, tables
 * and rules, with bold, italics, code, links and bare addresses inside them. Raw HTML stays text.
 *
 * - `href(url)` says where a link goes, or null to keep only its words.
 * - `ref(id)` says where a task id in the text, "CLX-12", links to, or null.
 * - `shift` is how many levels a heading goes down, so a task's "#" sits under the page's own.
 */
export function markdown(md, opts = {}) {
	const o = { href: (u) => u, ref: () => null, shift: 2, ...opts };
	return blocks(String(md ?? '').replace(/\r\n?/g, '\n').split('\n'), o);
}

const FENCE = /^\s{0,3}(`{3,}|~{3,})\s*([\w+#.-]*)/;
const HEADING = /^\s{0,3}(#{1,6})\s+(.*?)(?:\s+#+)?\s*$/;
const RULE = /^\s{0,3}([-*_])(?:\s*\1){2,}\s*$/;
const QUOTE = /^\s{0,3}>/;
const ITEM = /^(\s*)([-*+]|\d{1,9}[.)])\s+(.*)$/;
const TABLE_RULE = /^\s*\|?\s*:?-+:?\s*(\|\s*:?-+:?\s*)+\|?\s*$|^\s*\|\s*:?-+:?\s*\|\s*$/;

/** A table row's cells. A "|" inside `code`, or written "\|", is part of its cell. */
function splitRow(row) {
	const cells = [''];
	let ticks = 0;
	for (let k = 0; k < row.length; k++) {
		const c = row[k];
		if (c === '\\' && row[k + 1] === '|') {
			cells[cells.length - 1] += '|';
			k++;
		} else if (c === '`') {
			let run = 1;
			while (row[k + run] === '`') run++;
			ticks = ticks === 0 ? run : ticks === run ? 0 : ticks;
			cells[cells.length - 1] += '`'.repeat(run);
			k += run - 1;
		} else if (c === '|' && !ticks) cells.push('');
		else cells[cells.length - 1] += c;
	}
	return cells.map((c) => c.trim());
}

const startsBlock = (l) => FENCE.test(l) || HEADING.test(l) || RULE.test(l) || QUOTE.test(l) || ITEM.test(l);

function blocks(lines, o) {
	const out = [];
	let i = 0;
	while (i < lines.length) {
		const line = lines[i];
		if (!line.trim()) {
			i++;
			continue;
		}
		let m;
		if ((m = line.match(FENCE))) {
			const body = [];
			const close = new RegExp(`^\\s{0,3}${m[1][0]}{${m[1].length},}\\s*$`);
			for (i++; i < lines.length && !close.test(lines[i]); i++) body.push(lines[i]);
			i++;
			out.push(`<pre><code>${esc(body.join('\n'))}</code></pre>`);
			continue;
		}
		if ((m = line.match(HEADING))) {
			const level = Math.min(6, m[1].length + o.shift);
			out.push(`<h${level}>${inline(m[2], o)}</h${level}>`);
			i++;
			continue;
		}
		if (RULE.test(line)) {
			out.push('<hr>');
			i++;
			continue;
		}
		if (line.includes('|') && i + 1 < lines.length && TABLE_RULE.test(lines[i + 1])) {
			const cells = (l) => splitRow(l.trim().replace(/^\|/, '').replace(/\|$/, ''));
			const head = cells(line);
			const rows = [];
			for (i += 2; i < lines.length && lines[i].includes('|') && lines[i].trim(); i++) rows.push(cells(lines[i]));
			out.push(
				`<div class="rm-table"><table><thead><tr>${head.map((c) => `<th>${inline(c, o)}</th>`).join('')}</tr></thead><tbody>` +
					rows.map((r) => `<tr>${head.map((_, k) => `<td>${inline(r[k] ?? '', o)}</td>`).join('')}</tr>`).join('') +
					'</tbody></table></div>'
			);
			continue;
		}
		if (QUOTE.test(line)) {
			const body = [];
			for (; i < lines.length && QUOTE.test(lines[i]); i++) body.push(lines[i].replace(/^\s{0,3}>\s?/, ''));
			out.push(`<blockquote>${blocks(body, o)}</blockquote>`);
			continue;
		}
		if ((m = line.match(ITEM))) {
			const indent = m[1].length;
			const ordered = /\d/.test(m[2]);
			const items = [];
			let loose = false;
			while (i < lines.length) {
				const l = lines[i];
				const im = l.match(ITEM);
				if (im && im[1].length <= indent + 1 && /\d/.test(im[2]) === ordered) {
					items.push([im[3]]);
					i++;
				} else if (!l.trim()) {
					const next = lines[i + 1] ?? '';
					const nm = next.match(ITEM);
					if (/^\s{2,}\S/.test(next) && !(nm && nm[1].length <= indent + 1)) items.at(-1).push('');
					else if (nm && nm[1].length <= indent + 1 && /\d/.test(nm[2]) === ordered) loose = true;
					else break;
					i++;
				} else if (/^\s+\S/.test(l) || !startsBlock(l)) {
					// A line indented under the item, or one that carries its sentence on.
					items.at(-1).push(l.replace(new RegExp(`^\\s{1,${indent + 4}}`), ''));
					i++;
				} else break;
			}
			const tag = ordered ? 'ol' : 'ul';
			const first = ordered ? Number.parseInt(m[2], 10) : 1;
			out.push(
				`<${tag}${ordered && first !== 1 ? ` start="${first}"` : ''}>` +
					items
						.map((it) => {
							const box = it[0].match(/^\[([ xX])\]\s+(.*)$/);
							if (box) it[0] = box[2];
							let html = blocks(it, o);
							if (!loose && !it.includes('')) html = html.replace(/^<p>([\s\S]*?)<\/p>/, '$1');
							return box
								? `<li class="rm-check" data-done="${box[1] !== ' '}"><span class="sr-only">${box[1] !== ' ' ? 'Done: ' : 'To do: '}</span>${html}</li>`
								: `<li>${html}</li>`;
						})
						.join('') +
					`</${tag}>`
			);
			continue;
		}
		const para = [];
		for (; i < lines.length && lines[i].trim() && !startsBlock(lines[i]); i++) {
			if (para.length && lines[i].includes('|') && TABLE_RULE.test(lines[i + 1] ?? '')) break;
			para.push(lines[i].trim());
		}
		out.push(`<p>${inline(para.join('\n'), o)}</p>`);
	}
	return out.join('');
}

/** A line of Markdown as HTML. Code, links and addresses are set aside first, so nothing in them is
 *  read as emphasis, then the rest is escaped and marked up, and they go back in. */
function inline(text, o, { links = true } = {}) {
	const kept = [];
	const keep = (html) => `\u0000${kept.push(html) - 1}\u0000`;
	let s = String(text).replace(/\u0000/g, '');
	s = s.replace(/(`+)([^`]|[^`][\s\S]*?[^`])\1(?!`)/g, (_, _t, code) => keep(`<code>${esc(code.trim())}</code>`));
	s = s.replace(/\[((?:[^\]\\]|\\.)+)\]\(\s*<?([^)\s>]+)>?(?:\s+"[^"]*")?\s*\)/g, (_, label, url) => {
		const words = inline(label, o, { links: false });
		const mapped = links ? o.href(url) : null;
		const href = mapped && safeHref(mapped);
		if (!href) return keep(words);
		return keep(`<a href="${esc(href)}"${external(href) ? ' rel="noopener"' : ''}>${words}</a>`);
	});
	s = s.replace(/<(https?:\/\/[^\s>]+)>|\bhttps?:\/\/[^\s<]*[^\s<.,;:!?)\]'"]/g, (all, angled) => {
		const url = angled ?? all;
		const mapped = links ? o.href(url) : null;
		const href = mapped && safeHref(mapped);
		return keep(href ? `<a href="${esc(href)}"${external(href) ? ' rel="noopener"' : ''}>${esc(url)}</a>` : esc(url));
	});
	s = esc(s)
		.replace(/\\([\\`*_{}[\]()#+\-.!|~>])/g, '$1')
		.replace(/\*\*(?=\S)([\s\S]*?\S)\*\*/g, '<strong>$1</strong>')
		.replace(/__(?=\S)([\s\S]*?\S)__(?!\w)/g, '<strong>$1</strong>')
		.replace(/(^|[^\w*])\*(?=[^\s*])([\s\S]*?[^\s*])\*(?![\w*])/g, '$1<em>$2</em>')
		.replace(/(^|[^\w])_(?=\S)([\s\S]*?\S)_(?!\w)/g, '$1<em>$2</em>')
		.replace(/~~(?=\S)([\s\S]*?\S)~~/g, '<del>$1</del>');
	if (links)
		s = s.replace(/\b([A-Z][A-Z0-9]{1,9}-\d{1,6})\b/g, (id) => {
			const href = o.ref(id);
			return href ? `<a href="${esc(href)}">${id}</a>` : id;
		});
	return s.replace(/\u0000(\d+)\u0000/g, (_, n) => kept[Number(n)]);
}

/** One line of Markdown, for a list item from docs/roadmap.md or a title. */
export function markdownLine(text, opts = {}) {
	return inline(text, { href: (u) => u, ref: () => null, ...opts });
}

// ---------- the board ----------

const SVG = (id, cls = 'i') => `<svg class="${cls}" aria-hidden="true"><use href="#${id}"></use></svg>`;

/** "In review" for work that's done and waiting for a look, which the board counts as in progress. */
export const statusNote = (t) => (t.status === 'review' ? 'In review' : t.status === 'in_progress' ? 'Being built' : '');

function progress(t) {
	const s = t.subtasks;
	if (!s || !s.total) return '';
	const pct = Math.round((s.done / s.total) * 100);
	return `<span class="rm-prog" title="${esc(`${s.done} of ${s.total} steps done`)}"><span class="rm-bar"><i style="width:${pct}%"></i></span>${s.done}/${s.total}</span>`;
}

/**
 * A vote button, or the count alone where this visitor can't vote. `can` is whether they can.
 * The count is the number, and the button says whose it is to anyone who can't see it.
 */
export function voteButton(kind, item, can, { wide = false } = {}) {
	const n = item.votes ?? 0;
	if (item.votes === null) return '';
	const label = `${item.voted ? 'Take back your vote for' : 'Vote for'} “${item.title}”, ${plural(n, 'vote')}`;
	if (!can)
		return n > 0
			? `<span class="rm-votes" title="${esc(plural(n, 'vote'))}">${SVG('i-up')}<span class="n">${n}</span></span>`
			: '';
	return `<button type="button" class="rm-vote${wide ? ' wide' : ''}" data-vote="${kind}" data-id="${esc(item.id)}" aria-pressed="${item.voted ? 'true' : 'false'}" aria-label="${esc(label)}">${SVG('i-up')}${wide ? `<span class="w">${item.voted ? 'Voted' : 'Vote'}</span>` : ''}<span class="n">${n}</span></button>`;
}

/** A task's card. The whole card opens the task; the vote button sits above that link. */
export function card(t, ctx) {
	const note = statusNote(t);
	const meta = [
		note && `<span class="rm-note" data-status="${esc(t.status)}">${note}</span>`,
		t.stage === 'shipped' && t.shipped && `<span class="rm-date">${esc(day(t.shipped))}</span>`,
		t.stage !== 'shipped' && progress(t),
	].filter(Boolean);
	return (
		`<li class="rm-card" data-id="${esc(t.id)}" data-stage="${esc(t.stage)}">` +
		`<a class="rm-open" href="#${esc(t.id)}"><span class="rm-id">${esc(t.id)}</span><span class="rm-title">${markdownLine(t.title, { href: () => null })}</span></a>` +
		(meta.length ? `<div class="rm-meta">${meta.join('')}</div>` : '') +
		(t.stage !== 'shipped' ? voteButton('task', t, ctx.canVote) : '') +
		'</li>'
	);
}

/** Everything about one task, for the dialog a card opens. */
export function detail(t, ctx) {
	const stage = ctx.stages[t.stage] ?? t.stage;
	const note = statusNote(t);
	const s = t.subtasks;
	const facts = [
		t.stage === 'shipped' && t.shipped && `Shipped ${esc(day(t.shipped))}`,
		s && s.total > 0 && `${progress(t)}<span>${esc(`${s.done} of ${s.total} steps done`)}</span>`,
	].filter(Boolean);
	const body = t.description.trim()
		? markdown(t.description, { href: (u) => taskLink(u, ctx.ids), ref: (id) => (ctx.ids.has(id) && id !== t.id ? `#${id}` : null) })
		: '<p class="rm-empty">No description yet.</p>';
	return (
		`<div class="rm-d-head"><span class="rm-id">${esc(t.id)}</span><span class="rm-pill" data-stage="${esc(t.stage)}">${esc(stage)}${note ? ` · ${note}` : ''}</span></div>` +
		`<h2 id="rm-d-title">${markdownLine(t.title, { href: () => null })}</h2>` +
		(facts.length ? `<div class="rm-d-facts">${facts.map((f) => `<span>${f}</span>`).join('')}</div>` : '') +
		`<div class="rm-md">${body}</div>` +
		`<div class="rm-d-foot">${t.stage !== 'shipped' ? voteButton('task', t, ctx.canVote, { wide: true }) : ''}` +
		`<button type="button" class="rm-copy" data-copy="${esc(t.id)}">${SVG('i-link')}<span>Copy link</span></button></div>`
	);
}

/** An idea someone sent. Its details are plain text. */
export function ideaItem(i, ctx) {
	const mine = ctx.mine?.has(i.id);
	return (
		`<li class="rm-idea" data-idea="${esc(i.id)}">` +
		(voteButton('idea', i, ctx.canVote) || '<span class="rm-votes empty"></span>') +
		`<div class="rm-idea-body"><b>${esc(i.title)}</b>` +
		(i.details ? `<p>${esc(i.details)}</p>` : '') +
		`<small>${esc(i.by || 'A visitor')} · ${esc(day(i.at))}${mine ? ' · <span class="rm-mine">You sent this</span>' : ''}</small></div>` +
		'</li>'
	);
}

// ---------- finding things ----------

const STOP = new Set(
	'the and for with from that this into when what which have has are was were not but can its it’s their them then than there out add make use using a an to of in on at by as be is or'.split(' ')
);
const words = (s) =>
	String(s)
		.toLowerCase()
		.replace(/[`*_[\]()]/g, ' ')
		.split(/[^\p{L}\p{N}.<>-]+/u)
		.map((w) => w.replace(/^[.<>-]+|[.<>-]+$/g, ''))
		.filter((w) => w.length > 2 && !STOP.has(w));

/** What on the board or among the ideas looks like what someone is about to write: up to `n`,
 *  best first. A word in a title counts more than one in a description. */
export function similar(text, tasks, ideas, n = 3) {
	const want = [...new Set(words(text))];
	if (!want.length) return [];
	const score = (title, rest) => {
		const t = new Set(words(title));
		const r = new Set(words(rest));
		let s = 0;
		for (const w of want) s += t.has(w) ? 2 : [...t].some((x) => x.startsWith(w) || w.startsWith(x)) ? 1.5 : r.has(w) ? 0.5 : 0;
		return s / Math.max(2, want.length);
	};
	return [
		...tasks.map((t) => ({ kind: 'task', id: t.id, title: t.title, stage: t.stage, s: score(t.title, t.description) })),
		...ideas.map((i) => ({ kind: 'idea', id: i.id, title: i.title, s: score(i.title, i.details ?? '') })),
	]
		.filter((x) => x.s >= 0.5)
		.sort((a, b) => b.s - a.s)
		.slice(0, n);
}

/** Whether a task matches what's typed in the board's search: its id, or every word somewhere in it. */
export function matches(t, query) {
	const q = query.trim().toLowerCase();
	if (!q) return true;
	if (t.id.toLowerCase() === q || `#${t.id.toLowerCase()}` === q) return true;
	const hay = `${t.id} ${t.title} ${t.description}`.toLowerCase();
	return q.split(/\s+/).every((w) => hay.includes(w));
}
