// The crystal: banner.py's dither of the calcite photo, drawn live on the page's canvases.
//
// /crystal-tones.jpg is assets/logo/calx-tones.jpg, which banner.py writes: the photo's darkness
// (top half) and warmth (bottom half). Each canvas dithers its own crop of it, Atkinson for the ink
// and Bayer for the amber veins, as banner.py does. Dots are a whole number of screen pixels, so
// they stay crisp, and each scene fades out around what it keeps clear, measured where that is on
// the page. It grows outward from that clearing the first time it comes into view, unless the
// reader asks for less motion.
//
// A canvas says which scene it is with data-scene: "hero", "band" or "404".

const BAYER = [0, 32, 8, 40, 2, 34, 10, 42, 48, 16, 56, 24, 50, 18, 58, 26, 12, 44, 4, 36, 14, 46, 6, 38,
	60, 28, 52, 20, 62, 30, 54, 22, 3, 35, 11, 43, 1, 33, 9, 41, 51, 19, 59, 27, 49, 17, 57, 25,
	15, 47, 7, 39, 13, 45, 5, 37, 63, 31, 55, 23, 61, 29, 53, 21].map((v) => v / 64 + 1 / 128);
const DOT = 3.5; // CSS pixels per dot, before rounding to whole screen pixels

// Crops are fractions of the photo: x0, y0, x1, y1. The hero's is the banner's own (TOP 0.30, 10:3).
// A quiet zone is a box kept clear, with a margin of pad and then soft to fade in over. One that's
// open runs to the canvas's left and top edges too, so the crystal grows only beside and below the text.
const KINDS = {
	hero: { crop: [0, 0.3, 1, 0.8375], quiet: [{ el: '.hero-text', open: true, pad: 32, soft: 260 }], fade: { bottom: 80 } },
	band: { crop: [0, 0, 1, 0.54], quiet: [{ el: '.band .text', pad: 28, soft: 190 }], fade: { top: 130, bottom: 130 } },
	404: { crop: [0, 0, 1, 1], quiet: [{ el: '.nf-copy', pad: 30, soft: 220 }], glyph: '.nf-glyph', fade: { top: 40 } },
};

const $ = (s) => document.querySelector(s);
const $$ = (s) => [...document.querySelectorAll(s)];
const root = document.documentElement;
const still = matchMedia('(prefers-reduced-motion: reduce)').matches;
let T = null; // { w, h, dark, warm }

const SCENES = $$('canvas.crystal[data-scene]').map((el) => ({
	...KINDS[el.dataset.scene],
	el,
	key: '',
	g: null,
	p: still ? 1 : 0,
}));

function sample(map, u, v) {
	// bilinear; u and v in tone pixels
	u = Math.min(Math.max(u - 0.5, 0), T.w - 1.001);
	v = Math.min(Math.max(v - 0.5, 0), T.h - 1.001);
	const x = u | 0, y = v | 0, fx = u - x, fy = v - y, i = y * T.w + x;
	return (map[i] * (1 - fx) + map[i + 1] * fx) * (1 - fy) + (map[i + T.w] * (1 - fx) + map[i + T.w + 1] * fx) * fy;
}
const clamp01 = (x) => (x < 0 ? 0 : x > 1 ? 1 : x);
const smooth = (x) => ((x = clamp01(x)), x * x * (3 - 2 * x));

// A mask at dot resolution, from drawing on a small canvas: the 404's text.
function mask(cols, rows, paint) {
	const c = document.createElement('canvas');
	c.width = cols;
	c.height = rows;
	const g = c.getContext('2d');
	paint(g);
	const a = g.getImageData(0, 0, cols, rows).data, m = new Float32Array(cols * rows);
	for (let i = 0; i < m.length; i++) m[i] = a[i * 4 + 3] / 255;
	return m;
}
// Computed styles ignore CSS zoom and boxes don't, so text measured from styles is scaled by this.
const zoomOf = (el) => el.getBoundingClientRect().height / el.offsetHeight || 1;

