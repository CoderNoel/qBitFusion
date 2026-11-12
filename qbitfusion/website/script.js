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

/* ---------- Scroll-linked effect: hero tilt ---------- */

{
    const shot = $("[data-tilt] .shot");
    let queued = false;
    const update = () => {
        queued = false;
        if (!shot) return;
        if (reduceMotion.matches) {
            shot.style.removeProperty("--tilt");
            shot.style.removeProperty("--scale");
            return;
        }
        const vh = window.innerHeight;
        const top = shot.parentElement.getBoundingClientRect().top;
        const p = clamp(1 - (top - vh * 0.12) / (vh * 0.62), 0, 1);
        const eased = 1 - Math.pow(1 - p, 3);
        shot.style.setProperty("--tilt", `${(16 * (1 - eased)).toFixed(2)}deg`);
        shot.style.setProperty("--scale", (0.92 + 0.08 * eased).toFixed(4));
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
        httpsState.textContent = !httpsOn ? "Not used" : verified >= N ? "Done" : "Sending";
        peersState.textContent = !peersOn ? "Off · no swarm connections"
            : mode === "preferred" ? "For the file your service lacks"
            : verified >= N ? "Done" : `${PEER_SLOTS} peers`;
    }

    function render() {
        const pct = verified / N;
        bar.style.width = `${(pct * 100).toFixed(1)}%`;
        count.textContent = verified >= N ? `All ${N} pieces checked` : `${verified} of ${N} pieces checked`;
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

/* ---------- SVG helpers ---------- */

const SVGNS = "http://www.w3.org/2000/svg";
function svg(tag, attrs = {}, parent = null, text = null) {
    const e = document.createElementNS(SVGNS, tag);
    for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
    if (text !== null) e.textContent = text;
    if (parent) parent.append(e);
    return e;
}

// A small person, used for peers.
function drawPeer(parent, x, y, online, r = 14) {
    const g = svg("g", { class: online ? "peer on" : "peer off", transform: `translate(${x} ${y})` }, parent);
    svg("circle", { r, class: "peer-bg" }, g);
    svg("circle", { cy: -r * 0.22, r: r * 0.26, class: "peer-fg" }, g);
    svg("path", { d: `M${-r * 0.5} ${r * 0.52}a${r * 0.5} ${r * 0.42} 0 0 1 ${r} 0`, class: "peer-fg-line" }, g);
    return g;
}

// A laptop, used for "you".
function drawLaptop(parent, x, y, label) {
    const g = svg("g", { class: "laptop", transform: `translate(${x} ${y})` }, parent);
    svg("rect", { x: -40, y: -30, width: 80, height: 52, rx: 6, class: "laptop-screen" }, g);
    svg("path", { d: "M-50 26h100l-6 8h-88z", class: "laptop-base" }, g);
    if (label) svg("text", { y: 58, class: "svg-label", "text-anchor": "middle" }, g, label);
    return g;
}

// A server rack, used for the debrid service.
function drawServer(parent, x, y, w = 76) {
    const g = svg("g", { class: "server", transform: `translate(${x} ${y})` }, parent);
    for (let i = 0; i < 3; i++) {
        const ry = -36 + i * 25;
        svg("rect", { x: -w / 2, y: ry, width: w, height: 21, rx: 5, class: "server-slab" }, g);
        svg("circle", { cx: -w / 2 + 11, cy: ry + 10.5, r: 2.6, class: "server-led" }, g);
        svg("rect", { x: -w / 2 + 20, y: ry + 9, width: w * 0.42, height: 3, rx: 1.5, class: "server-slot" }, g);
    }
    return g;
}

// Moving dots along a path, derived from time so they can be paused and scrubbed.
function packetLayer(parent, path, cls, max = 14) {
    const len = path.getTotalLength();
    const dots = Array.from({ length: max }, () => svg("circle", { r: 3.6, class: cls, opacity: 0 }, parent));
    return (t, from, to, rate, travel) => {
        let n = 0;
        if (t >= from) {
            const last = Math.floor((Math.min(t, to) - from) * rate);
            for (let k = last; k >= 0 && n < max; k--) {
                const born = from + k / rate;
                const age = t - born;
                if (age < 0 || age > travel) { if (age > travel) break; continue; }
                const p = path.getPointAtLength(len * (age / travel));
                const d = dots[n++];
                d.setAttribute("cx", p.x.toFixed(1));
                d.setAttribute("cy", p.y.toFixed(1));
                d.setAttribute("opacity", Math.min(1, age * 6, (travel - age) * 6).toFixed(2));
            }
        }
        for (; n < max; n++) dots[n].setAttribute("opacity", 0);
    };
}

function runWhenVisible(target, frame) {
    let visible = false;
    let raf = 0;
    let last = 0;
    const loop = (now) => {
        const dt = last ? Math.min(0.1, (now - last) / 1000) : 0;
        last = now;
        frame(dt);
        raf = visible && !document.hidden ? requestAnimationFrame(loop) : 0;
    };
    const kick = () => {
        if (visible && !document.hidden && !raf) { last = 0; raf = requestAnimationFrame(loop); }
    };
    new IntersectionObserver(([e]) => { visible = e.isIntersecting; kick(); }, { threshold: 0.1 }).observe(target);
    document.addEventListener("visibilitychange", kick);
    return kick;
}

/* ---------- "New to debrid?" comparison ---------- */

{
    const root = $("[data-compare]");
    if (root) {
        const scenes = [];

        for (const art of $$("[data-compare-art]", root)) {
            const debrid = art.dataset.compareArt === "debrid";
            const peers = [[64, 58, false], [42, 136, false], [76, 206, false], [146, 110, true]];
            const scene = { debrid };

            if (debrid) {
                // The service fetched the file from the swarm earlier: faint lines into the server.
                for (const [x, y] of peers) svg("path", { d: `M${x} ${y} L${200} 128`, class: "cmp-line faint" }, art);
                peers.forEach(([x, y]) => drawPeer(art, x, y, false, 12));
                drawServer(art, 232, 136);
                svg("text", { x: 232, y: 82, class: "svg-label strong", "text-anchor": "middle" }, art, "Debrid service");
                svg("text", { x: 232, y: 196, class: "svg-tag", "text-anchor": "middle" }, art, "Already has the file");
                scene.path = svg("path", { d: "M274 140 L350 140", class: "cmp-line fast" }, art);
                scene.packets = packetLayer(art, scene.path, "pkt pkt-https", 8);
            } else {
                peers.forEach(([x, y, on]) => drawPeer(art, x, y, on));
                scene.path = svg("path", { d: "M162 114 C 240 104, 290 140, 350 140", class: "cmp-line slow" }, art);
                scene.packets = packetLayer(art, scene.path, "pkt pkt-peer", 4);
            }
            drawLaptop(art, 396, 140, "You");

            svg("rect", { x: 40, y: 262, width: 400, height: 8, rx: 4, class: "cmp-track" }, art);
            scene.fill = svg("rect", { x: 40, y: 262, width: 0, height: 8, rx: 4, class: debrid ? "cmp-fill https" : "cmp-fill peer" }, art);
            scene.left = svg("text", { x: 40, y: 250, class: "svg-small" }, art);
            scene.right = svg("text", { x: 440, y: 250, class: "svg-small", "text-anchor": "end" }, art);
            scenes.push(scene);
        }

        const LOOP = 7;
        let t = 0;
        const draw = () => {
            const still = reduceMotion.matches;
            for (const s of scenes) {
                if (s.debrid) {
                    const p = still ? 1 : clamp(t / 4.2, 0, 1);
                    s.fill.setAttribute("width", (400 * p).toFixed(1));
                    s.left.textContent = "Full speed over HTTPS";
                    s.right.textContent = p >= 1 ? "Done" : "A few minutes";
                    s.packets(still ? -1 : t, 0, 4.2, 7, 0.45);
                } else {
                    const p = still ? 0.12 : 0.14 * (t / LOOP);
                    s.fill.setAttribute("width", (400 * p).toFixed(1));
                    s.left.textContent = "1 of 4 peers online";
                    s.right.textContent = "About 3 days left";
                    s.packets(still ? -1 : t, 0, LOOP, 0.8, 1.8);
                }
            }
        };
        runWhenVisible(root, (dt) => { t = (t + dt) % LOOP; draw(); });
        reduceMotion.addEventListener("change", draw);
        draw();
    }
}

/* ---------- The flow: one download, two sources ---------- */

{
    const stage = $("[data-flow]");
    if (stage) {
        const art = $("[data-flow-art]", stage);
        const caption = $("[data-flow-caption]", stage);
        const stepButtons = $$("[data-step]", stage);
        const playBtn = $("[data-flow-play]", stage);

        const N = 40;
        const T = { drop: 6, swarmOn: 7, back: 12, serviceOn: 13, done: 16.5, end: 21 };
        const STEP_START = [0, T.drop, T.back, T.done];
        const CAPTIONS = [
            "It starts on your debrid service, at the full speed of your connection.",
            "The service drops out. qBitFusion hands the download to the torrent swarm, if you've allowed that for this torrent. Every finished piece stays put.",
            "Your service is back. The swarm finishes the pieces it was working on, and the service takes over the rest.",
            "Done. Every piece was checked against the torrent's own fingerprint, wherever it came from.",
        ];

        // When each piece finishes, and from where. Fixed, so the story can be scrubbed.
        const schedule = [];
        {
            const order = Array.from({ length: N }, (_, i) => i);
            const first = 16;
            for (let i = 0; i < first; i++) schedule.push({ i, src: "h", at: 0.5 + (i * (T.drop - 0.7)) / first });
            let seed = 7;
            const rnd = () => ((seed = (seed * 16807) % 2147483647) / 2147483647);
            const rest = order.slice(first);
            const swarmPicks = [];
            while (swarmPicks.length < 8) {
                const k = rest[Math.floor(rnd() * rest.length)];
                if (!swarmPicks.includes(k)) swarmPicks.push(k);
            }
            swarmPicks.forEach((i, n) => schedule.push({ i, src: "p", at: T.swarmOn + 0.5 + n * ((T.back + 0.6 - T.swarmOn - 0.5) / swarmPicks.length) }));
            const left = rest.filter((i) => !swarmPicks.includes(i));
            left.forEach((i, n) => schedule.push({ i, src: "h", at: T.serviceOn + 0.3 + n * ((T.done - T.serviceOn - 0.5) / left.length) }));
        }

        let layout = null;
        let parts = null;
        let t = 0;
        let playing = !reduceMotion.matches;
        if (!playing) t = T.done + 1;

        function build() {
            const wide = stage.clientWidth >= 700;
            if (layout === (wide ? "wide" : "tall") && parts) return;
            layout = wide ? "wide" : "tall";
            art.textContent = "";

            const L = wide
                ? {
                    vb: "0 56 1000 376",
                    service: [160, 118], swarm: [160, 332],
                    card: { x: 520, y: 122, w: 440, h: 214 },
                    pService: "M240 118 C 390 118, 380 196, 520 196",
                    pSwarm: "M240 332 C 390 332, 380 262, 520 262",
                }
                : {
                    vb: "0 20 400 600",
                    service: [100, 92], swarm: [300, 92],
                    card: { x: 30, y: 360, w: 340, h: 204 },
                    pService: "M100 176 C 100 270, 160 270, 160 360",
                    pSwarm: "M300 176 C 300 270, 240 270, 240 360",
                };
            art.setAttribute("viewBox", L.vb);
            art.classList.toggle("tall", !wide);

            const P = {};
            P.baseService = svg("path", { d: L.pService, class: "fl-base" }, art);
            P.baseSwarm = svg("path", { d: L.pSwarm, class: "fl-base" }, art);
            P.lineService = svg("path", { d: L.pService, class: "fl-line service" }, art);
            P.lineSwarm = svg("path", { d: L.pSwarm, class: "fl-line swarm" }, art);
            P.pktService = packetLayer(art, P.lineService, "pkt pkt-https", 10);
            P.pktSwarm = packetLayer(art, P.lineSwarm, "pkt pkt-peer", 8);

            // The break mark on the service line.
            const mid = P.lineService.getPointAtLength(P.lineService.getTotalLength() * 0.5);
            P.cut = svg("g", { class: "fl-cut", transform: `translate(${mid.x} ${mid.y})` }, art);
            svg("circle", { r: 13 }, P.cut);
            svg("path", { d: "M-5 -5L5 5M5 -5L-5 5" }, P.cut);

            // Service node.
            const [sx, sy] = L.service;
            P.service = svg("g", { class: "fl-node" }, art);
            drawServer(P.service, sx, sy + 4, 86);
            svg("text", { x: sx, y: sy + 60, class: "svg-label strong", "text-anchor": "middle" }, P.service, "Your debrid service");
            P.serviceState = svg("text", { x: sx, y: sy + 80, class: "svg-small", "text-anchor": "middle" }, P.service);

            // Swarm node.
            const [wx, wy] = L.swarm;
            P.swarm = svg("g", { class: "fl-node" }, art);
            const ring = [[0, 0], [-30, -16], [30, -16], [-30, 18], [30, 18], [0, -34], [0, 34]];
            ring.forEach(([dx, dy]) => drawPeer(P.swarm, wx + dx, wy + dy, true, 11));
            svg("text", { x: wx, y: wy + 66, class: "svg-label strong", "text-anchor": "middle" }, P.swarm, "Torrent swarm");
            P.swarmState = svg("text", { x: wx, y: wy + 86, class: "svg-small", "text-anchor": "middle" }, P.swarm);

            // The app card.
            const c = L.card;
            P.card = svg("g", { class: "fl-card" }, art);
            P.cardBox = svg("rect", { x: c.x, y: c.y, width: c.w, height: c.h, rx: 18, class: "fl-card-box" }, P.card);
            svg("image", { href: "assets/logo.png", x: c.x + 22, y: c.y + 20, width: 26, height: 26 }, P.card);
            svg("text", { x: c.x + 58, y: c.y + 39, class: "svg-label strong" }, P.card, "qBitFusion");
            P.pct = svg("text", { x: c.x + c.w - 22, y: c.y + 39, class: "svg-label strong", "text-anchor": "end" }, P.card);
            svg("text", { x: c.x + 22, y: c.y + 70, class: "svg-small" }, P.card, "Source");
            P.source = svg("text", { x: c.x + 76, y: c.y + 70, class: "svg-small bright" }, P.card);

            const cols = 20;
            const gap = 3;
            const cw = (c.w - 44 - gap * (cols - 1)) / cols;
            P.cells = [];
            for (let i = 0; i < N; i++) {
                const r = Math.floor(i / cols);
                const x = c.x + 22 + (i % cols) * (cw + gap);
                const y = c.y + 88 + r * (cw + gap);
                P.cells.push(svg("rect", { x: x.toFixed(1), y: y.toFixed(1), width: cw.toFixed(1), height: cw.toFixed(1), rx: 3, class: "fl-cell" }, P.card));
            }
            const below = c.y + 88 + 2 * (cw + gap) + 26;
            P.note = svg("text", { x: c.x + 22, y: below, class: "svg-small" }, P.card);
            P.check = svg("text", { x: c.x + c.w - 22, y: below, class: "svg-small ok", "text-anchor": "end" }, P.card);
            parts = P;
        }

        function stepAt(time) {
            return time < T.drop ? 0 : time < T.back ? 1 : time < T.done ? 2 : 3;
        }

        let shownStep = -1;
        function draw() {
            build();
            const P = parts;
            const still = reduceMotion.matches && !playing;

            // Pieces.
            let done = 0;
            let kept = 0;
            for (const s of schedule) {
                const cell = P.cells[s.i];
                const finished = s.at <= t;
                cell.setAttribute("class", finished ? `fl-cell ${s.src === "h" ? "h" : "p"}` : "fl-cell");
                if (finished) done++;
                if (finished && s.at < T.drop) kept++;
            }
            // The next piece for the active source is shown as in flight.
            const active = (t < T.drop || (t >= T.serviceOn && t < T.done)) ? "h" : (t >= T.swarmOn && t < T.serviceOn) ? "p" : null;
            if (active) {
                const next = schedule.filter((s) => s.src === active && s.at > t).sort((a, b) => a.at - b.at)[0];
                if (next) P.cells[next.i].setAttribute("class", `fl-cell flight ${active}`);
            }
            P.pct.textContent = `${Math.round((done / N) * 100)}%`;

            // Lines and packets.
            const serviceOn = t < T.drop || (t >= T.serviceOn && t < T.done);
            const swarmOn = t >= T.swarmOn && t < T.serviceOn;
            const broken = t >= T.drop && t < T.back;
            P.lineService.setAttribute("class", `fl-line service${serviceOn ? " on" : ""}${broken ? " broken" : ""}`);
            P.lineSwarm.setAttribute("class", `fl-line swarm${swarmOn ? " on" : ""}`);
            P.cut.setAttribute("class", `fl-cut${broken ? " on" : ""}`);
            P.service.setAttribute("class", `fl-node${broken ? " down" : ""}`);
            const q = still ? -1 : t;
            const second = q >= T.serviceOn;
            P.pktService(q, second ? T.serviceOn : 0, second ? T.done - 0.3 : T.drop - 0.2, 6, 0.7);
            P.pktSwarm(q, T.swarmOn, T.serviceOn - 0.4, 2.6, 1.3);

            // Status text.
            P.serviceState.textContent = t < T.drop ? "Sending" : t < T.back ? "Dropped out" : t < T.serviceOn ? "Back online" : t < T.done ? "Sending" : "Done";
            P.swarmState.textContent = t < T.drop ? "Standing by" : t < T.swarmOn ? "Taking over" : t < T.back ? "Sending" : t < T.serviceOn ? "Finishing its pieces" : t < T.done ? "Standing by" : "Done";
            P.source.textContent = t < T.drop ? "Your debrid service" : t < T.swarmOn ? "Switching…" : t < T.serviceOn ? "Torrent swarm" : t < T.done ? "Your debrid service" : "Complete";
            P.note.textContent = (t >= T.drop && t < T.back) ? `Kept all ${kept} finished pieces` : "";
            P.check.textContent = t >= T.done ? `All ${N} pieces checked ✓` : "";
            P.cardBox.setAttribute("class", `fl-card-box${t >= T.done ? " done" : ""}`);

            // Steps and caption.
            const step = stepAt(t);
            if (step !== shownStep) {
                shownStep = step;
                caption.textContent = CAPTIONS[step];
                stepButtons.forEach((b, i) => {
                    if (i === step) b.setAttribute("aria-current", "step");
                    else b.removeAttribute("aria-current");
                });
            }
        }

        function setPlaying(on) {
            playing = on;
            $("[data-icon-pause]", playBtn).toggleAttribute("hidden", !on);
            $("[data-icon-play]", playBtn).toggleAttribute("hidden", on);
            playBtn.setAttribute("aria-label", on ? "Pause the animation" : "Play the animation");
            if (on) kick();
        }

        const kick = runWhenVisible(stage, (dt) => {
            if (playing) {
                t += dt;
                if (t >= T.end) t = 0;
            }
            draw();
        });

        stepButtons.forEach((b, i) => b.addEventListener("click", () => {
            // With reduced motion, show the end of the step; otherwise play it from its start.
            t = reduceMotion.matches ? [T.drop - 0.01, T.back - 0.01, T.done - 0.01, T.end - 0.01][i] : STEP_START[i];
            shownStep = -1;
            draw();
        }));
        playBtn.addEventListener("click", () => {
            if (!playing && t >= T.end - 0.05) t = 0;
            setPlaying(!playing);
        });
        new ResizeObserver(() => { const before = layout; build(); if (before !== layout) draw(); }).observe(stage);
        setPlaying(playing);
        draw();
    }
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
