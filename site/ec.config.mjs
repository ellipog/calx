// Code blocks: Expressive Code, which Starlight uses, highlighting Calx with the VS Code
// extension's own TextMate grammar, in two themes made from the site's colors.
import { readFileSync } from 'node:fs';
import { defineEcConfig } from '@astrojs/starlight/expressive-code';
import { calxBlocks } from './src/ec-calx.mjs';
import { REPO } from './src/paths.mjs';

const grammar = JSON.parse(readFileSync(new URL('tools/vscode/syntaxes/calx.tmLanguage.json', REPO), 'utf8'));

// The docs mark what an example prints with "// out:" and the line that must not compile with
// "// error:" (D41). The site gives those comments scopes of their own, ahead of the plain comment,
// so they can be colored apart. The extension's grammar is left as it is.
grammar.repository.comments.patterns.unshift(
	{ name: 'comment.line.output.calx', match: '//\\s*out:.*$' },
	{ name: 'comment.line.error.calx', match: '//\\s*error:.*$' },
);

// The site's token colors, light and dark. On the light paper a color dark enough to read is close
// to the ink, so the light ones are as saturated as they can be and still have 4.5:1 contrast, or
// the code looks black and white.
const PALETTE = {
	light: {
		bg: '#F3EEE5', fg: '#1E1F24', kw: '#A85200', type: '#1D63A8', num: '#A8306E', str: '#3C7A12',
		com: '#6B6C73', out: '#8A5A14', err: '#B3261E', mac: '#7A3FC4', sel: '#E3DCCF',
	},
	dark: {
		bg: '#11171F', fg: '#ECE6DA', kw: '#E3A65C', type: '#8DB4E2', num: '#E09AC0', str: '#A8C987',
		com: '#898F98', out: '#B59A6E', err: '#F2766B', mac: '#B9A2E6', sel: '#2F3947',
	},
};

function theme(type) {
	const c = PALETTE[type];
	const rule = (scope, foreground, fontStyle) => ({ scope, settings: { foreground, ...(fontStyle && { fontStyle }) } });
	return {
		name: `calx-${type}`,
		type,
		colors: { 'editor.background': c.bg, 'editor.foreground': c.fg, 'editor.selectionBackground': c.sel },
		tokenColors: [
			{ settings: { foreground: c.fg, background: c.bg } },
			rule(['comment', 'punctuation.definition.comment'], c.com, 'italic'),
			rule(['comment.line.output'], c.out, 'italic'),
			rule(['comment.line.error'], c.err, 'italic'),
			rule(['string', 'punctuation.definition.string', 'storage.type.string'], c.str),
			rule(['meta.interpolation', 'punctuation.section.interpolation'], c.fg),
			rule(['constant.character.escape'], c.mac),
			rule(['constant.numeric', 'constant.language', 'storage.type.numeric'], c.num),
			rule(
				['keyword', 'storage.type', 'storage.modifier', 'variable.language', 'keyword.operator.wordlike', 'keyword.other.as'],
				c.kw,
			),
			rule(['keyword.operator', 'punctuation'], c.fg),
			rule(['entity.name.type', 'support.type', 'entity.name.namespace', 'storage.type.built-in', 'support.type.property-name'], c.type),
			rule(['entity.name.function', 'support.function'], c.fg, 'bold'),
			rule(['entity.name.function.decorator', 'punctuation.definition.annotation', 'meta.preprocessor', 'entity.name.function.preprocessor'], c.mac),
			rule(['invalid'], c.err),
		],
	};
}

export default defineEcConfig({
	themes: [theme('dark'), theme('light')],
	shiki: { langs: [{ ...grammar, name: 'calx' }] },
	plugins: [calxBlocks()],
	// Expressive Code darkens any color with less than 5.5:1 contrast, which takes the light colors
	// back toward the ink. 4.5:1 is what WCAG asks of text.
	minSyntaxHighlightingColorContrast: 4.5,
	// A "// file: main.calx" line is part of a test of several modules (tests/run.py). Expressive
	// Code would take it out of the code and make it the block's title.
	frames: { extractFileNameFromCode: false },
	// Code blocks scroll with the site's own scrollbars (src/styles/tokens.css).
	useThemedScrollbars: false,
	styleOverrides: {
		borderRadius: '12px',
		borderColor: 'var(--line)',
		codeFontFamily: 'var(--f-mono)',
		codeFontSize: '13.5px',
		codeLineHeight: '1.68',
		codePaddingBlock: '16px',
		codePaddingInline: '20px',
		uiFontFamily: 'var(--f-body)',
		frames: {
			shadowColor: 'transparent',
			editorTabBarBackground: 'var(--raised)',
			editorActiveTabBackground: 'var(--sunk)',
			editorActiveTabIndicatorTopColor: 'transparent',
			editorActiveTabIndicatorBottomColor: 'var(--amber)',
			editorTabBarBorderBottomColor: 'var(--line)',
			terminalTitlebarBackground: 'var(--raised)',
			terminalTitlebarBorderBottomColor: 'var(--line)',
			terminalBackground: 'var(--code)',
			frameBoxShadowCssValue: 'none',
		},
	},
});
