// qBitFusion website. Plain JavaScript, no dependencies.
"use strict";

const reduceMotion = window.matchMedia("(prefers-reduced-motion: reduce)");
const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
const $ = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

// "Both at once" (the app's "Download from the debrid service and from peers at the same time"). Several
// switches on the page show it; flipping one flips them all and tells the illustrations.
const combined = {
    on: true,
    listeners: [],
    set(on) {
        this.on = on;
        $$("[data-combined]").forEach((s) => { s.checked = on; });
        this.listeners.forEach((f) => f(on));
    },
    listen(f) { this.listeners.push(f); },
};
document.addEventListener("change", (e) => {
    if (e.target.matches("[data-combined]")) combined.set(e.target.checked);
});

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

    // In "Debrid preferred", one source at a time: the service first, then peers for what it lacks.
    // With "Both at once", peers work alongside the service from the start.
    let phase = "";
    const both = () => mode === "preferred" && combined.on;

    function setStates() {
        const httpsOn = mode !== "bittorrent";
        const peersOn = mode !== "debrid";
        const finished = verified >= N;
        const httpsBusy = httpsOn && !finished && phase !== "peers";
        const peersBusy = peersOn && !finished && (mode !== "preferred" || both() || phase === "peers");
        httpsRow.classList.toggle("is-off", !httpsOn);
        peersRow.classList.toggle("is-off", !peersOn);
        httpsRow.classList.toggle("is-active", httpsBusy);
        peersRow.classList.toggle("is-active", peersBusy);
        httpsState.textContent = !httpsOn ? "Not used"
            : finished ? "Done"
            : httpsBusy ? (mode === "preferred" ? "Sending everything it has" : "Sending")
            : "Done with what it has";
        peersState.textContent = !peersOn ? "Off · no swarm connections"
            : finished ? "Done"
            : mode !== "preferred" ? `${PEER_SLOTS} peers`
            : both() ? "Alongside your service, on other pieces"
            : phase === "peers" ? "Now fetching the file your service lacks"
            : "Waiting for the file your service lacks";
        sim.classList.toggle("is-both", both());
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

    // Both at once: the service takes the first unclaimed pieces in order, a few at a time. A claimed
    // piece is hidden from peers; a failed one is shown to everyone again.
    async function httpsInOrder(gen, end) {
        while (gen === generation) {
            await whenVisible();
            if (gen !== generation) return;
            let i = 0;
            while (i < end && state[i] !== 0) i++;
            if (i >= end) return;
            await fetchPiece(gen, i, "h");
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
        phase = "";
        render();
        setStates();

        const end = mode === "preferred" ? PROVIDER_LACKS : N;
        const every = Array.from({ length: N }, (_, i) => i);
        const lacking = every.slice(PROVIDER_LACKS);
        const peers = (allowed, slots) => Array.from({ length: slots }, () => peerWorker(gen, allowed));
        const https = () => {
            if (both()) return Array.from({ length: HTTPS_SLOTS }, () => httpsInOrder(gen, end));
            const chunk = Math.ceil(end / HTTPS_SLOTS);
            return Array.from({ length: HTTPS_SLOTS }, (_, s) => httpsWorker(gen, s * chunk, Math.min(end, (s + 1) * chunk)));
        };

        if (mode === "debrid") {
            await Promise.all(https());
        } else if (mode === "bittorrent") {
            await Promise.all(peers(every, PEER_SLOTS));
        } else if (both()) {
            await Promise.all([...https(), ...peers(every, 6)]);
        } else {
            await Promise.all(https());
            if (gen !== generation) return;
            phase = "peers";
            setStates();
            await Promise.all(peers(lacking, 6));
        }
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
        // "Both at once" applies only to "Debrid preferred".
        $("[data-sim-combined]").hidden = mode !== "preferred";
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
    combined.listen(() => { if (mode === "preferred") run(); });
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
        const seeded = (seed) => () => ((seed = (seed * 16807) % 2147483647) / 2147483647);
        const inRuns = (time, runs) => runs.some(([a, b]) => time >= a && time < b);

        // One source at a time: the default. When each piece finishes, and from where, is fixed so the
        // story can be scrubbed.
        function serialStory() {
            const T = { drop: 6, swarmOn: 7, back: 12, serviceOn: 13, done: 16.5, end: 21 };
            const schedule = [];
            const first = 16;
            for (let i = 0; i < first; i++) schedule.push({ i, src: "h", at: 0.5 + (i * (T.drop - 0.7)) / first });
            const rnd = seeded(7);
            const rest = Array.from({ length: N - first }, (_, k) => first + k);
            const swarmPicks = [];
            while (swarmPicks.length < 8) {
                const k = rest[Math.floor(rnd() * rest.length)];
                if (!swarmPicks.includes(k)) swarmPicks.push(k);
            }
            swarmPicks.forEach((i, n) => schedule.push({ i, src: "p", at: T.swarmOn + 0.5 + n * ((T.back + 0.6 - T.swarmOn - 0.5) / swarmPicks.length) }));
            rest.filter((i) => !swarmPicks.includes(i))
                .forEach((i, n, left) => schedule.push({ i, src: "h", at: T.serviceOn + 0.3 + n * ((T.done - T.serviceOn - 0.5) / left.length) }));

            return {
                T, schedule,
                steps: [0, T.drop, T.back, T.done],
                labels: ["Starts on your service", "The service drops out", "It comes back", "Done, all checked"],
                captions: [
                    "It starts on your debrid service, at the full speed of your connection.",
                    "The service drops out. qBitFusion hands the download to the torrent swarm, if you've allowed that for this torrent. Every finished piece stays put.",
                    "Your service is back. The swarm finishes the pieces it was working on, and the service takes over the rest.",
                    "Done. Every piece was checked against the torrent's own fingerprint, wherever it came from.",
                ],
                label: "Animated illustration: a download starts from the debrid service, switches to the torrent swarm when the service drops out, switches back when it returns, and finishes with every piece checked.",
                serviceRuns: [[0, T.drop - 0.2], [T.serviceOn, T.done - 0.3]],
                swarmRuns: [[T.swarmOn, T.serviceOn - 0.4]],
                window: { h: 1, p: 1 },
                rates: { h: 6, p: 2.6 },
                reserve: () => [],
                state(t) {
                    return {
                        service: t < T.drop ? "Sending" : t < T.back ? "Dropped out" : t < T.serviceOn ? "Back online" : t < T.done ? "Sending" : "Done",
                        swarm: t < T.drop ? "Standing by" : t < T.swarmOn ? "Taking over" : t < T.back ? "Sending" : t < T.serviceOn ? "Finishing its pieces" : t < T.done ? "Standing by" : "Done",
                        source: t < T.drop ? "Your debrid service" : t < T.swarmOn ? "Switching…" : t < T.serviceOn ? "Torrent swarm" : t < T.done ? "Your debrid service" : "Complete",
                        speed: t < T.drop ? "6.1 MB/s" : t < T.swarmOn ? "" : t < T.serviceOn ? "3.7 MB/s" : t < T.done ? "6.1 MB/s" : "",
                    };
                },
            };
        }

        // Both at once. The service walks the pieces in order a few at a time, and only those are hidden
        // from peers; peers take others. When the service drops out, peers simply carry on. Near the end,
        // the faster source (here the service) finishes alone.
        function combinedStory() {
            const T = { drop: 5.5, back: 10, serviceOn: 10.6, tail: Infinity, done: 0, end: 0 };
            const HTTP_WINDOW = 3;
            const rate = { h: 2.0, p: 1.2 }; // pieces a second, in the same ratio as the speeds shown
            const rnd = seeded(11);
            const finished = new Array(N).fill(false);
            const schedule = [];
            const windowAt = () => {
                const w = [];
                for (let i = 0; i < N && w.length < HTTP_WINDOW; i++) if (!finished[i]) w.push(i);
                return w;
            };
            let count = 0;
            let accH = 0;
            let accP = 0;
            const step = 0.01;
            for (let t = 0; count < N && t < 60; t += step) {
                const serviceOn = (t >= 0.4 && t < T.drop) || t >= T.serviceOn;
                if (T.tail === Infinity && t >= T.serviceOn && N - count <= 6) T.tail = t;
                const peersOn = t >= 0.8 && t < T.tail;
                if (serviceOn && (accH += rate.h * step) >= 1) {
                    accH -= 1;
                    const i = windowAt()[0];
                    finished[i] = true;
                    count++;
                    schedule.push({ i, src: "h", at: t });
                }
                if (peersOn && count < N && (accP += rate.p * step) >= 1) {
                    accP -= 1;
                    const held = serviceOn ? windowAt() : [];
                    const free = [];
                    for (let i = 0; i < N; i++) if (!finished[i] && !held.includes(i)) free.push(i);
                    if (free.length) {
                        const i = free[Math.floor(rnd() * free.length)];
                        finished[i] = true;
                        count++;
                        schedule.push({ i, src: "p", at: t });
                    }
                }
                T.done = t + 0.2;
            }
            T.end = T.done + 4.5;

            return {
                T, schedule,
                steps: [0, T.drop, T.back, T.done],
                labels: ["Both at once", "The service drops out", "It comes back", "Done, all checked"],
                captions: [
                    "Your service and the swarm work at the same time, on different pieces. The service goes through the file in order, peers fill in around it, and neither fetches a piece the other is on.",
                    "The service drops out. The swarm is already connected, so it simply carries on with the rest. Every finished piece stays put.",
                    "Your service is back, and the two share the work again. For the last few pieces, whichever source is faster finishes alone.",
                    "Done. Every piece was checked against the torrent's own fingerprint, wherever it came from.",
                ],
                label: "Animated illustration: the debrid service and the torrent swarm download different pieces at the same time. When the service drops out the swarm carries on, when it returns both work again, and the download finishes with every piece checked.",
                serviceRuns: [[0.4, T.drop - 0.2], [T.serviceOn, T.done - 0.3]],
                swarmRuns: [[0.8, T.tail - 0.3]],
                window: { h: HTTP_WINDOW, p: 2 },
                rates: { h: 6, p: 3.4 },
                // The pieces the service holds at a moment: the first few unfinished ones, in order.
                reserve: (t, done) => {
                    const on = (t >= 0.4 && t < T.drop) || (t >= T.serviceOn && t < T.done);
                    if (!on) return [];
                    const w = [];
                    for (let i = 0; i < N && w.length < HTTP_WINDOW; i++) if (!done[i]) w.push(i);
                    return w;
                },
                state(t) {
                    const both = (t < T.drop) || (t >= T.serviceOn && t < T.tail);
                    return {
                        service: t < T.drop ? "Sending" : t < T.back ? "Dropped out" : t < T.serviceOn ? "Back online" : t < T.tail ? "Sending" : t < T.done ? "Finishing the last pieces" : "Done",
                        swarm: t < 0.8 ? "Connecting" : t < T.drop ? "Sending" : t < T.serviceOn ? "Carrying on alone" : t < T.tail ? "Sending" : t < T.done ? "Paused for the last pieces" : "Done",
                        source: t >= T.done ? "Complete" : both ? "Your service + peers" : t < T.serviceOn ? "Torrent swarm" : "Your debrid service",
                        speed: t >= T.done ? "" : both ? "9.8 MB/s" : t < T.serviceOn ? "3.7 MB/s" : "6.1 MB/s",
                    };
                },
            };
        }

        const stories = { serial: serialStory(), combined: combinedStory() };
        let story = combined.on ? stories.combined : stories.serial;

        let layout = null;
        let parts = null;
        let t = 0;
        let playing = !reduceMotion.matches;
        if (!playing) t = story.T.done + 1;

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
                    cols: 20, gap: 3, sourceDx: 76,
                }
                : {
                    // A phone: fewer, larger pieces, and a card that fits them.
                    vb: "0 24 400 600",
                    service: [100, 92], swarm: [300, 92],
                    card: { x: 22, y: 352, w: 356, h: 0 },
                    pService: "M100 176 C 100 266, 160 266, 160 352",
                    pSwarm: "M300 176 C 300 266, 240 266, 240 352",
                    cols: 10, gap: 6, sourceDx: 88,
                };
            const cw = (L.card.w - 44 - L.gap * (L.cols - 1)) / L.cols;
            const rows = Math.ceil(N / L.cols);
            const gridBottom = 92 + rows * (cw + L.gap) - L.gap;
            if (!L.card.h) L.card.h = gridBottom + 54;
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
            P.source = svg("text", { x: c.x + L.sourceDx, y: c.y + 70, class: "svg-small bright" }, P.card);
            P.speed = svg("text", { x: c.x + c.w - 22, y: c.y + 70, class: "svg-small", "text-anchor": "end" }, P.card);

            const { cols, gap } = L;
            P.cells = [];
            for (let i = 0; i < N; i++) {
                const r = Math.floor(i / cols);
                const x = c.x + 22 + (i % cols) * (cw + gap);
                const y = c.y + 92 + r * (cw + gap);
                P.cells.push(svg("rect", { x: x.toFixed(1), y: y.toFixed(1), width: cw.toFixed(1), height: cw.toFixed(1), rx: wide ? 3 : 5, class: "fl-cell" }, P.card));
            }
            const below = c.y + gridBottom + 30;
            P.note = svg("text", { x: c.x + 22, y: below, class: "svg-small" }, P.card);
            P.check = svg("text", { x: c.x + c.w - 22, y: below, class: "svg-small ok", "text-anchor": "end" }, P.card);
            parts = P;
        }

        function stepAt(time) {
            const s = story.steps;
            return time < s[1] ? 0 : time < s[2] ? 1 : time < s[3] ? 2 : 3;
        }

        let shownStep = -1;
        function draw() {
            build();
            const P = parts;
            const { T } = story;
            const still = reduceMotion.matches && !playing;

            // Pieces.
            const done = new Array(N).fill(false);
            let count = 0;
            let kept = 0;
            for (const s of story.schedule) {
                const finished = s.at <= t;
                P.cells[s.i].setAttribute("class", finished ? `fl-cell ${s.src}` : "fl-cell");
                if (finished) {
                    done[s.i] = true;
                    count++;
                    if (s.at < T.drop) kept++;
                }
            }
            // Pieces in flight: the service's next few (hidden from peers when both run), and the peers' next.
            const serviceSending = inRuns(t, story.serviceRuns);
            const swarmSending = inRuns(t, story.swarmRuns);
            const held = story.reserve(t, done);
            const flying = (src, n) => story.schedule.filter((s) => s.src === src && s.at > t).sort((a, b) => a.at - b.at).slice(0, n);
            if (serviceSending) (held.length ? held : flying("h", story.window.h).map((s) => s.i))
                .forEach((i) => P.cells[i].setAttribute("class", "fl-cell flight h"));
            if (swarmSending) flying("p", story.window.p).forEach((s) => P.cells[s.i].setAttribute("class", "fl-cell flight p"));
            P.pct.textContent = `${Math.round((count / N) * 100)}%`;

            // Lines and packets.
            const broken = t >= T.drop && t < T.back;
            P.lineService.setAttribute("class", `fl-line service${serviceSending ? " on" : ""}${broken ? " broken" : ""}`);
            P.lineSwarm.setAttribute("class", `fl-line swarm${swarmSending ? " on" : ""}`);
            P.cut.setAttribute("class", `fl-cut${broken ? " on" : ""}`);
            P.service.setAttribute("class", `fl-node${broken ? " down" : ""}`);
            const q = still ? -1 : t;
            const runAt = (runs) => runs.filter(([a]) => q >= a).pop() ?? runs[0];
            const [sa, sb] = runAt(story.serviceRuns);
            const [wa, wb] = runAt(story.swarmRuns);
            P.pktService(q, sa, sb, story.rates.h, 0.7);
            P.pktSwarm(q, wa, wb, story.rates.p, 1.3);

            // Status text.
            const st = story.state(t);
            P.serviceState.textContent = st.service;
            P.swarmState.textContent = st.swarm;
            P.source.textContent = st.source;
            P.speed.textContent = st.speed;
            P.note.textContent = broken ? `Kept all ${kept} finished pieces` : "";
            P.check.textContent = t >= T.done ? `All ${N} pieces checked ✓` : "";
            P.cardBox.setAttribute("class", `fl-card-box${t >= T.done ? " done" : ""}`);

            // Steps and caption.
            const step = stepAt(t);
            if (step !== shownStep) {
                shownStep = step;
                caption.textContent = story.captions[step];
                stepButtons.forEach((b, i) => {
                    if (i === step) b.setAttribute("aria-current", "step");
                    else b.removeAttribute("aria-current");
                });
            }
        }

        function useStory(next) {
            story = next;
            stepButtons.forEach((b, i) => {
                const num = Object.assign(document.createElement("span"), { textContent: String(i + 1) });
                b.replaceChildren(num, story.labels[i]);
            });
            art.setAttribute("aria-label", story.label);
            t = playing ? 0 : story.T.done + 1;
            shownStep = -1;
            draw();
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
                if (t >= story.T.end) t = 0;
            }
            draw();
        });

        stepButtons.forEach((b, i) => b.addEventListener("click", () => {
            // With reduced motion, show the end of the step; otherwise play it from its start.
            const ends = [...story.steps.slice(1), story.T.end];
            t = reduceMotion.matches ? ends[i] - 0.01 : story.steps[i];
            shownStep = -1;
            draw();
        }));
        playBtn.addEventListener("click", () => {
            if (!playing && t >= story.T.end - 0.05) t = 0;
            setPlaying(!playing);
        });
        combined.listen((on) => useStory(on ? stories.combined : stories.serial));
        new ResizeObserver(() => { const before = layout; build(); if (before !== layout) draw(); }).observe(stage);
        setPlaying(playing);
        useStory(story);
    }
}

