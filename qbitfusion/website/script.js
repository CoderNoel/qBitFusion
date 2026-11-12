// qBitFusion website. Plain JavaScript, no dependencies.
"use strict";

const reduceMotion = window.matchMedia("(prefers-reduced-motion: reduce)");
const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
const $ = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

/* ---------- Reveal on scroll ---------- */

{
    const io = new IntersectionObserver((entries) => {
        for (const e of entries) {
            if (e.isIntersecting) {
                e.target.classList.add("in");
                io.unobserve(e.target);
            }
        }
    }, { rootMargin: "0px 0px -8% 0px" });
    $$(".reveal").forEach((el) => io.observe(el));
}

/* ---------- Navigation ---------- */

{
    const nav = $("[data-nav]");
    const links = new Map($$(".nav-links a").map((a) => [a.getAttribute("href").slice(1), a]));
    const io = new IntersectionObserver((entries) => {
        for (const e of entries) {
            if (!e.isIntersecting) continue;
            links.forEach((l) => l.removeAttribute("aria-current"));
            links.get(e.target.id)?.setAttribute("aria-current", "true");
        }
    }, { rootMargin: "-45% 0px -50% 0px" });
    $$("main > section[id]").forEach((s) => io.observe(s));

    const onScroll = () => nav.classList.toggle("is-scrolled", window.scrollY > 8);
    window.addEventListener("scroll", onScroll, { passive: true });
    onScroll();
}

/* ---------- Scroll-linked effects: hero tilt, statement highlight ---------- */

{
    const shot = $("[data-tilt] .shot");
    const statement = $("[data-statement]");
    let words = [];

    if (statement) {
        // Wrap each word so it can light up as the paragraph scrolls past.
        for (const part of $$("span, strong", statement)) {
            const text = part.textContent.trim().split(/\s+/);
            part.textContent = "";
            text.forEach((w, i) => {
                const s = document.createElement("span");
                s.className = "w";
                s.textContent = w;
                part.append(s, i < text.length - 1 ? " " : "");
            });
        }
        words = $$(".w", statement);
    }

    let queued = false;
    const update = () => {
        queued = false;
        const vh = window.innerHeight;

        if (shot) {
            if (reduceMotion.matches) {
                shot.style.removeProperty("--tilt");
                shot.style.removeProperty("--scale");
            } else {
                const top = shot.parentElement.getBoundingClientRect().top;
                const p = clamp(1 - (top - vh * 0.12) / (vh * 0.62), 0, 1);
                const eased = 1 - Math.pow(1 - p, 3);
                shot.style.setProperty("--tilt", `${(16 * (1 - eased)).toFixed(2)}deg`);
                shot.style.setProperty("--scale", (0.92 + 0.08 * eased).toFixed(4));
            }
        }

        if (words.length) {
            let lit = words.length;
            if (!reduceMotion.matches) {
                const r = statement.getBoundingClientRect();
                const p = clamp((vh * 0.78 - r.top) / (r.height + vh * 0.12), 0, 1);
                lit = Math.round(p * words.length);
            }
            words.forEach((w, i) => w.classList.toggle("lit", i < lit));
        }
    };
    const queue = () => { if (!queued) { queued = true; requestAnimationFrame(update); } };
    window.addEventListener("scroll", queue, { passive: true });
    window.addEventListener("resize", queue);
    reduceMotion.addEventListener("change", queue);
    update();
}

/* ---------- Segmented control + piece-map illustration ---------- */

