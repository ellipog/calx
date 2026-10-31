// The roadmap's data, from the Calx board on Acta. The build and the browser both read it from here.
//
// - The keyless feed holds everything the board's public page shows, and anyone may read it from
//   anywhere. The build reads it, and so does the browser when there's no roadmap key.
// - The roadmap API takes a publishable key (PUBLIC_ACTA_ROADMAP_KEY). With it, the browser also
//   learns what this visitor voted for, and can vote and send ideas.
//
// Both give the same shapes (Acta's src/lib/roadmap-json.ts), which this turns into one.

export const ACTA = 'https://acta.aaenz.no';
export const WORKSPACE = 'aaen-studios';
export const BOARD = 'clx';
export const FEED = `${ACTA}/api/public/${WORKSPACE}/${BOARD}`;
export const API = `${ACTA}/api/roadmap/v1`;
/** Acta's own public page for the board, for anyone without JavaScript or when the key fails. */
export const ACTA_PAGE = `${ACTA}/p/${WORKSPACE}/${BOARD}`;
export const GITHUB_ISSUES = 'https://github.com/ellipog/calx/issues';

export const STAGES = ['planned', 'progress', 'shipped'];

/**
 * @typedef {{ id: string, number: number, title: string, description: string, status: string,
 *   stage: 'planned' | 'progress' | 'shipped', labels: string[], shipped: string | null,
 *   subtasks: { done: number, total: number } | null, votes: number | null, voted?: boolean | null }} Task
 * @typedef {{ id: string, title: string, details: string | null, by: string, at: string,
 *   votes: number | null, voted?: boolean | null }} Idea
 * @typedef {{ stages: Record<string, string>, voting: boolean, ideas: { open: boolean, public: boolean },
 *   announcement: string | null }} Page
 * @typedef {{ page: Page, tasks: Task[], ideas: Idea[], live: boolean, at: string }} Roadmap
 */

const task = (t) => ({
	id: t.id,
	number: t.number,
	title: t.title,
	description: t.description ?? '',
	status: t.status,
	stage: t.stage,
	labels: t.labels ?? [],
	shipped: t.shipped ?? null,
	subtasks: t.subtasks ?? null,
	votes: t.votes ?? null,
	...(t.voted !== undefined ? { voted: t.voted } : {}),
});

const idea = (i) => ({
	id: i.id,
	title: i.title,
	details: i.details ?? null,
	by: i.by,
	at: i.at,
	votes: i.votes ?? null,
	...(i.voted !== undefined ? { voted: i.voted } : {}),
});

/** The keyless feed. It doesn't say whether the page takes votes and ideas, so votes that are there
 *  say voting is on, and ideas are taken unless the API says otherwise. */
export function fromFeed(feed) {
	const tasks = feed.tasks.map(task);
	return {
		page: {
			stages: feed.page.stages,
			voting: tasks.some((t) => t.votes !== null),
			ideas: { open: true, public: true },
			announcement: feed.page.announcement ?? null,
		},
		tasks,
		ideas: feed.ideas.map(idea),
		live: false,
		at: new Date().toISOString(),
	};
}

/** The roadmap API's GET /, GET /tasks and GET /ideas, read with a visitor. */
export function fromApi(info, tasks, ideas) {
	return {
		page: {
			stages: info.page.stages,
			voting: info.page.voting,
			ideas: { open: info.page.ideas.open, public: info.page.ideas.public },
			announcement: info.page.announcement ?? null,
		},
		tasks: tasks.map(task),
		ideas: ideas.ideas.map(idea),
		live: true,
		at: new Date().toISOString(),
	};
}

export class ActaError extends Error {
	constructor(status, message) {
		super(message);
		this.status = status;
	}
}

/** JSON from Acta, or an ActaError with its own words for what went wrong. */
export async function acta(url, init = {}) {
	const res = await fetch(url, { ...init, signal: init.signal ?? AbortSignal.timeout(12000) });
	const body = await res.json().catch(() => null);
	if (!res.ok) throw new ActaError(res.status, body?.error ?? `Acta answered ${res.status}.`);
	return body;
}

/** Everything, through the API with a key: the page, every task and the ideas, with the visitor's votes. */
export async function readApi(key, visitor) {
	const q = new URLSearchParams({ key, ...(visitor ? { visitor } : {}) });
	const [info, ideas] = await Promise.all([acta(`${API}?${q}`), acta(`${API}/ideas?sort=top&${q}`)]);
	const tasks = [];
	for (let offset = 0; ; offset += 200) {
		const page = await acta(`${API}/tasks?limit=200&offset=${offset}&${q}`);
		tasks.push(...page.tasks);
		if (tasks.length >= page.total || !page.tasks.length) break;
	}
	return fromApi(info, tasks, ideas);
}

export async function readFeed() {
	return fromFeed(await acta(FEED));
}
