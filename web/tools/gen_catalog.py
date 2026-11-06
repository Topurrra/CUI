#!/usr/bin/env python3
"""Builds site/catalog.js: every documented component with its docs (as HTML),
the live scenes that show it, and the C source of each scene with file and
line references.
    gen_catalog.py <repo-root> <out.js>"""
import html, json, re, sys

ROOT, OUT = sys.argv[1], sys.argv[2]
import os
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXTRA_REL = os.path.relpath(os.path.join(HERE, 'host', 'extra_scenes.inc'), ROOT)

def read(rel):
    return open(f"{ROOT}/{rel}", encoding="utf-8").read()

def slug(title):
    s = title.lower().strip()
    s = re.sub(r"[^\w\- ]", "", s)
    return s.replace(" ", "-")

# ---------------------------------------------------------------- scenes
SHOTS = "tests/component_shots.c"
EXTRA = EXTRA_REL

def scene_table():
    src = read(SHOTS)
    body = re.search(r"static const scene scenes\[\] = \{(.*?)\n    \};", src, re.S).group(1)
    extra = re.search(r"#define WEB_EXTRA_SCENES(.*?)\n\n|#define WEB_EXTRA_SCENES(.*)$", read(EXTRA), re.S)
    extra = (extra.group(1) or extra.group(2)) if extra else ""
    out = {}
    for text, file in ((body, SHOTS), (extra, EXTRA)):
        for m in re.finditer(r'\{\s*"([^"]+)",\s*(\w+),\s*([\d.]+),\s*([\d.]+)((?:,\s*\w+)*)\s*\}', text):
            flags = [f.strip() for f in m.group(5).split(",") if f.strip()]
            out[m.group(1)] = {"fn": m.group(2), "w": float(m.group(3)), "h": float(m.group(4)), "file": file,
                               "dark_only": len(flags) > 1 and flags[1] == "true",
                               "accent": flags[2] if len(flags) > 2 else "CUI_ACCENT_NEUTRAL"}
    return out

def brace_block(src, start):
    """Returns the end index of the {...} block whose first '{' is at or after start."""
    i = src.index("{", start)
    depth, n = 0, len(src)
    in_str = in_chr = in_line = in_block = False
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if in_line:
            if c == "\n": in_line = False
        elif in_block:
            if c == "*" and nxt == "/": in_block = False; i += 1
        elif in_str:
            if c == "\\": i += 1
            elif c == '"': in_str = False
        elif in_chr:
            if c == "\\": i += 1
            elif c == "'": in_chr = False
        else:
            if c == "/" and nxt == "/": in_line = True; i += 1
            elif c == "/" and nxt == "*": in_block = True; i += 1
            elif c == '"': in_str = True
            elif c == "'": in_chr = True
            elif c == "{": depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0: return i + 1
        i += 1
    return n

def find_function(src, fn):
    """Finds a scene function: SCENE(name) { ... } or static void name(...) { ... }."""
    pats = []
    if fn.startswith("scene_"):
        pats.append(r"^SCENE\(" + re.escape(fn[6:]) + r"\)\s*\{")
    pats.append(r"^static\s+void\s+" + re.escape(fn) + r"\s*\([^;{]*\)\s*\{")
    for p in pats:
        m = re.search(p, src, re.M)
        if m:
            end = brace_block(src, m.start())
            # keep the comment block directly above
            start = m.start()
            above = src[:start].rstrip("\n").rsplit("\n", 1)
            while above and above[-1].lstrip().startswith(("/*", "*", "//")) and not above[-1].strip().endswith("{"):
                start = src.rfind(above[-1], 0, start)
                above = src[:start].rstrip("\n").rsplit("\n", 1)
            return start, end
    return None

