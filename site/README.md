# The Calx website

This folder holds the Calx website, [calxlang.com](https://calxlang.com): its home page, its layout and styles, and the build that turns [`docs/`](../docs/README.md) into web pages.

The docs stay in `docs/`, as plain Markdown that reads fine on GitHub. The site renders them unchanged, using each page's `title` and `description` front matter and the page order in `docs/README.md` (D40). So a change to the language and its docs lands in one commit, and the site always shows what the compiler at that commit does.

A push that only changes this folder skips the compiler's CI. A change to `docs/` still runs it, because every example in the docs is a test (D41).

The site was built on [CLX-60](https://acta.aaenz.no/aaen-studios/clx#CLX-60), made findable by search engines on [CLX-61](https://acta.aaenz.no/aaen-studios/clx#CLX-61), moved to calxlang.com on [CLX-62](https://acta.aaenz.no/aaen-studios/clx#CLX-62), reworked after a critical read on [CLX-63](https://acta.aaenz.no/aaen-studios/clx#CLX-63), polished on [CLX-67](https://acta.aaenz.no/aaen-studios/clx#CLX-67), given its roadmap on [CLX-68](https://acta.aaenz.no/aaen-studios/clx#CLX-68), and its own dropdowns on [CLX-78](https://acta.aaenz.no/aaen-studios/clx#CLX-78).

## Running it

It needs Node 22.23 or later, as Astro does. From this folder:

```sh
npm install
npm run dev      # http://localhost:4321/, reloading as you edit the site or docs/
npm run build    # the whole site, into dist/
npm run preview  # serves dist/
```

Both start from an empty content cache (`--force`). Astro keeps the docs pages it rendered and renders a page again only when the page or the Astro config changes, so without it a change to how code blocks render, in `ec.config.mjs` or `src/ec-calx.mjs`, wouldn't show. Rendering every page again takes about a second.

Search works only in a build: Pagefind indexes the pages as `npm run build` writes them, so the dev server's search box finds nothing.

## Publishing it

Vercel builds and deploys the site from this repo. Its project's root directory is `site/`, and it runs `npm run build` and serves `dist/`. A push to main updates the live site at https://calxlang.com. The project's `vercel.app` address serves the same pages, and their canonical links send search engines to calxlang.com. A push to another branch, or a pull request, gets a preview of its own, and its build shows as a check on GitHub, so a change that breaks the site shows there before it merges.

[`vercel.json`](vercel.json) sends an address without its closing slash, `/reference/parallel`, to the one with it, and one ending in `index.html` to the page's own, so each page has one address.

The project has one environment variable, `PUBLIC_ACTA_ROADMAP_KEY`, the roadmap's key ([The roadmap](#the-roadmap)). Without it the site still builds, and the roadmap is read only.

## Search engines

Every page's links for search engines and link previews use the site's address, https://calxlang.com, set once as `SITE` in [`astro.config.mjs`](astro.config.mjs). Every build uses it, including previews and local builds, so no page names another address.

- **Each page** has a title and description from its front matter, a canonical link to its own address, and the `og:` tags that link previews read, with `social.png` as the picture.
- **Titles say which Calx.** "Calx" is also a word in chemistry and anatomy, so a page's `<title>` ends with the part of the site it's in: "Lists | Calx language reference", "std.zlib | Calx standard library", and "Install Calx | The Calx programming language" for the rest. [`src/seo.mjs`](src/seo.mjs) has the rule, and [`src/route-data.mjs`](src/route-data.mjs) applies it to the docs pages, as Starlight's route middleware. The page itself shows the front matter's title as it is.
- **Descriptions are 160 characters at most.** Search results cut a longer one short, so a description says what the page is first, in under 160 characters, and over 70, so a result has something to show.
- **One address for each page.** [`vercel.json`](vercel.json) sends `/reference/parallel` to `/reference/parallel/`, and `/index.html` to `/` (`cleanUrls`), so links to a page all count for the same address. The project's `vercel.app` addresses send `X-Robots-Tag: noindex`, so only calxlang.com is in search results.
- **The site** has `robots.txt` ([`src/pages/robots.txt.ts`](src/pages/robots.txt.ts)), which lets every page be crawled and points at the sitemap, and the sitemap, with every page but the 404 one. Each page made from a file in `docs/` has a `lastmod` there, when the file last changed, which search engines use to know what to crawl again. The section pages and `/docs/` take theirs from `docs/README.md`; the home page and the roadmap, made from more than one file, have none.
- **Dates from git.** [`src/facts.mjs`](src/facts.mjs) reads when each file in `docs/` was first committed and last changed, in one `git log`. Vercel's clone has only recent history, so a date from before it begins isn't known, and it's left out rather than guessed. A docs page shows its last change at the bottom, the same date its structured data and the sitemap give.
- **Section pages.** `/guide/`, `/reference/` and `/std/` list their section's pages, as `/docs/` does all of them ([`src/pages/[hub].astro`](src/pages/[hub].astro), `HUBS` in [`src/seo.mjs`](src/seo.mjs)). So the address above each page is a page too, the crumbs link to it, and a search for "Calx standard library" has a page to find. The header's Reference and Std go there, and a docs link to `README.md#reference` does too.
- **The 404 page** asks not to be indexed and has no canonical link.
- **Structured data** (schema.org, as JSON-LD, from [`src/seo.mjs`](src/seo.mjs)):
  - the home page names the site Calx (`WebSite`, which Google reads for a result's site name), the language (`ComputerLanguage`, the same as the GitHub repo), and Aaen Studios, by the `@id` aaenz.no gives it, so both sites name one organization;
  - each docs page is a `TechArticle` with its dates and `article:` tags, and gives its crumbs as a `BreadcrumbList`, so a search result can show "Reference › Parallel loops" in place of the address.

Previews aren't indexed: Vercel serves them with `X-Robots-Tag: noindex`.

The pages' furniture for search results and browsers is in every page's head (`HEAD` in [`src/seo.mjs`](src/seo.mjs)): the SVG favicon, with `favicon.ico` beside it for what asks for that file whatever the page says, `apple-touch-icon.png` for a phone's home screen, and `theme-color`, each theme's paper.

## The docs for language models

The docs are also plain Markdown, for language models and the tools that read docs to them ([`src/llms.mjs`](src/llms.mjs)):

- [`/llms.txt`](https://calxlang.com/llms.txt), as [llmstxt.org](https://llmstxt.org) describes it: what Calx is, and every page by section, with the page list's line on what it covers;
- [`/llms-full.txt`](https://calxlang.com/llms-full.txt), every page in one file, in reading order;
- each page's own Markdown beside it, `/reference/parallel.md` for `/reference/parallel/`, which its head links to as `rel="alternate"`.

Each is the page as the site shows it: the front matter's title and description at the top, no decisions cited, and every link a full address. `vercel.json` serves them with `X-Robots-Tag: noindex`, so search results show the pages rather than their Markdown.

## How it's made

It's [Astro](https://astro.build) with [Starlight](https://starlight.astro.build), the docs theme.

- **Docs pages.** Starlight renders every page of `docs/` (`src/content.config.ts` points it there). [`src/markdown.mjs`](src/markdown.mjs) changes four things as it does:
  - links to `.md` files become the pages' URLs;
  - links to the rest of the repo go to GitHub;
  - the `# Title` heading goes, since the page shows the front matter's title;
  - a decision the text cites, "(D21)", is left out, since the site's readers don't have the [decision log](https://github.com/ellipog/calcite/blob/main/docs/decisions.md) beside them. The docs cite decisions only in parentheses, so each sentence reads the same without one (`docs/README.md` has the rule).
- **Page order.** [`src/docs-index.mjs`](src/docs-index.mjs) reads the page list in `docs/README.md`, for the sidebar and for the `/docs/` overview. Adding a page to that list adds it to the site.
- **Code.** Expressive Code highlights Calx with the VS Code extension's grammar, in the site's colors ([`ec.config.mjs`](ec.config.mjs)). On the light paper, a color dark enough to read is close to the ink, so the light theme's colors are as saturated as they can be at WCAG's 4.5:1, and Expressive Code is told to keep them rather than darken them to its own 5.5:1. [`src/ec-calx.mjs`](src/ec-calx.mjs) gives each docs block a bar of its own:
  - a block `tests/run.py` tests has Calx and Output tabs, its `// out:` comments come out of the code and become the Output tab, and a "Tested" badge says CI runs it;
  - a `// error:` line is marked, with the error under it, and the badge says the block doesn't compile, on purpose;
  - the copy button copies the tab that's showing, as it shows it.
- **Type.** Headings are in Source Serif 4, code in JetBrains Mono, and everything else in the system's own sans-serif, so a page loads two font files. Code has no ligatures, so `->` and `!=` look as they're typed ([`src/styles/tokens.css`](src/styles/tokens.css)).
- **Scrollbars.** Thin, in the theme's colors, without arrow buttons.
- **Selects.** A `<select>` is shown as the site's own dropdown: [`src/scripts/dropdown.js`](src/scripts/dropdown.js) puts a button and a list in its place, styled by [`src/styles/dropdown.css`](src/styles/dropdown.css), and keeps the select, hidden, as what holds the value, so the code around it reads `select.value` and hears `change` as before. It's read out and used from the keyboard as a select is (WAI-ARIA's select-only combobox): arrows, Home and End, typing to jump, Enter or Space to pick, Escape to close. A page calls `dropdown(select)` on the selects it has, and without JavaScript the native select stays.
- **Page furniture.** [`src/components/`](src/components/) replaces Starlight's header, theme switch, page title and footer. On narrow screens the header puts the sections and GitHub in a menu. The title shows how many of the page's examples CI tests, links to the file to edit, and links to the roadmap's issue form with the page filled in, and on the benchmark page, where the header's Performance link goes, it's followed by the home page's results chart, so that page opens with the numbers and then says how they're made. The footer has the previous and next pages, the commit the page was built from, and when its file last changed ([`src/facts.mjs`](src/facts.mjs)). The home page's and the roadmap's footer ends with a line crediting Aaen Studios, which makes Calx and Calcite, and linking to [aaenz.no](https://aaenz.no). The studio's mark is the `aaen` symbol in [`Sprite.astro`](src/components/Sprite.astro), traced from the site's `logo-mark.png` and drawn in the footer's ink. Calcite's site has the same credit.
- **Pages of its own.** [`src/pages/`](src/pages/) has the home page, the `/docs/` overview, the section pages, the roadmap and the 404 page. `docs/roadmap.md` is in the docs' page list but isn't a Starlight page: the roadmap page renders its summary under the board, and the sidebar links to it.

The home page doesn't repeat anything the repo already says. It reads it at build time:

- its example and the C it compiles to are the first ones in [`docs/reference/parallel.md`](../docs/reference/parallel.md), and the example that doesn't compile is the page's first one marked `// error:`, shown with its error as the other promises' examples are ([`src/examples.mjs`](src/examples.mjs));
- each of the other promises shows an example from the page it links to, found by a piece of its code, with what it prints or the error it's written to get ([`src/components/Example.astro`](src/components/Example.astro)), and the build times are the README's whole table;
- what `stash` reads and answers is the first two text blocks in [`docs/guide/getting-started.md`](../docs/guide/getting-started.md), and the guide's steps are its page list;
- the benchmark chart and every number on the page come from the README's [Performance](../README.md#performance) section ([`src/numbers.mjs`](src/numbers.mjs)).

So when a benchmark run updates the README, the next build shows the new numbers, and if the README's tables change shape the build fails rather than show something wrong.

## The roadmap

[`/roadmap/`](src/pages/roadmap.astro) is the Calx board on [Acta](https://acta.aaenz.no), as its public roadmap shows it: the tasks marked public, in three stages, Planned, In progress and Shipped. Under the board are the ideas people have sent, a form to send one or to report an issue, and `docs/roadmap.md`'s summary, by feature. Which tasks are public, the stages' names, and whether the board takes votes and ideas are set in Acta, on the board's public roadmap page.

Where it reads the board ([`src/roadmap/acta.mjs`](src/roadmap/acta.mjs)):

- **The build** reads Acta's keyless feed, `/api/public/aaen-studios/clx`, and renders the board as it is, so the page works without JavaScript and search engines read it. If Acta can't be reached, the build still finishes, and the page loads the board in the browser.
- **The browser** ([`src/scripts/roadmap.js`](src/scripts/roadmap.js)) reads it again when the page opens, and every minute while it's in view. With a roadmap key it reads Acta's roadmap API, `/api/roadmap/v1`, as this visitor, so it knows what they voted for, and they can vote and send ideas. Without one, or if the key is refused, it reads the feed, and the board is read only.

The key is `PUBLIC_ACTA_ROADMAP_KEY`, set in Vercel's project settings, or in `site/.env` to try it locally. It's made in Acta, under the board's public roadmap → Customize → API, and it's publishable: it reaches only what the public page shows, so it's in the page for anyone to read. Acta lets it work only from the sites listed with it, `https://calxlang.com` and, for `npm run dev`, `http://localhost:4321`. Votes and ideas are counted per visitor, a random id the browser keeps in localStorage (`calx-roadmap-visitor`); Acta keeps only a hash of it with the board's id. The ideas a visitor sent are kept there too (`calx-roadmap-mine`), to mark them.

What it does:

- **The board.** Each stage is a column, ten cards long until "Show all", and one at a time on a phone, picked with tabs. Search matches a task's id, title and description; sort, one of the site's dropdowns, is the board's order, most votes or newest; both stay in the address (`?q=`, `?sort=`). Press `/` to search.
- **A task.** A card opens the task in a dialog with its description, rendered from Markdown by [`src/roadmap/render.mjs`](src/roadmap/render.mjs), which the build and the browser share, so a card looks the same either way. Everything from Acta is escaped, and only links to web pages and to this site are kept: a link to the docs on GitHub goes to the docs here, and a link to another task on the board opens it here if it's public. Each task has an address, `/roadmap/#CLX-12`, and Back closes it.
- **Votes**, on any task not shipped and on ideas, show at once and are taken back if Acta turns them down.
- **Ideas** go to Acta, and so does every vote. As someone types a title, the page lists tasks and ideas that look the same, so a vote can go to one that's already there.
- **Issues** go to GitHub. The form is a GitHub issue form, [`.github/ISSUE_TEMPLATE/`](../.github/ISSUE_TEMPLATE): the page opens `issues/new` with the template and its fields filled in from what was typed, and the reader checks it and submits it there. A field's `name` here is its `id` in the template, so the two must stay the same. An address that would be longer than GitHub takes leaves out its longest field and puts it on the clipboard. Each docs page's "Report a problem" opens this form as a docs issue with the page filled in (`?report=docs&page=/spec/`).
- **Powered by Acta**, with Acta's mark, under the board.

## The crystal

The site's art is the crystal from the README's banner, dithered: ink dots by Atkinson's error diffusion, and amber veins by ordered dithering. [`assets/logo/banner.py`](../assets/logo/banner.py) makes all of it:

- `calx-banner.png` and `calx-banner-dark.png`, the README's banner
- `calx-social.png`, the card link previews show, 1200 by 630: the site's `og:image`
- `calx-tones.jpg`, the photo's darkness and warmth, which the site dithers live in the browser with the script's own steps
- `calx-icon-180.png` and `calx-icon.ico`, the mark in ink on paper, the site's `apple-touch-icon.png` and `favicon.ico`

Dithering live means the dots are whole screen pixels at any size and screen density, so they stay sharp, and the crystal can grow in when it comes into view.

How the site uses it:

- **Accents only.** The crystal is in the home page's hero, the "Built with Calx" band and the 404 page. It never sits behind docs text, code or charts.
- **The banner as the first screen.** The hero is the README's banner at the size of the screen: the logo in a clearing of the crystal, its own shape knocked out of the dots so they run up to its edges, and the page's one line and its two buttons under it. Each section of the home page has a line or two and an example, and the docs say the rest.
- **Quiet around text.** The dither fades out around text, and around the logo in banner.py's ellipse.
- **Narrow screens.** On a canvas taller than it is wide, such as the hero on a phone, the photo is turned a quarter so the crystal runs down the screen rather than being cut to a sliver. The photo's largest faces are its darkest, which the banner hides behind the logo but a narrow screen shows, so the darkest tones are eased off and dither rather than go solid. On a phone the band's text is as wide as the screen, so its crystal is above and below the text.
- **Whole pixels.** Dots are drawn at whole screen pixels, never as a scaled image.
- **Two colors for anything else.** Other illustrations are dithers in ink and amber, on the same paper.
- **Still when asked.** With reduced motion on, the crystal appears without growing in. Switching the theme only repaints it.