{
    const seg = $("[data-segmented]");
    const thumb = $(".segmented-thumb", seg);
    const tabs = $$("button", seg);
    const sim = $("[data-sim]");
    const map = $("[data-pieces]");
    const bar = $("[data-bar]");
    const count = $("[data-count]");
    const httpsRow = $('[data-source="https"]');
    const peersRow = $('[data-source="peers"]');
    const httpsState = $("[data-state-https]");
    const peersState = $("[data-state-peers]");

    const N = 240;
    const PROVIDER_LACKS = 192;       // in "preferred", pieces from here on are in a file the provider lacks
    const HTTPS_SLOTS = 4;            // the app's default number of parallel requests
    const PEER_SLOTS = 10;
    const FAIL_RATE = 0.025;

    const cells = Array.from({ length: N }, () => {
        const i = document.createElement("i");
        map.append(i);
        return i;
    });

    let mode = "preferred";
    let state = [];                   // 0 empty, 1 in flight, 2 verified
    let verified = 0;
    let generation = 0;
    let visible = false;
    let waiting = [];

    const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
    const rand = (a, b) => a + Math.random() * (b - a);
    const whenVisible = () => (visible && !document.hidden) ? Promise.resolve() : new Promise((r) => waiting.push(r));
    const wake = () => {
        if (visible && !document.hidden) {
            waiting.forEach((r) => r());
            waiting = [];
        }
    };

    function setStates() {
        const httpsOn = mode !== "bittorrent";
        const peersOn = mode !== "debrid";
        httpsRow.classList.toggle("is-off", !httpsOn);
        peersRow.classList.toggle("is-off", !peersOn);
        httpsRow.classList.toggle("is-active", httpsOn && verified < N);
        peersRow.classList.toggle("is-active", peersOn && verified < N);
        httpsState.textContent = !httpsOn ? "Not used" : verified >= N ? "Done" : `${HTTPS_SLOTS} parallel requests`;
        peersState.textContent = !peersOn ? "Off · no swarm connections"
            : mode === "preferred" ? "For the file the provider lacks"
            : verified >= N ? "Done" : `${PEER_SLOTS} peers`;
    }

    function render() {
        const pct = verified / N;
        bar.style.width = `${(pct * 100).toFixed(1)}%`;
        count.textContent = verified >= N ? `All ${N} pieces verified` : `${verified} of ${N} pieces verified`;
    }

    async function fetchPiece(gen, idx, source) {
        state[idx] = 1;
        cells[idx].className = source === "h" ? "f h" : "f";
        await sleep(source === "h" ? rand(260, 420) : rand(500, 1500));
        await whenVisible();
        if (gen !== generation) return false;

        if (Math.random() < FAIL_RATE) {
            cells[idx].className = "x";
            await sleep(520);
            if (gen !== generation) return false;
            cells[idx].className = "";
            state[idx] = 0;
            return false;
        }

        state[idx] = 2;
        cells[idx].className = source;
        verified++;
        render();
        return true;
    }

    async function httpsWorker(gen, from, to) {
        let i = from;
        while (i < to && gen === generation) {
            await whenVisible();
            if (gen !== generation) return;
            if (state[i] === 2) { i++; continue; }
            if (await fetchPiece(gen, i, "h")) i++;
        }
    }

    async function peerWorker(gen, allowed) {
        await sleep(rand(0, 600));
        while (gen === generation) {
            await whenVisible();
            if (gen !== generation) return;
            const free = allowed.filter((i) => state[i] === 0);
            if (!free.length) return;
            await fetchPiece(gen, free[Math.floor(Math.random() * free.length)], "p");
        }
    }

    async function run() {
        const gen = ++generation;
        state = new Array(N).fill(0);
        verified = 0;
        cells.forEach((c, i) => {
            c.className = (mode === "preferred" && i >= PROVIDER_LACKS && state[i] === 0) ? "u" : "";
        });
        render();
        setStates();

        const jobs = [];
        if (mode !== "bittorrent") {
            const end = mode === "preferred" ? PROVIDER_LACKS : N;
            const chunk = Math.ceil(end / HTTPS_SLOTS);
            for (let s = 0; s < HTTPS_SLOTS; s++)
                jobs.push(httpsWorker(gen, s * chunk, Math.min(end, (s + 1) * chunk)));
        }
        if (mode !== "debrid") {
            const allowed = [];
            for (let i = mode === "preferred" ? PROVIDER_LACKS : 0; i < N; i++) allowed.push(i);
            const slots = mode === "preferred" ? 4 : PEER_SLOTS;
            for (let s = 0; s < slots; s++) jobs.push(peerWorker(gen, allowed));
        }

        await Promise.all(jobs);
        if (gen !== generation) return;
        setStates();
        await sleep(3200);
        await whenVisible();
        if (gen === generation) run();
    }

    function placeThumb(animate = true) {
        const sel = tabs.find((t) => t.getAttribute("aria-selected") === "true");
        if (!animate) thumb.style.transition = "none";
        thumb.style.width = `${sel.offsetWidth}px`;
        thumb.style.transform = `translateX(${sel.offsetLeft}px)`;
        if (!animate) { thumb.offsetWidth; thumb.style.transition = ""; }
    }

    function select(tab, focus = false) {
        tabs.forEach((t) => {
            const on = t === tab;
            t.setAttribute("aria-selected", String(on));
            t.tabIndex = on ? 0 : -1;
        });
        if (focus) tab.focus();
        placeThumb();
        mode = tab.dataset.mode;
        $$("[data-mode-copy]").forEach((a) => { a.hidden = a.dataset.modeCopy !== mode; });
        run();
    }

    tabs.forEach((t) => {
        t.tabIndex = t.getAttribute("aria-selected") === "true" ? 0 : -1;
        t.addEventListener("click", () => { if (t.getAttribute("aria-selected") !== "true") select(t); });
    });
    seg.addEventListener("keydown", (e) => {
        const i = tabs.indexOf(document.activeElement);
        if (i < 0) return;
        const step = { ArrowRight: 1, ArrowDown: 1, ArrowLeft: -1, ArrowUp: -1 }[e.key];
        if (e.key === "Home" || e.key === "End" || step) {
            e.preventDefault();
            const next = e.key === "Home" ? 0 : e.key === "End" ? tabs.length - 1 : (i + step + tabs.length) % tabs.length;
            select(tabs[next], true);
        }
    });

    new ResizeObserver(() => placeThumb(false)).observe(seg);
    new IntersectionObserver(([e]) => { visible = e.isIntersecting; wake(); }, { threshold: 0.15 }).observe(sim);
    document.addEventListener("visibilitychange", wake);
    placeThumb(false);
    run();
}