def scene_sources(table):
    files = {SHOTS: read(SHOTS), EXTRA: read(EXTRA)}
    out = {}
    for name, s in table.items():
        src = files[s["file"]]
        parts, seen = [], set()
        queue = [s["fn"]]
        while queue and len(parts) < 4:
            fn = queue.pop(0)
            if fn in seen: continue
            seen.add(fn)
            loc = find_function(src, fn)
            if not loc: continue
            a, b = loc
            line = src.count("\n", 0, a) + 1
            text = src[a:b]
            parts.append({"line": line, "text": text})
            # helpers the scene builds with (body_x, x_body, *_body ...)
            for h in re.findall(r"\b(body_\w+|\w+_body)\s*\(", text):
                if h not in seen and h not in queue and h != "frame_wrap": queue.append(h)
            for h in re.findall(r"\bframe_wrap\s*\(\s*ui\s*,\s*(\w+)\s*\)", text):
                if h not in seen: queue.append(h)
            for h in re.findall(r"\bstage\s*\(\s*ui\s*,\s*&?\w+\s*,\s*(\w+)\s*\)", text):
                if h not in seen: queue.append(h)
            for h in re.findall(r"\b(scene_\w+)\s*\(\s*ui\s*,", text):
                if h not in seen and h != fn: queue.append(h)
        # a SCENE() scene is two pieces in the same place; show them in file order
        parts.sort(key=lambda p: p["line"])
        out[name] = {"file": s["file"], "line": parts[0]["line"] if parts else 1,
                     "source": "\n\n".join(p["text"] for p in parts),
                     "w": s["w"], "h": s["h"], "dark_only": s["dark_only"]}
    return out

# ---------------------------------------------------------------- markdown
def inline(text, anchors):
    parts = re.split(r"(`[^`]+`)", text)
    out = []
    for p in parts:
        if p.startswith("`") and p.endswith("`") and len(p) > 1:
            out.append(f"<code>{html.escape(p[1:-1])}</code>")
            continue
        p = html.escape(p, quote=False)
        # images in running text or table cells (component screenshots are shown live instead)
        p = re.sub(r"!\[([^\]]*)\]\((images/[\w/\-]+\.png)\)", r'<img class="doc-img" src="\2" alt="\1" loading="lazy">', p)
        p = re.sub(r"!\[([^\]]*)\]\(([^)]+)\)", "", p)
        def link(m):
            label, target = m.group(1), m.group(2)
            anchor = target.split("#", 1)[1] if "#" in target else None
            if target.startswith("http"):
                return f'<a href="{target}" target="_blank" rel="noreferrer">{label}</a>'
            if anchor and anchor in anchors:
                return f'<a href="#/c/{anchors[anchor]}">{label}</a>'
            doc = re.match(r"^(?:\.\./)?(?:docs/)?([\w-]+)\.md(?:#([\w-]+))?$", target)
            if doc and doc.group(1) in GUIDE_IDS:
                return f'<a href="#/guide/{doc.group(1)}' + (f'/{doc.group(2)}' if doc.group(2) else '') + f'">{label}</a>'
            if doc and doc.group(1) in ("components", "components-more"):
                return f'<a href="#/components">{label}</a>'
            return f'<span class="doc-ref" title="{html.escape(target)}">{label}</span>'
        p = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", link, p)
        p = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", p)
        p = re.sub(r"(?<![\w*])\*([^*\s][^*]*)\*(?![\w*])", r"<em>\1</em>", p)
        p = re.sub(r"(?<![\w])_([^_\s][^_]*)_(?![\w])", r"<em>\1</em>", p)
        out.append(p)
    return "".join(out)

# the guide: docs/*.md in reading order (components have their own pages)
GUIDES = ["getting-started", "layout", "theming", "icons", "custom-components", "integration", "rendering", "design"]
GUIDE_IDS = set(GUIDES)

IMG_RE = re.compile(r"images/(components|showcase)/([\w\-]+?)(?:-dark)?\.png")