function layout(S) {
	const cv = S.el, R = cv.getBoundingClientRect();
	if (!R.width || !R.height) return false;
	const dpr = devicePixelRatio || 1;
	// Text drawn in dots needs about 55 dots to its em to stay legible, so small text gets smaller dots.
	const dot = S.glyph ? Math.min(DOT, (parseFloat(getComputedStyle($(S.glyph)).fontSize) * zoomOf($(S.glyph))) / 55) : DOT;
	const cd = Math.max(2, Math.round(dot * dpr)); // screen pixels per dot
	const cs = cd / dpr; // CSS pixels per dot
	cv.width = Math.round(R.width * dpr);
	cv.height = Math.round(R.height * dpr);
	const cols = Math.ceil(cv.width / cd), rows = Math.ceil(cv.height / cd), n = cols * rows;

	// The crop, scaled to cover the canvas.
	const [x0, y0, x1, y1] = S.crop;
	const s = Math.min(((x1 - x0) * T.w) / cols, ((y1 - y0) * T.h) / rows); // tone pixels per dot
	const uc = ((x0 + x1) / 2) * T.w, vc = ((y0 + y1) / 2) * T.h;
	const k = Math.min(3, Math.max(1, Math.round(s))); // samples per axis when dots are big

	const quiet = S.quiet.flatMap((q) =>
		$$(q.el).map((el) => {
			const r = el.getBoundingClientRect(), x = r.left - R.left;
			// An open box stays open only with room for the crystal to its right; without, the crystal goes above it.
			const open = q.open && R.width - x - r.width > 240;
			// Where there's little room beside the box, it fades in over less, so narrower screens still get some crystal.
			const room = (open ? R.width - x - r.width : Math.min(x, R.width - x - r.width)) - (q.pad || 0);
			return { ...q, open, x, y: r.top - R.top, w: r.width, h: r.height, soft: Math.min(q.soft || 0, Math.max(60, room * 0.55)) };
		})
	);
	const ox = quiet.length ? quiet[0].x + quiet[0].w / 2 : R.width / 2;
	const oy = quiet.length ? quiet[0].y + quiet[0].h / 2 : R.height / 2;
	const far = Math.hypot(Math.max(ox, R.width - ox), Math.max(oy, R.height - oy));

	// Text drawn as dots (the 404): its glyphs, at dot resolution.
	let glyph = null;
	if (S.glyph) {
		const el = $(S.glyph), r = el.getBoundingClientRect(), st = getComputedStyle(el), z = zoomOf(el);
		glyph = mask(cols, rows, (g) => {
			g.font = `${st.fontWeight} ${(parseFloat(st.fontSize) * z) / cs}px ${st.fontFamily}`;
			g.textAlign = 'center';
			g.textBaseline = 'middle';
			g.letterSpacing = `${(parseFloat(st.letterSpacing) * z) / cs || 0}px`;
			g.fillText(el.textContent, (r.left - R.left + r.width / 2) / cs, (r.top - R.top + r.height / 2) / cs);
		});
	}

	const tone = new Float32Array(n), warmth = new Float32Array(n), t = new Float32Array(n);
	for (let j = 0, i = 0; j < rows; j++)
		for (let x = 0; x < cols; x++, i++) {
			const px = (x + 0.5) * cs, py = (j + 0.5) * cs;
			let c = 1;
			for (const q of quiet) {
				// a soft margin around the box
				const dx = Math.max(q.open ? 0 : q.x - px, 0, px - q.x - q.w), dy = Math.max(q.open ? 0 : q.y - py, 0, py - q.y - q.h);
				c *= clamp01((Math.hypot(dx, dy) - q.pad) / q.soft) ** 1.5;
			}
			if (S.fade) {
				if (S.fade.top) c *= smooth(py / S.fade.top);
				if (S.fade.bottom) c *= smooth((R.height - py) / S.fade.bottom);
			}
			let d = 0, w = 0;
			for (let b = 0; b < k; b++)
				for (let a = 0; a < k; a++) {
					const u = uc + (x - cols / 2 + (a + 0.5) / k) * s, v = vc + (j - rows / 2 + (b + 0.5) / k) * s;
					d += sample(T.dark, u, v);
					w += sample(T.warm, u, v);
				}
			d /= k * k;
			w /= k * k;
			const hash = ((i * 2654435761) >>> 0) / 4294967296;
			if (glyph && glyph[i] > 0.5) {
				// inside the glyphs, the crystal at full strength, and a little denser
				tone[i] = 0.45 + 0.55 * d;
				warmth[i] = w;
				t[i] = 0.3 * hash;
			} else {
				tone[i] = d * c;
				warmth[i] = w * c;
				t[i] = (0.78 * Math.hypot(px - ox, py - oy)) / far + 0.22 * hash; // grows outward from the clearing
			}
		}

	// Atkinson's dither, as banner.py's atkinson(): 6/8 of each dot's error passes on.
	const W = cols + 3, buf = new Float32Array(W * (rows + 2));
	for (let j = 0; j < rows; j++) buf.set(tone.subarray(j * cols, (j + 1) * cols), j * W + 1);
	const ink = new Uint8Array(n), vein = new Uint8Array(n);
	for (let j = 0; j < rows; j++)
		for (let x = 1; x <= cols; x++) {
			const at = j * W + x, on = buf[at] >= 0.5, e = (buf[at] - on) / 8;
			ink[j * cols + x - 1] = on;
			buf[at + 1] += e;
			buf[at + 2] += e;
			buf[at + W - 1] += e;
			buf[at + W] += e;
			buf[at + W + 1] += e;
			buf[at + 2 * W] += e;
		}
	for (let j = 0, i = 0; j < rows; j++)
		for (let x = 0; x < cols; x++, i++) vein[i] = !ink[i] && warmth[i] * 0.9 > BAYER[(j & 7) * 8 + (x & 7)];

	S.g = { cols, rows, cd, ink, vein, t };
	return true;
}