/* ---------- Screenshots that follow the accent ---------- */

// The screenshots were taken with the Ember accent. For another accent, the pixels close to Ember's hue are
// recolored in a canvas, keeping their lightness and relative saturation, so the window looks as it would with
// that accent. Areas listed in data-accent-keep ("x y w h, ...", in image pixels) are left alone, such as the logo.

const EMBER = "#ff5b2e";

function rgbToHsl(r, g, b) {
    r /= 255; g /= 255; b /= 255;
    const max = Math.max(r, g, b);
    const min = Math.min(r, g, b);
    const l = (max + min) / 2;
    const d = max - min;
    if (!d) return [0, 0, l];
    const s = d / (1 - Math.abs(2 * l - 1));
    const h = max === r ? ((g - b) / d) % 6 : max === g ? (b - r) / d + 2 : (r - g) / d + 4;
    return [(h * 60 + 360) % 360, s, l];
}

function hslToRgb(h, s, l) {
    const c = (1 - Math.abs(2 * l - 1)) * s;
    const x = c * (1 - Math.abs(((h / 60) % 2) - 1));
    const m = l - c / 2;
    const [r, g, b] = h < 60 ? [c, x, 0] : h < 120 ? [x, c, 0] : h < 180 ? [0, c, x] : h < 240 ? [0, x, c] : h < 300 ? [x, 0, c] : [c, 0, x];
    return [(r + m) * 255, (g + m) * 255, (b + m) * 255];
}

