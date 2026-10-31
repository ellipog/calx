// The roadmap in the browser. The build rendered the board as it was then; this keeps it current,
// and makes it something to use: search and sort, a task's details in a dialog with its own link,
// votes, ideas, and the issue form's way to GitHub.
//
// - With a roadmap key (PUBLIC_ACTA_ROADMAP_KEY), it reads the board through Acta's roadmap API,
//   as this visitor, so it knows what they voted for, and they can vote and send ideas. A visitor
//   is a random id this browser keeps in localStorage; Acta keeps only a hash of it.
// - Without one, or if the key stops working, it reads the keyless feed, and the board is read only.
//
// It reads the board again every minute while the page is in view.

import { API, ActaError, STAGES, acta, readApi, readFeed } from '../roadmap/acta.mjs';
import { card, detail, esc, ideaItem, matches, similar } from '../roadmap/render.mjs';

const KEY = import.meta.env.PUBLIC_ACTA_ROADMAP_KEY || '';
const FIRST = 10; // cards a long column shows until "Show all"
const EVERY = 60_000; // how often the board is read again
const MAX_URL = 7500; // GitHub turns away a longer address
const TASK_ID = /^[A-Z][A-Z0-9]{1,9}-\d{1,6}$/;

const root = document.querySelector('[data-roadmap]');
if (root) start(root);