function paint(S) {
	const G = S.g;
	if (!G) return;
	const ctx = S.el.getContext('2d'), st = getComputedStyle(root);
	ctx.clearRect(0, 0, S.el.width, S.el.height);
	for (const [bits, color] of [[G.vein, '--dither-vein'], [G.ink, '--dither-ink']]) {
		ctx.fillStyle = st.getPropertyValue(color).trim();
		ctx.beginPath();
		for (let j = 0, i = 0; j < G.rows; j++)
			for (let x = 0; x < G.cols; x++, i++) if (bits[i] && G.t[i] <= S.p) ctx.rect(x * G.cd, j * G.cd, G.cd, G.cd);
		ctx.fill();
	}
}

// It crystallizes once, outward from its clearing, the first time it comes into view.
function grow(S) {
	if (S.p >= 1 || S.growing) return;
	S.growing = true;
	const t0 = performance.now();
	const step = () => {
		const x = clamp01((performance.now() - t0) / 1300);
		S.p = 1 - (1 - x) ** 3;
		paint(S);
		if (x < 1) requestAnimationFrame(step);
		else S.growing = false;
	};
	requestAnimationFrame(step);
}
const seen = new IntersectionObserver(
	(es) =>
		es.forEach((e) => {
			const S = SCENES.find((s) => s.el === e.target);
			if (e.isIntersecting) {
				S.inView = true;
				if (S.g) grow(S);
			}
		}),
	{ threshold: 0.15 }
);
SCENES.forEach((S) => seen.observe(S.el));

// Lays out what changed size (or everything, when text may have moved), then repaints.
function drawAll(force) {
	if (!T) return;
	for (const S of SCENES) {
		if (!S.el.offsetParent) continue;
		const r = S.el.getBoundingClientRect(), key = `${r.width}x${r.height}@${devicePixelRatio}`;
		if ((force || key !== S.key) && layout(S)) S.key = key;
		paint(S);
		if (S.inView) grow(S);
	}
}

if (SCENES.length) {
	let resizeTimer;
	addEventListener('resize', () => {
		clearTimeout(resizeTimer);
		resizeTimer = setTimeout(drawAll, 60);
	});
	// The theme switch changes the ink and vein colors, not the dots.
	addEventListener('calx:theme', () => SCENES.forEach(paint));

	const tones = new Image();
	tones.onload = () => {
		const c = document.createElement('canvas');
		c.width = tones.width;
		c.height = tones.height;
		const g = c.getContext('2d');
		g.drawImage(tones, 0, 0);
		const px = g.getImageData(0, 0, c.width, c.height).data;
		const w = c.width, h = c.height >> 1, n = w * h, dark = new Float32Array(n), warm = new Float32Array(n);
		for (let i = 0; i < n; i++) {
			dark[i] = px[i * 4] / 255;
			warm[i] = px[(i + n) * 4] / 255;
		}
		T = { w, h, dark, warm };
		drawAll(true);
		// The 404's glyph and the clearings depend on the fonts' metrics.
		document.fonts?.ready.then(() => drawAll(true));
	};
	tones.src = '/crystal-tones.jpg';
}