const hexToHsl = (hex) => rgbToHsl(...[1, 3, 5].map((i) => parseInt(hex.slice(i, i + 2), 16)));

function accentShot(img) {
    const original = img.getAttribute("src");
    const keep = (img.dataset.accentKeep || "").split(",").filter(Boolean).map((r) => r.trim().split(/\s+/).map(Number));
    const [eh, es, el] = hexToHsl(EMBER);
    let canvas, ctx, out, base, idx, weight, hsl;
    let gen = 0;
    let url = null;

    // Find the accent-colored pixels once, with how strongly each one is accent.
    function analyse() {
        const w = img.naturalWidth;
        const h = img.naturalHeight;
        canvas = Object.assign(document.createElement("canvas"), { width: w, height: h });
        ctx = canvas.getContext("2d", { willReadFrequently: true });
        ctx.drawImage(img, 0, 0);
        out = ctx.getImageData(0, 0, w, h);
        base = out.data.slice();
        const found = [];
        for (let p = 0; p < base.length; p += 4) {
            const r = base[p], g = base[p + 1], b = base[p + 2];
            const chroma = Math.max(r, g, b) - Math.min(r, g, b);
            if (chroma < 12 || r < g || r < b) continue;
            const [ph, ps, pl] = rgbToHsl(r, g, b);
            const dist = Math.min(Math.abs(ph - eh), 360 - Math.abs(ph - eh));
            const k = clamp((20 - dist) / 10, 0, 1) * clamp((chroma - 12) / 20, 0, 1);
            if (k < 0.02) continue;
            const x = (p / 4) % w;
            const y = Math.floor(p / 4 / w);
            if (keep.some(([kx, ky, kw, kh]) => x >= kx && x < kx + kw && y >= ky && y < ky + kh)) continue;
            found.push(p, k, ps, pl);
        }
        const n = found.length / 4;
        idx = new Uint32Array(n);
        weight = new Float32Array(n);
        hsl = new Float32Array(n * 2);
        for (let i = 0; i < n; i++) {
            idx[i] = found[i * 4];
            weight[i] = found[i * 4 + 1];
            hsl[i * 2] = found[i * 4 + 2];
            hsl[i * 2 + 1] = found[i * 4 + 3];
        }
    }

    return async (accent) => {
        const mine = ++gen;
        if (accent.toLowerCase() === EMBER) {
            if (url) {
                img.src = original;
                URL.revokeObjectURL(url);
                url = null;
            }
            return;
        }
        if (!canvas) {
            if (!img.complete || !img.naturalWidth) {
                await new Promise((resolve) => img.addEventListener("load", resolve, { once: true }));
                if (mine !== gen) return;
            }
            analyse();
        }

        // Map Ember's lightness onto the new accent's, keeping black black and white white.
        const [th, ts, tl] = hexToHsl(accent);
        const light = (l) => (l <= el ? (l * tl) / el : tl + ((l - el) * (1 - tl)) / (1 - el));
        const d = out.data;
        for (let i = 0; i < idx.length; i++) {
            const p = idx[i];
            const k = weight[i];
            const [r, g, b] = hslToRgb(th, clamp((hsl[i * 2] * ts) / es, 0, 1), clamp(light(hsl[i * 2 + 1]), 0, 1));
            d[p] = base[p] + (r - base[p]) * k;
            d[p + 1] = base[p + 1] + (g - base[p + 1]) * k;
            d[p + 2] = base[p + 2] + (b - base[p + 2]) * k;
        }
        ctx.putImageData(out, 0, 0);

        const blob = await new Promise((resolve) => canvas.toBlob(resolve, "image/jpeg", 0.92));
        if (mine !== gen || !blob) return;
        const next = URL.createObjectURL(blob);
        // Load it off-screen first, so the swap doesn't flash.
        await new Promise((resolve) => {
            const ready = new Image();
            ready.onload = ready.onerror = resolve;
            ready.src = next;
        });
        if (mine !== gen) {
            URL.revokeObjectURL(next);
            return;
        }
        img.src = next;
        if (url) URL.revokeObjectURL(url);
        url = next;
    };
}