function start(root) {
	root.classList.add('js');
	const $ = (s, el = root) => el.querySelector(s);
	const $$ = (s, el = root) => [...el.querySelectorAll(s)];

	// ---------- state ----------

	const embedded = JSON.parse($('#rm-data').textContent);
	let data = embedded.data; // the Roadmap from acta.mjs, or null until the first read
	let live = false; // read through the API, as this visitor
	let keyFailed = false;
	let tried = false; // the first read has finished, one way or the other
	let lastRead = 0;
	let reading = null;
	const expanded = new Set();
	const pending = new Set(); // votes on their way, "task:CLX-3"
	const view = { query: '', sort: 'position', ideas: 'top' };

	const store = {
		get(name, fallback) {
			try {
				return JSON.parse(localStorage.getItem(name)) ?? fallback;
			} catch {
				return fallback;
			}
		},
		set(name, value) {
			try {
				localStorage.setItem(name, JSON.stringify(value));
			} catch {
				// Private windows may not keep it. The page works without, minus remembering.
			}
		},
	};
	function visitor() {
		let id = store.get('calx-roadmap-visitor', '');
		if (!/^[A-Za-z0-9-]{8,64}$/.test(id)) {
			id = crypto.randomUUID?.() ?? Array.from(crypto.getRandomValues(new Uint8Array(16)), (b) => b.toString(16).padStart(2, '0')).join('');
			store.set('calx-roadmap-visitor', id);
		}
		return id;
	}
	const mine = new Set(store.get('calx-roadmap-mine', []));

	const canVote = () => live && !!data?.page.voting;
	const takesIdeas = () => live && !!data?.page.ideas.open;
	const ctx = () => ({
		canVote: canVote(),
		stages: data?.page.stages ?? {},
		ids: new Set((data?.tasks ?? []).map((t) => t.id)),
		mine,
	});
	const taskById = (id) => data?.tasks.find((t) => t.id === id);
	const ideaById = (id) => data?.ideas.find((i) => i.id === id);

	// ---------- reading the board ----------

	async function read() {
		if (reading) return reading;
		reading = (async () => {
			try {
				let next;
				if (KEY && !keyFailed) {
					try {
						next = await readApi(KEY, visitor());
					} catch (e) {
						// A key that's wrong or not for this site won't start working: read the feed from now on.
						if (e instanceof ActaError && (e.status === 401 || e.status === 403)) {
							keyFailed = true;
							console.warn(`[roadmap] The roadmap key didn't work, so the board is read only: ${e.message}`);
						}
						next = await readFeed();
					}
				} else next = await readFeed();
				// A vote on its way keeps what the visitor sees until it lands.
				for (const p of pending) {
					const [kind, id] = p.split(/:(.*)/);
					const was = kind === 'task' ? taskById(id) : ideaById(id);
					const now = kind === 'task' ? next.tasks.find((t) => t.id === id) : next.ideas.find((i) => i.id === id);
					if (was && now) Object.assign(now, { votes: was.votes, voted: was.voted });
				}
				data = next;
				live = next.live;
				lastRead = Date.now();
				status('live');
			} catch (e) {
				console.warn('[roadmap] The board couldn\'t be read:', e);
				status('error');
			} finally {
				reading = null;
				tried = true;
				render();
			}
		})();
		return reading;
	}

	const time = (ms) => new Date(ms).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
	function status(state) {
		const el = $('.rm-status');
		el.dataset.state = state;
		el.lastElementChild.textContent =
			state === 'live'
				? `Updated ${time(lastRead)}`
				: lastRead
					? `Couldn't reach the board. As of ${time(lastRead)}`
					: data
						? "Couldn't reach the board. This is how it was when the site was built."
						: "Couldn't reach the board. Try again later.";
	}

	setInterval(() => {
		if (!document.hidden) read();
	}, EVERY);
	document.addEventListener('visibilitychange', () => {
		if (!document.hidden && Date.now() - lastRead > EVERY / 2) read();
	});

	// ---------- the board ----------

	const ORDER = { in_progress: 0, review: 1 };
	function column(stage) {
		const all = (data?.tasks ?? []).map((t, i) => ({ t, i })).filter((x) => x.t.stage === stage);
		const byBoard = (a, b) => {
			if (stage === 'progress') return (ORDER[a.t.status] ?? 2) - (ORDER[b.t.status] ?? 2) || a.i - b.i;
			if (stage === 'shipped') return (b.t.shipped ?? '').localeCompare(a.t.shipped ?? '') || a.i - b.i;
			return a.i - b.i;
		};
		const sort =
			view.sort === 'votes'
				? (a, b) => (b.t.votes ?? 0) - (a.t.votes ?? 0) || byBoard(a, b)
				: view.sort === 'newest'
					? (a, b) => b.t.number - a.t.number
					: byBoard;
		return {
			total: all.length,
			shown: all
				.filter((x) => matches(x.t, view.query))
				.sort(sort)
				.map((x) => x.t),
		};
	}

	// innerHTML only where it changed, and focus back where it was, so a read every minute doesn't
	// move anyone's place on the page.
	function fill(el, html) {
		if (el.innerHTML === html) return;
		const active = document.activeElement;
		const where = el.contains(active) ? focusKey(active) : null;
		el.innerHTML = html;
		if (where) el.querySelector(where)?.focus();
	}
	function focusKey(el) {
		if (el.dataset.vote) return `[data-vote="${el.dataset.vote}"][data-id="${CSS.escape(el.dataset.id)}"]`;
		if (el.classList.contains('rm-open')) return `.rm-open[href="${CSS.escape(el.getAttribute('href'))}"]`;
		if (el.dataset.copy) return '[data-copy]';
		return null;
	}
	const setCount = (name, n) => $$(`[data-count="${name}"]`).forEach((el) => (el.textContent = n));

	function render() {
		const c = ctx();
		const q = view.query.trim();
		let found = 0;
		for (const stage of STAGES) {
			const col = $(`.rm-col[data-stage="${stage}"]`);
			const { total, shown } = column(stage);
			found += shown.length;
			const open = q || expanded.has(stage);
			fill($('.rm-list', col), (open ? shown : shown.slice(0, FIRST)).map((t) => card(t, c)).join(''));
			const more = $('.rm-more', col);
			more.hidden = q || shown.length <= FIRST;
			more.textContent = expanded.has(stage) ? 'Show fewer' : `Show all ${shown.length}`;
			const none = $('.rm-none', col);
			none.hidden = shown.length > 0;
			none.textContent = !data ? '' : q ? 'Nothing here matches.' : 'Nothing here yet.';
			setCount(stage, q ? `${shown.length}/${total}` : total);
			$(`.rm-stats [data-total="${stage}"]`).textContent = total;
		}
		const foundEl = $('.rm-found');
		foundEl.hidden = !q;
		if (q)
			foundEl.innerHTML = `${found === 0 ? 'No tasks match' : found === 1 ? '1 task matches' : `${found} tasks match`} “${esc(q)}”. <button type="button" class="rm-link" data-clear>Clear</button>`;
		$('.rm-search input').placeholder = data ? `Search ${data.tasks.length} tasks` : 'Search tasks';

		const ann = $('.rm-announce');
		ann.hidden = !data?.page.announcement;
		ann.textContent = data?.page.announcement ?? '';

		renderIdeas(c);
		renderForms();
		if (dialog.open && shownId) {
			const t = taskById(shownId);
			if (t) fill(body, detail(t, c));
		}
	}

	// ---------- search and sort ----------

	const search = $('.rm-search input');
	const sortSel = $('.rm-sort select');
	const params = new URLSearchParams(location.search);
	if (params.get('q')) search.value = view.query = params.get('q');
	if (['votes', 'newest'].includes(params.get('sort'))) sortSel.value = view.sort = params.get('sort');

	function remember() {
		const p = new URLSearchParams(location.search);
		view.query.trim() ? p.set('q', view.query.trim()) : p.delete('q');
		view.sort !== 'position' ? p.set('sort', view.sort) : p.delete('sort');
		const s = p.toString();
		history.replaceState(history.state, '', `${location.pathname}${s ? `?${s}` : ''}${location.hash}`);
	}
	let typing;
	search.addEventListener('input', () => {
		view.query = search.value;
		render();
		clearTimeout(typing);
		typing = setTimeout(remember, 400);
	});
	search.addEventListener('keydown', (e) => {
		if (e.key === 'Escape' && search.value) {
			e.preventDefault();
			search.value = view.query = '';
			render();
			remember();
		}
	});
	sortSel.addEventListener('change', () => {
		view.sort = sortSel.value;
		render();
		remember();
	});
	// "/" goes to the search box from anywhere that isn't for typing.
	document.addEventListener('keydown', (e) => {
		if (e.key !== '/' || e.ctrlKey || e.metaKey || e.altKey || dialog.open) return;
		const t = e.target;
		if (t.closest?.('input, textarea, select, [contenteditable="true"]')) return;
		e.preventDefault();
		search.focus();
		search.select();
	});

	// ---------- phone tabs ----------

	const cols = $('.rm-cols');
	const tabs = $$('.rm-tabs [role="tab"]');
	function showStage(stage, focus = false) {
		cols.dataset.active = stage;
		for (const tab of tabs) {
			const on = tab.dataset.stage === stage;
			tab.setAttribute('aria-selected', String(on));
			tab.tabIndex = on ? 0 : -1;
			if (on && focus) tab.focus();
		}
	}
	tabs.forEach((tab, i) => {
		tab.addEventListener('click', () => showStage(tab.dataset.stage));
		tab.addEventListener('keydown', (e) => {
			const step = e.key === 'ArrowRight' ? 1 : e.key === 'ArrowLeft' ? -1 : 0;
			if (!step) return;
			e.preventDefault();
			showStage(tabs[(i + step + tabs.length) % tabs.length].dataset.stage, true);
		});
	});
	showStage(cols.dataset.active);

	// ---------- one task, in a dialog ----------

	const dialog = $('.rm-dialog');
	const body = $('.rm-d-body', dialog);
	let shownId = null;
	let pushed = false; // whether opening it added a page to the history, for Back to close it
	let opener = null;

	const hashId = () => {
		const h = decodeURIComponent(location.hash.slice(1)).toUpperCase();
		return TASK_ID.test(h) ? h : null;
	};
	function show(id) {
		const t = taskById(id);
		if (!t) {
			if (data) toast(`${id} isn't on the public roadmap.`);
			return false;
		}
		shownId = id;
		fill(body, detail(t, ctx()));
		document.title = `${id}: ${t.title} | Calx roadmap`;
		if (!dialog.open) {
			opener = document.activeElement;
			dialog.showModal();
			body.scrollTop = 0;
		} else dialog.scrollTop = 0;
		return true;
	}
	const pageTitle = document.title;
	// Closing takes the task's address away: Back, if opening it added a page to the history, so
	// Forward opens it again. `fromHistory` is when Back or Forward is what closed it.
	function hide(fromHistory = false) {
		if (dialog.open) dialog.close();
		if (shownId === null) return;
		const id = shownId;
		shownId = null;
		document.title = pageTitle;
		if (!fromHistory && hashId()) {
			if (pushed) history.back();
			else history.replaceState(history.state, '', location.pathname + location.search);
		}
		pushed = false;
		const back = opener?.isConnected && opener !== document.body ? opener : $(`.rm-open[href="#${CSS.escape(id)}"]`);
		back?.focus({ preventScroll: true });
		opener = null;
	}
	dialog.addEventListener('cancel', (e) => {
		e.preventDefault();
		hide();
	});
	// Anything else that closes it, such as a browser's own gesture. The event comes a moment after,
	// so it's for nothing if the dialog has opened again since.
	dialog.addEventListener('close', () => {
		if (!dialog.open) hide();
	});
	$('.rm-close', dialog).addEventListener('click', () => hide());
	// A click on the backdrop, which is the dialog itself: its content fills it.
	dialog.addEventListener('click', (e) => {
		if (e.target === dialog) hide();
	});

	function sync() {
		const id = hashId();
		if (id) {
			if (id !== shownId && !show(id)) hide(true);
		} else hide(true);
	}
	window.addEventListener('popstate', sync);
	window.addEventListener('hashchange', sync);

	// ---------- clicks ----------

	document.addEventListener('click', (e) => {
		const el = e.target.closest?.('a, button');
		if (!el || !root.contains(el)) return;

		// A link to a task opens it here. Inside the dialog it replaces what's open, so Back still closes it.
		if (el.tagName === 'A') {
			const href = el.getAttribute('href') ?? '';
			if (!href.startsWith('#') || e.ctrlKey || e.metaKey || e.shiftKey || e.button) return;
			const id = decodeURIComponent(href.slice(1)).toUpperCase();
			if (!TASK_ID.test(id) || !taskById(id)) return;
			e.preventDefault();
			if (dialog.open) {
				history.replaceState(history.state, '', `#${id}`);
				show(id);
			} else {
				history.pushState(history.state, '', `#${id}`);
				pushed = show(id);
			}
			return;
		}

		if (el.dataset.vote) return vote(el.dataset.vote, el.dataset.id);
		if (el.dataset.copy) return copyLink(el.dataset.copy);
		if (el.matches('.rm-more')) {
			const stage = el.closest('.rm-col').dataset.stage;
			expanded.has(stage) ? expanded.delete(stage) : expanded.add(stage);
			render();
			if (!expanded.has(stage)) el.closest('.rm-col').scrollIntoView({ block: 'nearest' });
			return;
		}
		if (el.matches('[data-clear]')) {
			search.value = view.query = '';
			render();
			remember();
			search.focus();
			return;
		}
		if (el.dataset.sort) {
			view.ideas = el.dataset.sort;
			$$('.rm-seg button').forEach((b) => b.setAttribute('aria-pressed', String(b === el)));
			renderIdeas(ctx());
			return;
		}
		if (el.dataset.gotoIdea) {
			const li = $(`.rm-idea[data-idea="${CSS.escape(el.dataset.gotoIdea)}"]`);
			if (li) {
				li.scrollIntoView({ block: 'center', behavior: matchMedia('(prefers-reduced-motion: reduce)').matches ? 'auto' : 'smooth' });
				li.classList.remove('flash');
				void li.offsetWidth;
				li.classList.add('flash');
				$('[data-vote]', li)?.focus({ preventScroll: true });
			}
		}
	});

	// ---------- votes ----------

	async function vote(kind, id) {
		if (!canVote()) return;
		const item = kind === 'task' ? taskById(id) : ideaById(id);
		const key = `${kind}:${id}`;
		if (!item || pending.has(key)) return;
		const before = { votes: item.votes, voted: item.voted };
		const on = !item.voted;
		item.voted = on;
		item.votes = Math.max(0, (item.votes ?? 0) + (on ? 1 : -1));
		pending.add(key);
		render();
		try {
			const path = `${API}/${kind === 'task' ? 'tasks' : 'ideas'}/${encodeURIComponent(id)}/vote?key=${encodeURIComponent(KEY)}`;
			// A text body, which Acta reads as JSON, keeps this a simple request: no preflight first.
			const r = await acta(path, { method: 'POST', body: JSON.stringify({ visitor: visitor(), on }) });
			item.votes = r.votes;
			item.voted = r.voted;
		} catch (e) {
			Object.assign(item, before);
			toast(e instanceof ActaError ? e.message : "The vote didn't go through. Try again.");
		} finally {
			pending.delete(key);
			render();
		}
	}

	async function copyLink(id) {
		const url = `${location.origin}${location.pathname}#${id}`;
		try {
			await navigator.clipboard.writeText(url);
			toast('Link copied.');
		} catch {
			toast(url);
		}
	}

	// ---------- ideas ----------

	const ideaList = $('.rm-idea-list');
	function renderIdeas(c) {
		const ideas = [...(data?.ideas ?? [])].sort((a, b) =>
			view.ideas === 'new' ? Date.parse(b.at) - Date.parse(a.at) : (b.votes ?? 0) - (a.votes ?? 0) || Date.parse(b.at) - Date.parse(a.at)
		);
		fill(ideaList, ideas.map((i) => ideaItem(i, c)).join(''));
		const priv = live && data?.page.ideas.public === false;
		$('.rm-ideas .rm-none').hidden = ideas.length > 0 || priv;
		$('.rm-private').hidden = !priv;
		$$('[data-total="ideas"]').forEach((el) => (el.textContent = data?.ideas.length ?? 0));
		setCount('ideas', data?.ideas.length ?? 0);
	}

	// ---------- the forms ----------

	const ideaForm = $('form[data-form="idea"]');
	const issueForm = $('form[data-form="issue"]');
	// Until the first read says otherwise, a key is taken to work, so the form doesn't blink in.
	function renderForms() {
		const open = tried ? takesIdeas() : !!KEY && (data?.page.ideas.open ?? true);
		ideaForm.hidden = !open;
		$('[data-offline="idea"]').hidden = !!open;
	}

	const switches = $$('.rm-switch [role="tab"]');
	function showForm(which, focus = false) {
		for (const tab of switches) {
			const on = tab.dataset.tab === which;
			tab.setAttribute('aria-selected', String(on));
			tab.tabIndex = on ? 0 : -1;
			$(`#${tab.getAttribute('aria-controls')}`).hidden = !on;
			if (on && focus) tab.focus();
		}
	}
	switches.forEach((tab, i) => {
		tab.addEventListener('click', () => showForm(tab.dataset.tab));
		tab.addEventListener('keydown', (e) => {
			const step = e.key === 'ArrowRight' ? 1 : e.key === 'ArrowLeft' ? -1 : 0;
			if (!step) return;
			e.preventDefault();
			showForm(switches[(i + step + switches.length) % switches.length].dataset.tab, true);
		});
	});
	$$('[data-go]').forEach((a) => a.addEventListener('click', () => showForm(a.dataset.go)));
	showForm('idea');

	// "Already on the board?" under a title, as it's typed.
	function suggest(form) {
		const input = form.elements.title;
		const box = $('.rm-similar', form);
		let timer;
		input.addEventListener('input', () => {
			clearTimeout(timer);
			timer = setTimeout(() => {
				const found = data && input.value.trim().length > 3 ? similar(input.value, data.tasks, data.ideas) : [];
				box.hidden = !found.length;
				box.innerHTML = found.length
					? `<p>Already on the board?</p><ul>${found
							.map((f) =>
								f.kind === 'task'
									? `<li><a href="#${esc(f.id)}"><span class="rm-id">${esc(f.id)}</span> ${esc(f.title)}</a> <span class="rm-pill" data-stage="${esc(f.stage)}">${esc(data.page.stages[f.stage] ?? f.stage)}</span></li>`
									: `<li><button type="button" class="rm-link" data-goto-idea="${esc(f.id)}">${esc(f.title)}</button> <span class="rm-pill">Idea</span></li>`
							)
							.join('')}</ul>`
					: '';
			}, 180);
		});
	}
	suggest(ideaForm);
	suggest(issueForm);

	function message(form, text, kind = 'ok') {
		const el = $('.rm-form-msg', form);
		el.hidden = !text;
		el.dataset.kind = kind;
		el.textContent = text;
	}

	ideaForm.addEventListener('submit', async (e) => {
		e.preventDefault();
		if (!takesIdeas()) return;
		const title = ideaForm.elements.title.value.trim();
		const details = ideaForm.elements.details.value.trim();
		if (!title) return ideaForm.elements.title.focus();
		const button = $('button[type="submit"]', ideaForm);
		button.disabled = true;
		message(ideaForm, 'Sending…');
		try {
			const r = await acta(`${API}/ideas?key=${encodeURIComponent(KEY)}`, {
				method: 'POST',
				body: JSON.stringify({ visitor: visitor(), title, details: details || null }),
			});
			mine.add(r.idea.id);
			store.set('calx-roadmap-mine', [...mine]);
			ideaForm.reset();
			$('.rm-similar', ideaForm).hidden = true;
			if (data.page.ideas.public) {
				data.ideas = [r.idea, ...data.ideas.filter((i) => i.id !== r.idea.id)];
				view.ideas = 'new';
				$$('.rm-seg button').forEach((b) => b.setAttribute('aria-pressed', String(b.dataset.sort === 'new')));
				render();
				message(ideaForm, 'Thanks. Your idea is on the list, with your vote.');
				$(`.rm-idea[data-idea="${CSS.escape(r.idea.id)}"]`)?.classList.add('flash');
			} else message(ideaForm, "Thanks. It's sent, and it's read before it's listed here.");
		} catch (err) {
			message(ideaForm, err instanceof ActaError ? err.message : "It didn't go through. Try again.", 'error');
		} finally {
			button.disabled = false;
		}
	});

	// The issue form shows the fields of the kind that's picked, and only those are sent.
	const kinds = $$('.rm-kind', issueForm);
	function pickKind() {
		const kind = issueForm.elements.template.value;
		for (const f of kinds) f.disabled = f.hidden = f.dataset.kind !== kind;
	}
	issueForm.addEventListener('change', (e) => {
		if (e.target.name === 'template') pickKind();
	});
	pickKind();

	issueForm.addEventListener('submit', (e) => {
		e.preventDefault();
		const form = new FormData(issueForm);
		const fields = [...form].filter(([, v]) => String(v).trim());
		const q = new URLSearchParams(fields);
		let url = `${issueForm.action}?${q}`;
		// A long paste doesn't fit in an address: the longest field goes to the clipboard instead.
		let moved = null;
		if (url.length > MAX_URL) {
			const [name, value] = fields.filter(([n]) => n !== 'template' && n !== 'title').sort((a, b) => String(b[1]).length - String(a[1]).length)[0] ?? [];
			if (name) {
				q.delete(name);
				url = `${issueForm.action}?${q}`;
				moved = $(`[name="${name}"]`, issueForm).closest('label').firstElementChild.firstChild.textContent.trim();
				navigator.clipboard?.writeText(String(value)).catch(() => {});
			}
		}
		const tab = window.open(url, '_blank', 'noopener');
		if (!tab) location.href = url;
		message(
			issueForm,
			moved
				? `“${moved}” was too long to send in the address, so it's on your clipboard. Paste it into that field on GitHub.`
				: 'GitHub opened in a new tab, with what you wrote. Check it there, then submit it.'
		);
	});

	// /roadmap/?report=docs&page=/spec/#add, from a docs page's "Report a problem".
	const report = params.get('report');
	if (report) {
		const kind = { bug: 'bug.yml', docs: 'docs.yml', feature: 'feature.yml' }[report];
		if (kind) {
			issueForm.elements.template.value = kind;
			pickKind();
		}
		const page = params.get('page');
		if (page && /^\/[\w./#-]*$/.test(page)) issueForm.elements.page.value = new URL(page, 'https://calxlang.com').href;
		showForm('issue');
		params.delete('report');
		params.delete('page');
		const s = params.toString();
		history.replaceState(history.state, '', `${location.pathname}${s ? `?${s}` : ''}${location.hash}`);
		requestAnimationFrame(() => issueForm.elements.title.focus({ preventScroll: true }));
	}

	// ---------- toast ----------

	const toastEl = $('.rm-toast');
	let toastTimer;
	function toast(text) {
		toastEl.textContent = text;
		toastEl.hidden = false;
		clearTimeout(toastTimer);
		toastTimer = setTimeout(() => (toastEl.hidden = true), 4000);
	}

	// ---------- go ----------

	render();
	read().then(() => {
		if (hashId() && !dialog.open) sync();
	});
	if (hashId() && data) sync();
}