def md_to_html(lines, anchors, scenes):
    """Returns (html, scene names in order, static images) for a section body."""
    out, found, statics = [], [], []
    i = 0
    para = []
    def flush():
        if para:
            out.append("<p>" + inline(" ".join(para), anchors) + "</p>")
            para.clear()
    while i < len(lines):
        ln = lines[i]
        s = ln.strip()
        if s.startswith("```"):
            flush()
            lang = s[3:].strip() or "text"
            body = []
            i += 1
            while i < len(lines) and not lines[i].strip().startswith("```"):
                body.append(lines[i]); i += 1
            out.append(f'<pre class="code" data-lang="{lang}"><code>{html.escape(chr(10).join(body))}</code></pre>')
            i += 1
            continue
        imgs = IMG_RE.findall(s) if ("<picture" in s or s.startswith("![") or "<img" in s) else []
        if imgs:
            flush()
            for kind, name in imgs:
                if name in scenes:
                    if name not in found: found.append(name)
                else:
                    alt = re.search(r'alt="([^"]*)"', s) or re.search(r"!\[([^\]]*)\]", s)
                    entry = {"src": f"images/{kind}/{name}", "alt": alt.group(1) if alt else name}
                    if entry not in statics: statics.append(entry)
            i += 1
            continue
        if s.startswith("|"):
            flush()
            rows = []
            while i < len(lines) and lines[i].strip().startswith("|"):
                rows.append([c.strip() for c in lines[i].strip().strip("|").split("|")]); i += 1
            head, body = rows[0], [r for r in rows[1:] if not all(re.fullmatch(r":?-+:?", c) for c in r)]
            t = "<table><thead><tr>" + "".join(f"<th>{inline(c, anchors)}</th>" for c in head) + "</tr></thead><tbody>"
            for r in body: t += "<tr>" + "".join(f"<td>{inline(c, anchors)}</td>" for c in r) + "</tr>"
            out.append(t + "</tbody></table>")
            continue
        m = re.match(r"^(\s*)([-*]|\d+\.)\s+(.*)", ln)
        if m and not s.startswith("**"):
            flush()
            ordered = m.group(2)[0].isdigit()
            items = []
            while i < len(lines):
                m = re.match(r"^(\s*)([-*]|\d+\.)\s+(.*)", lines[i])
                if m: items.append(m.group(3)); i += 1
                elif lines[i].startswith("  ") and lines[i].strip() and items: items[-1] += " " + lines[i].strip(); i += 1
                else: break
            tag = "ol" if ordered else "ul"
            out.append(f"<{tag}>" + "".join(f"<li>{inline(it, anchors)}</li>" for it in items) + f"</{tag}>")
            continue
        h = re.match(r"^(#{3,6})\s+(.*)", s)
        if h:
            flush()
            out.append(f"<h4>{inline(h.group(2), anchors)}</h4>")
            i += 1
            continue
        if not s or s == "---" or s.startswith("<"):
            flush(); i += 1; continue
        para.append(s)
        i += 1
    flush()
    return "\n".join(out), found, statics

# ---------------------------------------------------------------- sections
def sections(rel, level):
    """Yields (title, line, body_lines, group) for each heading of `level` (## or ###)."""
    lines = read(rel).split("\n")
    marker = "#" * level + " "
    group, cur = None, None
    res, groups = [], {}
    for n, ln in enumerate(lines, 1):
        if level == 3 and ln.startswith("## "):
            if cur: res.append(cur); cur = None
            group = ln[3:].strip()
            groups[group] = {"line": n, "body": []}
            continue
        if ln.startswith(marker):
            if cur: res.append(cur)
            cur = {"title": ln[len(marker):].strip(), "line": n, "body": [], "group": group, "file": f"docs/{rel.split('/')[-1]}"}
            continue
        if ln.startswith("# ") or (level == 2 and ln.startswith("## ")):
            continue
        if cur: cur["body"].append(ln)
        elif group: groups[group]["body"].append(ln)
    if cur: res.append(cur)
    return res, groups