/* ---------- Accent picker ---------- */

{
    const swatches = $$("[data-accent-picker] button");
    const shots = $$("img[data-accent-shot]").map(accentShot);
    const pick = (b, focus) => {
        swatches.forEach((s) => {
            const on = s === b;
            s.setAttribute("aria-checked", String(on));
            s.tabIndex = on ? 0 : -1;
        });
        const accent = b.style.getPropertyValue("--c").trim();
        document.documentElement.style.setProperty("--accent", accent);
        shots.forEach((recolor) => recolor(accent));
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

    const started = $("[data-release-started]");

    // The site's build puts the latest release's zip in download/, beside latest.json, so the download comes
    // from this site and the visitor stays on the page. Without it, the build-from-source steps stay.
    fetch("download/latest.json", { cache: "no-cache" })
        .then((r) => (r.ok ? r.json() : null))
        .then((rel) => {
            if (!rel || !/^qBitFusion-[\d.]+-windows-x64\.zip$/.test(rel.file)) return;

            link.href = `download/${rel.file}`;
            link.download = rel.file;
            link.textContent = `Download v${rel.version}`;
            const part = (text, cls) => Object.assign(document.createElement("span"), { textContent: text, className: cls ?? "" });
            meta.replaceChildren(part(rel.file), part(" · ", "sep"), part(`${mib(rel.size)} · ${date(rel.published)}`));
            const links = document.createElement("span");
            links.className = "release-links";
            const add = (href, text, file) => {
                const a = Object.assign(document.createElement("a"), { href, className: "link-chevron", textContent: text });
                if (file) a.download = file;
                links.append(a);
            };
            add(rel.notes, "Release notes");
            add(`download/${rel.file}.sha256`, "SHA-256", `${rel.file}.sha256`);
            meta.after(links);

            // What to do next, in place of the SmartScreen note it repeats.
            link.addEventListener("click", () => {
                started.hidden = false;
                $("[data-release-fine]").hidden = true;
            });

            ready.hidden = false;
            none.hidden = true;
        })
        .catch(() => { /* Keep the build-from-source instructions. */ });
}
