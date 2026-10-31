// The docs' code blocks as the mockup has them: a bar over each one with what it is, a badge for
// the examples CI tests, and a copy button. A tested Calx example gets a tab for what it prints,
// from its "// out:" comments, and one that must fail to compile shows its error under the line
// instead of as a comment at its end (tests/run.py, D41). The docs files stay as they are.
import { relative } from 'node:path';
import { fileURLToPath } from 'node:url';
import { definePlugin } from '@astrojs/starlight/expressive-code';
import { DOCS } from './paths.mjs';

const docsDir = fileURLToPath(DOCS);
const LABELS = { calx: 'Calx', c: 'C', sh: 'Shell', text: 'Text', json: 'JSON' };

// run.py tests every block whose fence is exactly ```calx in these folders, and nothing else.
const TESTED = ['guide/', 'reference/', 'std/'];

/** Where in docs/ the block is, as "reference/parallel.md", or null for a block outside docs/. */
function docsPath(codeBlock) {
	const file = codeBlock.parentDocument?.sourceFilePath;
	if (!file) return null;
	const rel = relative(docsDir, file).replace(/\\/g, '/');
	return rel.startsWith('..') || rel.includes(':') ? null : rel;
}

// What the plugin learns about each block before it's rendered.
const facts = new WeakMap();

const el = (tagName, properties = {}, children = []) => ({ type: 'element', tagName, properties, children });
const text = (value) => ({ type: 'text', value });
const icon = (id) => el('svg', { className: ['i'], ariaHidden: 'true' }, [el('use', { href: `#${id}` })]);
/** A comment's text, with its `code` spans. */
const prose = (s) => s.split(/`([^`]+)`/).map((part, i) => (i % 2 ? el('code', {}, [text(part)]) : text(part)));
const classes = (node) => (node.properties.className ??= []);

export function calxBlocks() {
	return definePlugin({
		name: 'calx-blocks',
		hooks: {
			preprocessCode({ codeBlock }) {
				const path = docsPath(codeBlock);
				if (path === null) return;
				// One look for every block: no terminal window for the shell ones.
				codeBlock.props.frame = 'code';
				const tested = codeBlock.language === 'calx' && codeBlock.meta.trim() === '' && TESTED.some((d) => path.startsWith(d));
				const errors = new Map();
				const output = [];
				if (tested) {
					codeBlock.getLines().forEach((line, i) => {
						const error = line.text.match(/^(.*?)\s*\/\/ error: ?(.*)$/);
						if (error) {
							errors.set(i, error[2]);
							line.editText(error[1].length, undefined, '');
						}
						const out = line.text.match(/\/\/ out: ?(.*)$/);
						if (out) output.push(out[1]);
					});
				}
				facts.set(codeBlock, { tested, errors, output });
			},
			postprocessRenderedLine({ codeBlock, lineIndex, renderData }) {
				if (facts.get(codeBlock)?.errors.has(lineIndex)) classes(renderData.lineAst).push('calx-err');
			},
			postprocessRenderedBlock({ codeBlock, renderData }) {
				const f = facts.get(codeBlock);
				if (!f) return;
				const figure = renderData.blockAst.children.find((n) => n.tagName === 'figure') ?? renderData.blockAst;
				if (figure.tagName !== 'figure') return;
				const pre = figure.children.find((n) => n.tagName === 'pre');
				const code = pre?.children.find((n) => n.tagName === 'code');
				if (!pre || !code) return;
				classes(figure).push('calx-frame');

				// The error under the line it's on.
				let line = -1;
				code.children = code.children.flatMap((node) => {
					if (node.type !== 'element' || !node.properties.className?.includes('ec-line')) return [node];
					const message = f.errors.get(++line);
					if (message === undefined) return [node];
					return [node, el('div', { className: ['calx-callout'] }, [el('b', {}, [text('error:')]), text(' '), ...prose(message)])];
				});

				// The panes: the code, and what it prints.
				pre.properties.dataCode = codeBlock.code;
				const panes = [{ name: LABELS[codeBlock.language] ?? codeBlock.language.toUpperCase(), pre }];
				if (f.output.length) {
					const out = el('pre', { className: ['calx-out'], hidden: true, dataCode: f.output.join('\n') }, [text(f.output.join('\n'))]);
					figure.children.splice(figure.children.indexOf(pre) + 1, 0, out);
					panes.push({ name: 'Output', pre: out });
				}

				const label =
					codeBlock.language === 'calx'
						? el(
								'div',
								{ className: ['calx-tabs'], role: 'tablist', ariaLabel: 'Show' },
								panes.map((p, i) => el('button', { className: ['calx-tab'], type: 'button', role: 'tab', ariaSelected: String(i === 0) }, [text(p.name)])),
							)
						: el('span', { className: ['calx-lang'] }, [text(panes[0].name)]);
				const badge = !f.tested
					? []
					: f.errors.size
						? [el('span', { className: ['calx-badge', 'fails'], title: 'CI checks that this example fails to compile, with this error' }, [icon('i-x'), text("Doesn't compile, on purpose")])]
						: [el('span', { className: ['calx-badge'], title: 'CI compiles and runs this example on every push' }, [icon('i-check'), text('Tested')])];
				const copy = el('button', { className: ['calx-copy'], type: 'button', ariaLabel: 'Copy' }, [icon('i-copy'), icon('i-check')]);

				// The bar takes the place of Expressive Code's header, and its copy button of Expressive Code's.
				figure.children = figure.children.filter((n) => n.tagName !== 'figcaption' && !n.properties?.className?.includes('copy'));
				figure.children.unshift(el('div', { className: ['calx-head'] }, [label, el('div', { className: ['calx-right'] }, [...badge, copy])]));
			},
		},
		jsModules: [
			`document.addEventListener('click', (event) => {
				const target = event.target instanceof Element ? event.target : null;
				const frame = target?.closest('.calx-frame');
				if (!frame) return;
				const panes = [...frame.querySelectorAll(':scope > pre')];
				const tab = target.closest('.calx-tab');
				if (tab) {
					const tabs = [...frame.querySelectorAll('.calx-tab')];
					const i = tabs.indexOf(tab);
					tabs.forEach((t, j) => t.setAttribute('aria-selected', String(i === j)));
					panes.forEach((p, j) => (p.hidden = i !== j));
					return;
				}
				const copy = target.closest('.calx-copy');
				if (!copy) return;
				const shown = panes.find((p) => !p.hidden);
				navigator.clipboard?.writeText(shown?.dataset.code ?? '').then(() => {
					copy.classList.add('done');
					copy.setAttribute('aria-label', 'Copied');
					setTimeout(() => {
						copy.classList.remove('done');
						copy.setAttribute('aria-label', 'Copy');
					}, 1500);
				}, () => {});
			});`,
		],
	});
}