CATEGORIES = [
    ("basics", "Basics"), ("actions", "Actions"), ("inputs", "Inputs"), ("selection", "Selection"),
    ("pickers", "Pickers"), ("navigation", "Navigation"), ("overlays", "Overlays"), ("feedback", "Feedback"),
    ("data", "Data display"), ("charts", "Charts"), ("planning", "Planning"), ("social", "Conversation and media"),
    ("motion", "Motion"), ("ai", "AI interfaces"),
]
MAIN_CAT = {
    "Text": "basics", "Badge, avatar and keycap": "basics", "Card and metric": "basics", "Image": "basics",
    "Button": "actions", "Icon button": "actions", "Button group, copy and hold to confirm": "actions",
    "Text field": "inputs", "Text area": "inputs", "Number field": "inputs", "Tag input": "inputs",
    "One time code": "inputs", "Password strength": "inputs",
    "Segmented control": "selection", "Checkbox, switch and radio": "selection", "Slider": "selection",
    "Chips and rating": "selection", "Radio cards": "selection",
    "Select and combobox": "pickers", "Date picker and calendar": "pickers", "Color picker": "pickers",
    "Tabs": "navigation", "Sidebar navigation": "navigation", "Breadcrumb, pagination and stepper": "navigation",
    "Dropdown and context menu": "overlays", "Popover": "overlays", "Tooltip": "overlays", "Dialog": "overlays",
    "Sheet": "overlays", "Command palette": "overlays",
    "Toast": "feedback", "Alert": "feedback", "Progress, meter, spinner and skeleton": "feedback", "Empty state": "feedback",
    "Table": "data", "List and tree": "data", "Accordion": "data", "Timeline and progress ring": "data",
    "Reorderable list": "data", "Charts": "charts",
}
MORE_CAT = {
    "Actions": "actions", "Inputs": "inputs", "Pickers": "pickers", "Capture": "inputs", "Disclosure": "overlays",
    "Feedback": "feedback", "Data": "data", "Code": "data", "Conversation": "social", "Media": "social",
    "Planning": "planning", "More charts": "charts", "Motion": "motion", "AI interfaces": "ai",
}
ITEM_CAT = {"Table of contents": "navigation"}
# sections whose docs have no screenshot: the scene that shows them
NO_IMAGE = {
    "Text": ["text"], "Icon button": ["icon-button"], "Color picker": ["color-picker"], "Popover": ["popover"],
    "Sheet": ["sheet"], "Image": ["image"], "Reorderable list": ["reorder"], "One time code": ["tag-otp"],
    "Password strength": ["tag-otp"], "Confirm button": ["confirm-button"], "Reactions": ["reactions"],
    "Speed dial": ["speed-dial"], "Theme switcher and toggle": ["theme-switcher"], "Swipe actions": ["swipe-actions"],
    "Money field": ["money-field"], "Prefix and suffix": ["prefix-suffix"], "Rolling numbers": ["rolling-numbers"],
}
EXTRA_FOR = {"Toast": ["toast-demo"], "Dialog": ["dialog-demo"]}
# live variants shown before the doc scene, when the doc scene only poses a moment
PREPEND = {"Kanban board": ["kanban-live"]}

