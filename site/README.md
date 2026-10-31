# The Calx website

This folder holds the Calx website: its home page, its layout and styles, and the build that turns [`docs/`](../docs/README.md) into web pages.

The docs stay in `docs/`, as plain Markdown that reads fine on GitHub. The site renders them unchanged, using each page's `title` and `description` front matter and the page order in `docs/README.md` (D40). So a change to the language and its docs lands in one commit, and the site always shows what the compiler at that commit does.

A push that only changes this folder skips the compiler's CI. A change to `docs/` still runs it, because every example in the docs is a test (D41).

What the site has, how it looks, and what builds it are being decided on [CLX-60](https://acta.aaenz.no/aaen-studios/clx#CLX-60).

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