/* ---------- Accent picker ---------- */

{
    const swatches = $$("[data-accent-picker] button");
    const pick = (b, focus) => {
        swatches.forEach((s) => {
            const on = s === b;
            s.setAttribute("aria-checked", String(on));
            s.tabIndex = on ? 0 : -1;
        });
        document.documentElement.style.setProperty("--accent", b.style.getPropertyValue("--c"));
        if (focus) b.focus();
    };
    swatches.forEach((b, i) => {
        b.tabIndex = i === 0 ? 0 : -1;
        b.addEventListener("click", () => pick(b));
        b.addEventListener("keydown", (e) => {
            const step = { ArrowRight: 1, ArrowDown: 1, ArrowLeft: -1, ArrowUp: -1 }[e.key];
            if (!step) return;
            e.preventDefault();
            pick(swatches[(i + step + swatches.length) % swatches.length], true);
        });
    });
}

/* ---------- Copy button ---------- */

{
    for (const btn of $$("[data-copy]")) {
        if (btn.tagName !== "BUTTON") continue;
        btn.addEventListener("click", async () => {
            const code = btn.parentElement.querySelector("code").textContent;
            try {
                await navigator.clipboard.writeText(code);
                btn.textContent = "Copied";
            } catch {
                btn.textContent = "Select and copy";
            }
            setTimeout(() => { btn.textContent = "Copy"; }, 1800);
        });
    }
}

/* ---------- Latest release ---------- */

{
    const ready = $("[data-release-ready]");
    const none = $("[data-release-none]");
    const link = $("[data-release-link]");
    const meta = $("[data-release-meta]");

    const mib = (bytes) => `${(bytes / 1048576).toFixed(bytes > 104857600 ? 0 : 1)} MB`;
    const date = (iso) => new Date(iso).toLocaleDateString(undefined, { year: "numeric", month: "long", day: "numeric" });

    fetch("https://api.github.com/repos/CoderNoel/qBitFusion/releases?per_page=10", {
        headers: { Accept: "application/vnd.github+json" },
    })
        .then((r) => (r.ok ? r.json() : []))
        .then((releases) => {
            for (const rel of releases) {
                if (rel.draft) continue;
                // The release workflow publishes qBitFusion-<version>-windows-x64.zip and a .sha256 beside it.
                const zip = rel.assets.find((a) => /-windows-x64\.zip$/i.test(a.name))
                    ?? rel.assets.find((a) => /\.zip$/i.test(a.name));
                if (!zip) continue;
                const sum = rel.assets.find((a) => a.name === `${zip.name}.sha256`);

                link.href = zip.browser_download_url;
                link.textContent = `Download ${rel.tag_name}`;
                meta.textContent = `${zip.name} · ${mib(zip.size)} · ${date(rel.published_at)}`;
                const links = document.createElement("span");
                links.className = "release-links";
                const add = (href, text) => {
                    const a = document.createElement("a");
                    a.href = href;
                    a.className = "link-chevron";
                    a.textContent = text;
                    links.append(a);
                };
                add(rel.html_url, "Release notes");
                if (sum) add(sum.browser_download_url, "SHA-256");
                meta.after(links);

                ready.hidden = false;
                none.hidden = true;
                return;
            }
        })
        .catch(() => { /* Keep the build-from-source instructions. */ });
}
