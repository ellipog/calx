// The site's select. dropdown(select) puts a button and a list, in the site's style, where a native
// <select> was. The select stays in the page, hidden, and holds the value, so code that reads
// select.value or listens for its 'change' works as it did, and a form still sends it. After setting
// select.value from code, call refresh() on what dropdown() returns to show it.
//
// It behaves as WAI-ARIA's "select-only combobox": focus stays on the button, and the list is read
// out through aria-activedescendant.
// - Closed: Down, Up, Enter, Space or Alt+Down open the list; Home and End open it at the first or
//   last option; typing opens it at the option that starts with what was typed.
// - Open: Up, Down, Home, End, Page Up and Page Down move; Enter, Space, Tab and Alt+Up pick and
//   close; Escape, or a click anywhere else, closes without picking; typing jumps.
// Plain options only, no groups.

let made = 0;

const icon = (id) => `<svg class="i" aria-hidden="true"><use href="#${id}"></use></svg>`;

export function dropdown(select) {
	const id = `dd-${++made}`;
	const options = [...select.options];

	const wrap = document.createElement('div');
	wrap.className = 'dd';
	const button = document.createElement('div');
	button.className = 'dd-button';
	button.id = id;
	button.tabIndex = select.disabled ? -1 : 0;
	button.setAttribute('role', 'combobox');
	button.setAttribute('aria-haspopup', 'listbox');
	button.setAttribute('aria-expanded', 'false');
	button.setAttribute('aria-controls', `${id}-list`);
	if (select.disabled) button.setAttribute('aria-disabled', 'true');
	// Every option's text is in the button, one on top of the other and all but the chosen one
	// hidden, so the button is as wide as the widest, as a select is, and doesn't change width.
	button.innerHTML = `<span class="dd-value"></span>${icon('i-down')}`;
	const values = options.map((o) => {
		const span = document.createElement('span');
		span.textContent = o.text;
		button.firstElementChild.append(span);
		return span;
	});

	const list = document.createElement('ul');
	list.className = 'dd-list';
	list.id = `${id}-list`;
	list.hidden = true;
	list.tabIndex = -1;
	list.setAttribute('role', 'listbox');
	options.forEach((o, i) => {
		const li = document.createElement('li');
		li.id = `${id}-${i}`;
		li.setAttribute('role', 'option');
		if (o.disabled) li.setAttribute('aria-disabled', 'true');
		li.innerHTML = `<span></span>${icon('i-check')}`;
		li.firstElementChild.textContent = o.text;
		list.append(li);
	});
	const items = [...list.children];

	// The select's label names the button, and a click on it comes here rather than to the hidden select.
	const label = select.labels?.[0] ?? (select.getAttribute('aria-labelledby') && document.getElementById(select.getAttribute('aria-labelledby')));
	if (label) {
		label.id ||= `${id}-label`;
		button.setAttribute('aria-labelledby', label.id);
		label.addEventListener('click', (e) => {
			e.preventDefault();
			button.focus();
		});
	} else if (select.getAttribute('aria-label')) button.setAttribute('aria-label', select.getAttribute('aria-label'));

	select.hidden = true;
	select.after(wrap);
	wrap.append(button, list);

	let open = false;
	let active = -1;

	function refresh() {
		const i = select.selectedIndex;
		values.forEach((span, j) => span.classList.toggle('on', j === i));
		items.forEach((li, j) => li.setAttribute('aria-selected', String(j === i)));
	}

	function highlight(i) {
		if (i < 0 || i >= items.length) return;
		items[active]?.classList.remove('active');
		active = i;
		items[i].classList.add('active');
		button.setAttribute('aria-activedescendant', items[i].id);
		// Only the list scrolls: scrollIntoView could move the page too.
		const li = items[i];
		if (li.offsetTop < list.scrollTop) list.scrollTop = li.offsetTop - 5;
		else if (li.offsetTop + li.offsetHeight > list.scrollTop + list.clientHeight) list.scrollTop = li.offsetTop + li.offsetHeight - list.clientHeight + 5;
	}

	// The next option that can be picked, from i in the direction step, or i itself if there's none.
	function usable(i, step) {
		for (let j = i; j >= 0 && j < items.length; j += step) if (!options[j].disabled) return j;
		return active;
	}

	function show(at = select.selectedIndex) {
		if (open || select.disabled) return;
		open = true;
		list.hidden = false;
		button.setAttribute('aria-expanded', 'true');
		// Below the button if it fits, else above; and kept inside the window on the right.
		wrap.classList.remove('dd-up', 'dd-end');
		const r = button.getBoundingClientRect();
		const h = list.offsetHeight;
		if (r.bottom + h + 12 > innerHeight && r.top - h - 12 > 0) wrap.classList.add('dd-up');
		if (r.left + list.offsetWidth + 8 > document.documentElement.clientWidth) wrap.classList.add('dd-end');
		highlight(usable(Math.max(at, 0), 1));
		document.addEventListener('pointerdown', outside, true);
	}

	function hide() {
		if (!open) return;
		open = false;
		list.hidden = true;
		button.setAttribute('aria-expanded', 'false');
		button.removeAttribute('aria-activedescendant');
		items[active]?.classList.remove('active');
		active = -1;
		document.removeEventListener('pointerdown', outside, true);
	}

	function pick(i) {
		if (i < 0 || options[i].disabled) return;
		if (i !== select.selectedIndex) {
			select.selectedIndex = i;
			refresh();
			select.dispatchEvent(new Event('input', { bubbles: true }));
			select.dispatchEvent(new Event('change', { bubbles: true }));
		}
	}

	function outside(e) {
		if (!wrap.contains(e.target)) hide();
	}

	// Typing finds the next option that starts with what was typed in the last half second.
	let typed = '';
	let typedAt = 0;
	function find(key) {
		const now = Date.now();
		typed = now - typedAt > 500 ? key : typed + key;
		typedAt = now;
		const q = typed.toLowerCase();
		// One letter, or the same letter again, goes on to the next option with it; more letters
		// may still match the option that's highlighted.
		const same = [...q].every((c) => c === q[0]);
		const from = Math.max(open ? active : select.selectedIndex, 0) + (same ? 1 : 0);
		for (let k = 0; k < items.length; k++) {
			const j = (from + k) % items.length;
			const text = options[j].text.toLowerCase();
			if (!options[j].disabled && (same ? text.startsWith(q[0]) : text.startsWith(q))) return j;
		}
		return -1;
	}

	button.addEventListener('keydown', (e) => {
		const { key, altKey } = e;
		const printable = key.length === 1 && !e.ctrlKey && !e.metaKey && !altKey && key !== ' ';
		if (!open) {
			if (['ArrowDown', 'ArrowUp', 'Enter', ' '].includes(key)) show();
			else if (key === 'Home') show(usable(0, 1));
			else if (key === 'End') show(usable(items.length - 1, -1));
			else if (printable) {
				const j = find(key);
				show(j >= 0 ? j : undefined);
			} else return;
			e.preventDefault();
			return;
		}
		switch (key) {
			case 'ArrowDown':
				highlight(usable(Math.min(active + 1, items.length - 1), 1));
				break;
			case 'ArrowUp':
				if (altKey) {
					pick(active);
					hide();
				} else highlight(usable(Math.max(active - 1, 0), -1));
				break;
			case 'Home':
				highlight(usable(0, 1));
				break;
			case 'End':
				highlight(usable(items.length - 1, -1));
				break;
			case 'PageDown':
				highlight(usable(Math.min(active + 10, items.length - 1), -1));
				break;
			case 'PageUp':
				highlight(usable(Math.max(active - 10, 0), 1));
				break;
			case 'Enter':
			case ' ':
				pick(active);
				hide();
				break;
			case 'Escape':
				hide();
				e.stopPropagation();
				break;
			case 'Tab':
				pick(active);
				hide();
				return; // and focus moves on as usual
			default:
				if (!printable) return;
				highlight(find(key));
		}
		e.preventDefault();
	});

	button.addEventListener('click', () => (open ? hide() : show()));
	button.addEventListener('blur', () => hide());
	// The list never takes focus, so the button keeps it, and its blur doesn't close the list first.
	list.addEventListener('mousedown', (e) => e.preventDefault());
	list.addEventListener('pointermove', (e) => {
		const li = e.target.closest('[role="option"]');
		if (li && li.getAttribute('aria-disabled') !== 'true') highlight(items.indexOf(li));
	});
	list.addEventListener('click', (e) => {
		const li = e.target.closest('[role="option"]');
		if (!li || li.getAttribute('aria-disabled') === 'true') return;
		pick(items.indexOf(li));
		hide();
		button.focus();
	});

	refresh();
	return { refresh, element: wrap };
}