SHOWCASE = [
    ("hero-data", "Data grid", "Virtualized rows, filters, sorting, resizable columns and spreadsheet style selection."),
    ("hero-board", "Kanban", "The lifted card follows the pointer and the column opens a slot for it."),
    ("hero-desk", "Dock, tiles and player", "Icons grow under the pointer, the waveform seeks on click."),
    ("hero-pick", "Pickers", "A date range with presets over two months, and wheels that spin with momentum."),
    ("hero-palette", "Command palette", "Fuzzy search, groups and shortcuts."),
    ("hero-charts", "Charts", "Calendar heatmap, sankey and radar, with a live task plan."),
    ("hero-social", "Conversation", "Grouped bubbles, threads, a rolling counter, presence."),
    ("hero-effects", "Spotlight cards", "The surface and edge nearest the pointer light up."),
    ("hero-ai", "Assistant", "Reasoning, tool calls, citations and a composer, beside a task plan and a frame time chart."),
    ("hero-plan", "Planning", "Week calendar and gantt chart together."),
    ("stage-mosaic", "Build settings", "Forms, a calendar with time slots, metrics, a bar chart, switches and chips on one page."),
    ("stage-mosaic-pro", "Activity", "A chat with tool calls, a contribution heatmap, a sankey, a task plan, control tiles and a radar chart."),
]
APPS = [
    ("gallery", "Gallery", "The native gallery app: every component on its own page, live.", "examples/gallery_ui.c"),
    ("dashboard", "Dashboard", "Larkspur Supply: KPI strip with rolling figures, charts and an orders table.", "examples/dashboard.c"),
    ("finance", "Finance", "Halden: accounts, budgets and transactions.", "examples/finance.c"),
    ("music", "Music", "Tempo: library, now playing and queue.", "examples/music.c"),
    ("mail", "Mail", "Folders, a message list and a reading pane in split views.", "examples/mail.c"),
    ("notes", "Notes", "A notes app with a list, an editor and export.", "examples/notes.c"),
    ("settings", "Settings", "Account, notifications, appearance and billing settings.", "examples/settings.c"),
    ("tracker", "Tracker", "Orbit: an issue tracker with boards and filters.", "examples/tracker.c"),
    ("assistant", "Assistant", "Muse: an AI assistant with reasoning, tools and attachments.", "examples/assistant.c"),
]

