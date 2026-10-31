# The Calx website

This folder holds the Calx website: its home page, its layout and styles, and the build that turns [`docs/`](../docs/README.md) into web pages.

The docs stay in `docs/`, as plain Markdown that reads fine on GitHub. The site renders them unchanged, using each page's `title` and `description` front matter and the page order in `docs/README.md` (D40). So a change to the language and its docs lands in one commit, and the site always shows what the compiler at that commit does.

A push that only changes this folder skips the compiler's CI. A change to `docs/` still runs it, because every example in the docs is a test (D41).

The site is being built on [CLX-60](https://acta.aaenz.no/aaen-studios/clx#CLX-60).

## Running it

It needs Node 22.23 or later, as Astro does. From this folder:

```sh
npm install
npm run dev      # http://localhost:4321/, reloading as you edit the site or docs/
npm run build    # the whole site, into build/site
npm run preview  # serves build/site
```

Both start from an empty content cache (`--force`). Astro keeps the docs pages it rendered and renders a page again only when the page or the Astro config changes, so without it a change to how code blocks render, in `ec.config.mjs` or `src/ec-calx.mjs`, wouldn't show. Rendering every page again takes about a second.

Search works only in a build: Pagefind indexes the pages as `npm run build` writes them, so the dev server's search box finds nothing.

## Publishing it

[`.github/workflows/site.yml`](../.github/workflows/site.yml) builds the site on every push and pull request that touches something it's made from: this folder, `docs/`, the README, `assets/logo/` and the grammar. Deploying to GitHub Pages at calxlang.com is ready but off. It starts once Pages is set to deploy from GitHub Actions with calxlang.com as its domain, and the repository variable `SITE_DEPLOY` is `true`. Then every push to main that builds is published.

## How it's made

It's [Astro](https://astro.build) with [Starlight](https://starlight.astro.build), the docs theme.

- **Docs pages.** Starlight renders every page of `docs/` (`src/content.config.ts` points it there). [`src/markdown.mjs`](src/markdown.mjs) changes four things as it does:
  - links to `.md` files become the pages' URLs;
  - links to the rest of the repo go to GitHub;
  - the `# Title` heading goes, since the page shows the front matter's title;
  - a decision the text cites, "(D21)", becomes a small link to its row in the [decision log](https://github.com/ellipog/calcite/blob/main/docs/decisions.md).
- **Page order.** [`src/docs-index.mjs`](src/docs-index.mjs) reads the page list in `docs/README.md`, for the sidebar and for the `/docs/` overview. Adding a page to that list adds it to the site.
- **Code.** Expressive Code highlights Calx with the VS Code extension's grammar, in the site's colors ([`ec.config.mjs`](ec.config.mjs)). [`src/ec-calx.mjs`](src/ec-calx.mjs) gives each docs block the mockup's bar:
  - a block `tests/run.py` tests has Calx and Output tabs, its `// out:` lines become the Output tab, and a "Tested" badge says CI runs it;
  - a `// error:` line is marked, with the error under it, and the badge says the block doesn't compile, on purpose;
  - the copy button copies the tab that's showing, as it shows it.
- **Scrollbars and type.** Thin scrollbars in the theme's colors, without arrow buttons, and code without ligatures, so `->` and `!=` look as they're typed ([`src/styles/tokens.css`](src/styles/tokens.css)).
- **Page furniture.** [`src/components/`](src/components/) replaces Starlight's header, theme switch, page title and footer. The title shows how many of the page's examples CI tests and links to the file to edit. The footer has the previous and next pages and the commit the page was built from ([`src/facts.mjs`](src/facts.mjs)).
- **Pages of its own.** [`src/pages/`](src/pages/) has the home page, the `/docs/` overview and the 404 page.

The home page doesn't repeat anything the repo already says. It reads it at build time:

- its example, the C it compiles to and the error are the first ones in [`docs/reference/parallel.md`](../docs/reference/parallel.md) ([`src/examples.mjs`](src/examples.mjs));
- what `stash` reads and answers is the first two text blocks in [`docs/guide/getting-started.md`](../docs/guide/getting-started.md), and the guide's steps are its page list;
- the benchmark chart and every number on the page come from the README's [Performance](../README.md#performance) section ([`src/numbers.mjs`](src/numbers.mjs)).

So when a benchmark run updates the README, the next build shows the new numbers, and if the README's tables change shape the build fails rather than show something wrong.

## The mockup

[`mockup.html`](mockup.html) is the agreed look, in one file with no build: the home page, a reference page and the 404 page. Open it in a browser. The bar at the bottom switches between the pages and between light and dark. The real site is built to match it, and then it goes.

## The crystal

The site's art is the crystal from the README's banner, dithered: ink dots by Atkinson's error diffusion, and amber veins by ordered dithering. [`assets/logo/banner.py`](../assets/logo/banner.py) makes all of it:

- `calx-banner.png` and `calx-banner-dark.png`, the README's banner
- `calx-social.png`, the card link previews show, 1200 by 630: the site's `og:image`
- `calx-tones.jpg`, the photo's darkness and warmth, which the site dithers live in the browser with the script's own steps

Dithering live means the dots are whole screen pixels at any size and screen density, so they stay sharp, and the crystal can grow in when it comes into view. The mockup carries `calx-tones.jpg` as a data URL, so it works when opened from a file. After running `banner.py`, replace `TONES` in the mockup with the new file's data URL.

How the site uses it:

- **Accents only.** The crystal is in the home page's hero, the "Built with Calx" band and the 404 page. It never sits behind docs text, code or charts.
- **Quiet around text.** The dither fades out where text or the logo sits, and the logo has a margin of paper so no dot touches it.
- **Whole pixels.** Dots are drawn at whole screen pixels, never as a scaled image.
- **Two colors for anything else.** Other illustrations are dithers in ink and amber, on the same paper.
- **Still when asked.** With reduced motion on, the crystal appears without growing in. Switching the theme only repaints it.