def main():
    table = scene_table()
    srcs = scene_sources(table)
    main_secs, _ = sections("docs/components.md", 2)
    more_secs, more_groups = sections("docs/components-more.md", 3)
    all_secs = main_secs + more_secs
    anchors, used = {}, set()
    for s in all_secs:
        base = slug(s["title"]); sid = base; k = 2
        while sid in used: sid = f"{base}-{k}"; k += 1
        used.add(sid); s["id"] = sid; anchors[slug(s["title"])] = sid
    for g in more_groups:
        first = next((s for s in more_secs if s["group"] == g), None)
        if first: anchors.setdefault(slug(g), first["id"])

    items, cats = {}, {c: [] for c, _ in CATEGORIES}
    group_scenes = {}
    for g, info in more_groups.items():
        _, found, _ = md_to_html(info["body"], anchors, srcs)
        group_scenes[g] = found
    for s in all_secs:
        body_html, found, statics = md_to_html(s["body"], anchors, srcs)
        scenes_for = found or NO_IMAGE.get(s["title"]) or (group_scenes.get(s["group"]) if s["group"] else None) or []
        scenes_for = [x for x in PREPEND.get(s["title"], []) + scenes_for + EXTRA_FOR.get(s["title"], []) if x in srcs]
        cat = ITEM_CAT.get(s["title"]) or (MORE_CAT.get(s["group"]) if s["group"] else MAIN_CAT.get(s["title"])) or "basics"
        summary = ""
        for para in re.findall(r"<p>(.*?)</p>", body_html, re.S):
            text = html.unescape(re.sub(r"<[^>]+>", "", para)).strip()
            # skip option lists, return values and key maps: they do not say what the thing is
            if para.lstrip().startswith("<code>") or re.match(r"(Options|Keyboard|Returns|Each returns|Pass|Use `|Set )", text):
                continue
            if len(text.split()) < 5: continue
            summary = re.split(r"(?<=[.!?])\s", text, maxsplit=1)[0][:200]
            break
        if not summary:
            first = re.search(r"<p>(.*?)</p>", body_html, re.S)
            if first:
                text = html.unescape(re.sub(r"<[^>]+>", "", first.group(1))).strip()
                summary = re.split(r"(?<=[.!?])\s", text, maxsplit=1)[0][:200]
        apis = sorted(set(re.findall(r"\bcui_[a-z0-9_]+(?=\()", html.unescape(body_html))))
        items[s["id"]] = {"id": s["id"], "title": s["title"], "category": cat, "summary": summary,
                          "doc": {"file": s["file"], "line": s["line"], "group": s["group"]},
                          "html": body_html, "scenes": scenes_for, "images": statics, "apis": apis[:40]}
        cats[cat].append(s["id"])

    # the icon set has its own doc page
    icon_lines = read("docs/icons.md").split("\n")
    body_html, _, statics = md_to_html(icon_lines[1:], anchors, srcs)
    items["icons"] = {"id": "icons", "title": "Icons", "category": "basics",
                      "summary": "The Lucide icon set embedded as a font, used by name through CUI_ICON_* constants.",
                      "doc": {"file": "docs/icons.md", "line": 1, "group": None}, "html": body_html,
                      "scenes": ["icons"], "images": statics, "apis": ["cui_icon"]}
    cats["basics"].insert(1, "icons")

    guides = []
    for gid in GUIDES:
        lines = read(f"docs/{gid}.md").split("\n")
        title = next((l[2:].strip() for l in lines if l.startswith("# ")), gid)
        secs, cur, intro = [], None, []
        for n, ln in enumerate(lines, 1):
            if ln.startswith("# "): continue
            if ln.startswith("## "):
                if cur: secs.append(cur)
                t = ln[3:].strip()
                cur = {"title": re.sub(r"^\d+\.\s*", "", t), "id": slug(re.sub(r"^\d+\.\s*", "", t)), "line": n, "body": []}
                continue
            (cur["body"] if cur else intro).append(ln)
        if cur: secs.append(cur)
        def figs(st): return "".join(f'<figure class="figure"><img data-src="{x["src"]}" alt="{html.escape(x["alt"])}"></figure>' for x in st)
        ih, ifound, ist = md_to_html(intro, anchors, {})
        out_secs = []
        for sec in secs:
            bh, _, st = md_to_html(sec["body"], anchors, {})
            out_secs.append({"title": sec["title"], "id": sec["id"], "line": sec["line"], "html": bh + figs(st)})
        lede = ""
        m = re.search(r"<p>(.*?)</p>", ih, re.S)
        if m: lede = html.unescape(re.sub(r"<[^>]+>", "", m.group(1))).strip()
        guides.append({"id": gid, "title": title, "file": f"docs/{gid}.md", "intro": ih + figs(ist), "lede": lede, "sections": out_secs})

    showcase = []
    for name, title, desc in SHOWCASE:
        if name in srcs: showcase.append({"id": name, "title": title, "summary": desc, "scenes": [name]})
    accents = [n for n in srcs if n.startswith("accent-")]
    covered = {n for it in items.values() for n in it["scenes"]} | {n for s in showcase for n in s["scenes"]} | set(accents)
    unlisted = [n for n in srcs if n not in covered]

    catalog = {
        "categories": [{"id": c, "title": t, "items": cats[c]} for c, t in CATEGORIES if cats[c]],
        "items": items,
        "showcase": showcase,
        "accents": accents,
        "apps": [{"id": a, "title": t, "summary": d, "source": f} for a, t, d, f in APPS],
        "scenes": srcs,
        "unlisted": unlisted,
        "guides": guides,
        # the doc images that exist, so thumbnails only ask for real files
        "shots": sorted(f"{d}/{n[:-4]}" for d in ("components", "showcase")
                        for n in os.listdir(os.path.join(ROOT, "docs", "images", d)) if n.endswith(".png")),
    }
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("// generated by tools/gen_catalog.py, do not edit\nwindow.CUI_CATALOG = ")
        json.dump(catalog, f, ensure_ascii=False, separators=(",", ":"))
        f.write(";\n")
    n_scn = sum(1 for it in items.values() if it["scenes"])
    print(f"  catalog: {len(items)} components ({n_scn} with live scenes), {len(showcase)} showcase, {len(APPS)} apps, "
          f"{len(srcs)} scenes; unlisted scenes: {', '.join(unlisted) or 'none'}")
    missing = [it["title"] for it in items.values() if not it["scenes"]]
    if missing: print("  no live scene:", ", ".join(missing))

main()
