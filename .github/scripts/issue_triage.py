#!/usr/bin/env python3
"""Sorts the in-game reports the report relay files (label in-game-report). Python 3.9+, standard library only.

Every run (an issue was opened, the 6-hourly schedule, or by hand):
  - A crash report (the text the mod sends by itself after a crash) gets a crash: label for the module that crashed.
    The first report of a crash becomes its tracker (label crash-tracker, a readable title and a summary of every
    report); a later report of the same crash is added to the tracker and closed as its duplicate.
  - A written report with the same text as an older open one is closed as its duplicate.
With --sweep (the schedule and a run by hand) as well:
  - A tracker with no report for STALE_DAYS, or reported only on mod builds older than the latest release once that
    release is RELEASE_GRACE_DAYS old, is closed. A new report reopens it (an old-build tracker only for a current build).
  - A written in-game report with no activity for STALE_DAYS is closed.
Issues filed any other way (Discord, by hand) are never touched.

Environment: GITHUB_TOKEN, GITHUB_REPOSITORY (owner/name). --dry-run prints what would change and writes nothing.
Tests: python -m unittest discover -s .github/scripts
"""
import argparse
import collections
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timedelta, timezone

REPORT_LABEL = "in-game-report"
TRACKER_LABEL = "crash-tracker"
STALE_DAYS = 14
RELEASE_GRACE_DAYS = 3
KEEP_NUMBERS = 40                         # report numbers listed on a tracker; the counts cover every report
CRASH_PREFIX = "Crash (sent automatically): "
CRASH_RE = re.compile(r"crash: fault (?P<code>0x[0-9A-Fa-f]+) at (?P<module>.+?)\+0x(?P<offset>[0-9A-Fa-f]+)"
                      r"(?: (?P<op>reading|writing|executing))?")
STATE_RE = re.compile(r"<!-- crash-triage-state (\{.*?\}) -->", re.S)
SUMMARY_RE = re.compile(r"\n*<!-- crash-triage:begin -->.*<!-- crash-triage:end -->", re.S)
FACT_RE = re.compile(r"^\| (?P<key>[^|]+?) \| (?P<value>.*?) \|$", re.M)
VERSION_RE = re.compile(r"(\d+)\.(\d+)\.(\d+)(?:[-_](pre|beta)(\d+))?")
EXCEPTIONS = {"0xc0000005": "access violation", "0xc0000006": "in-page error", "0xc000001d": "illegal instruction",
              "0xc0000094": "divide by zero", "0xc00000fd": "stack overflow", "0xc0000374": "heap corruption",
              "0xc0000409": "stack buffer overrun"}
MOD_MODULE = "dwmapi.dll"
# Windows, the C runtime, graphics drivers and upscalers, Steam
SYSTEM_RE = re.compile(r"^(ntdll|kernel32|kernelbase|user32|win32u|gdi32|gdi32full|combase|ole32|rpcrt4|ucrtbase|msvcp\d*|vcruntime\d*"
                       r"|d3d\d+(core)?|dxgi|dxcore|nv\w*|amd\w*|atiu\w*|atidx\w*|amdxx\w*|igd\w*|igc\w*|intel\w*|sl\.\w+"
                       r"|crashhandler\d*|tier0_s\d*|steam\w*|gameoverlayrenderer\d*|xinput\w*|dinput\w*|audioses|xaudio\w*)\.dll$")


# ---------------------------------------------------------------- reading a report

def unescape(s):
    """The relay's md(): specials backslash-escaped, @, # and / broken with a zero-width space, HTML entities."""
    s = re.sub(r"\\(.)", r"\1", s.replace("​", ""))
    return s.replace("&lt;", "<").replace("&gt;", ">").replace("&amp;", "&")


def facts(body):
    return {m["key"]: unescape(m["value"]) for m in FACT_RE.finditer(body or "")}


def description(body):
    m = re.search(r"^> (.*)$", body or "", re.M)
    return unescape(m.group(1)).strip() if m else ""


def parse_crash(body):
    d = description(body)
    if not d.startswith(CRASH_PREFIX):
        return None
    m = CRASH_RE.match(d, len(CRASH_PREFIX))
    return m.groupdict() if m else None


def category(module):
    m = module.lower()
    if m.endswith("-shipping.exe"):
        return "crash: game"
    if m == "ue4ss.dll":
        return "crash: UE4SS"
    if m in (MOD_MODULE, "?"):
        return "crash: HSMMod"
    if SYSTEM_RE.match(m):
        return "crash: system/driver"
    return "crash: other mod"


def build_of(module, f):
    """The build an offset in `module` belongs to: dwmapi.dll changes with every mod build, the game's exe with every game
    build, UE4SS with its version. Windows, drivers and other mods: none known."""
    module = module.lower()
    return (f.get("Mod build", "").split(" #")[0] if module == MOD_MODULE else f.get("Game build", "") if module.endswith("-shipping.exe")
            else f.get("UE4SS", "") if module == "ue4ss.dll" else "")


def fingerprint(fault, f):
    """One tracker per module and build. Exact offsets hardly ever repeat (most groups would hold one report), so they are
    counted inside the tracker instead. "?" is an address outside any module: one tracker for all of those."""
    module = fault["module"].lower()
    return "?" if module == "?" else f"{module}|{build_of(module, f)}"


def where(fault):
    kind = EXCEPTIONS.get(fault["code"].lower(), fault["code"])
    if fault["op"]:
        kind += " " + fault["op"]
    place = "an address outside any module" if fault["module"] == "?" else f"{fault['module']}+0x{fault['offset'].upper()}"
    return f"{kind} at {place}"


def version(text):
    """A sortable mod version: 0.8.0-pre5 < 0.8.0-beta1 < 0.8.0. None if `text` names none."""
    m = VERSION_RE.search(text or "")
    if not m:
        return None
    stage = {"pre": 0, "beta": 1, None: 2}[m.group(4)]
    return (int(m.group(1)), int(m.group(2)), int(m.group(3)), stage, int(m.group(5) or 0))


def version_name(text):
    m = VERSION_RE.search(text or "")
    return m.group(0) if m else "unknown"


def tracker_title(fault, f):
    module = fault["module"]
    if module == "?":
        return "[crash] Outside any module"
    build = build_of(module, f)
    if module.lower().endswith("-shipping.exe"):
        return f"[crash] In the game (game build {build[:8] or 'unknown'})"
    if module.lower() == MOD_MODULE:
        return f"[crash] In HSMMod {build or '(unknown build)'}"
    if module.lower() == "ue4ss.dll":
        return f"[crash] In UE4SS {build or '(unknown version)'}"
    return f"[crash] In {module}"


# ---------------------------------------------------------------- tracker state

def new_state(fp, cat):
    return {"fp": fp, "category": cat, "count": 0, "first": None, "last": None, "max_version": None,
            "numbers": [], "mod_builds": {}, "ue4ss_mods": {}, "faults": {}, "closed_by": None}


def read_state(body):
    m = STATE_RE.search(body or "")
    if not m:
        return None
    try:
        return json.loads(m.group(1))
    except ValueError:
        return None


def add_report(state, issue):
    if issue["number"] in state["numbers"]:
        return False
    f = facts(issue["body"])
    day = issue["created_at"][:10]
    state["count"] += 1
    state["first"] = min(filter(None, [state["first"], day]))
    state["last"] = max(filter(None, [state["last"], day]))
    state["numbers"] = sorted(state["numbers"] + [issue["number"]])[-KEEP_NUMBERS:]
    build = version_name(f.get("Mod build"))
    state["mod_builds"][build] = state["mod_builds"].get(build, 0) + 1
    v = version(f.get("Mod build"))
    if v and (state["max_version"] is None or v > tuple(state["max_version"])):
        state["max_version"] = list(v)
    fault = parse_crash(issue["body"])
    if fault:
        faults = state.setdefault("faults", {})
        faults[where(fault)] = faults.get(where(fault), 0) + 1
    for mod in filter(None, (m.strip() for m in f.get("UE4SS mods", "").split(","))):
        if mod != "-":
            state["ue4ss_mods"][mod] = state["ue4ss_mods"].get(mod, 0) + 1
    return True


def top(counts, n=8):
    items = sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))
    text = ", ".join(f"{k} ({v})" for k, v in items[:n])
    return text + (f", and {len(items) - n} more" if len(items) > n else "") if items else "-"


def fault_table(state, n=15):
    items = sorted(state.get("faults", {}).items(), key=lambda kv: (-kv[1], kv[0]))
    if not items:
        return ""
    rows = "".join(f"| {v} | {k} |\n" for k, v in items[:n])
    more = f"\nand {len(items) - n} more.\n" if len(items) > n else ""
    return f"| Reports | Fault |\n|---|---|\n{rows}{more}\n"


def summary(state, tracker_number):
    others = [n for n in state["numbers"] if n != tracker_number]
    listed = ", ".join(f"#{n}" for n in others) or "-"
    return (f"<!-- crash-triage:begin -->\n\n---\n\n**Crash tracker.** Every automatic crash report from the same place and "
            f"build is closed as a duplicate of this issue and counted here.\n\n| | |\n|---|---|\n| Reports | {state['count']} |\n"
            f"| First seen | {state['first']} |\n| Last seen | {state['last']} |\n| Mod builds | {top(state['mod_builds'])} |\n"
            f"| UE4SS mods | {top(state['ue4ss_mods'])} |\n\n{fault_table(state)}Latest reports: {listed}\n\n"
            f"<!-- crash-triage-state {json.dumps(state, sort_keys=True)} -->\n<!-- crash-triage:end -->")


def with_summary(body, state, number):
    return SUMMARY_RE.sub("", body or "").rstrip() + "\n\n" + summary(state, number)


# ---------------------------------------------------------------- GitHub

class GitHub:
    def __init__(self, token, repo, dry_run=False, pause=1.0, api="https://api.github.com"):
        self.token, self.repo, self.dry_run, self.pause, self.api = token, repo, dry_run, pause, api

    def _call(self, method, path, body=None):
        url = path if path.startswith("http") else f"{self.api}/repos/{self.repo}{path}"
        req = urllib.request.Request(url, method=method, data=None if body is None else json.dumps(body).encode(), headers={
            "Authorization": "Bearer " + self.token, "Accept": "application/vnd.github+json",
            "X-GitHub-Api-Version": "2022-11-28", "User-Agent": "hsmmod-issue-triage", "Content-Type": "application/json"})
        for attempt in range(4):
            try:
                with urllib.request.urlopen(req, timeout=60) as r:
                    return json.loads(r.read() or b"null"), r.headers.get("Link", "")
            except urllib.error.HTTPError as e:
                # secondary rate limit (403/429) or a GitHub hiccup: wait and try again
                if e.code not in (403, 429, 500, 502, 503) or attempt == 3:
                    raise
                time.sleep(int(e.headers.get("Retry-After") or 30 * (attempt + 1)))

    def pages(self, path):
        url = f"{self.api}/repos/{self.repo}{path}"
        while url:
            data, link = self._call("GET", url)
            yield from data
            m = re.search(r'<([^>]+)>; rel="next"', link)
            url = m.group(1) if m else None

    def issues(self, **query):
        q = "&".join(f"{k}={v}" for k, v in {"per_page": 100, **query}.items())
        return [i for i in self.pages(f"/issues?{q}") if "pull_request" not in i]

    def latest_release(self):
        try:
            data, _ = self._call("GET", "/releases/latest")
            return data
        except urllib.error.HTTPError as e:
            if e.code == 404:
                return None
            raise

    def write(self, what, method, path, body=None):
        print(("would " if self.dry_run else "") + what)
        if self.dry_run:
            return None
        data, _ = self._call(method, path, body)
        time.sleep(self.pause)   # content-creating calls are rate limited per minute
        return data

    def comment(self, number, text):
        self.write(f"#{number}: comment: {text.splitlines()[0][:100]}", "POST", f"/issues/{number}/comments", {"body": text})

    def edit(self, number, what, **fields):
        self.write(f"#{number}: {what}", "PATCH", f"/issues/{number}", fields)

    def add_labels(self, number, labels):
        self.write(f"#{number}: label {', '.join(labels)}", "POST", f"/issues/{number}/labels", {"labels": labels})

    def ensure_label(self, name, color, text):
        try:
            self._call("GET", f"/labels/{urllib.request.quote(name)}")
        except urllib.error.HTTPError as e:
            if e.code != 404:
                raise
            self.write(f"create label {name}", "POST", "/labels", {"name": name, "color": color, "description": text})


# ---------------------------------------------------------------- the passes

def labels_of(issue):
    return {l["name"] for l in issue.get("labels", [])}


def days_since(stamp, now):
    return (now - datetime.fromisoformat(stamp.replace("Z", "+00:00"))).days


class Triage:
    def __init__(self, gh, now=None):
        self.gh = gh
        self.now = now or datetime.now(timezone.utc)
        self.trackers = {}   # fingerprint -> {"issue", "state", "dirty"}
        self.release = None

    def load(self):
        for issue in self.gh.issues(state="all", labels=TRACKER_LABEL):
            state = read_state(issue["body"])
            if state and state["fp"] not in self.trackers:
                self.trackers[state["fp"]] = {"issue": issue, "state": state, "dirty": False}
        self.open = sorted(self.gh.issues(state="open", labels=REPORT_LABEL), key=lambda i: i["number"])
        rel = self.gh.latest_release()
        if rel:
            self.release = {"tag": rel["tag_name"], "version": version(rel["tag_name"]), "published": rel["published_at"]}

    def current(self, v):
        """Whether a report's build is the latest release or newer (no release known: every build is current)."""
        return not self.release or not self.release["version"] or (v is not None and tuple(v) >= self.release["version"])

    def sort_crashes(self):
        for issue in self.open:
            if TRACKER_LABEL in labels_of(issue):
                continue
            fault = parse_crash(issue["body"])
            if not fault:
                continue
            f = facts(issue["body"])
            fp, cat = fingerprint(fault, f), category(fault["module"])
            t = self.trackers.get(fp)
            if t is None:
                state = new_state(fp, cat)
                add_report(state, issue)
                self.trackers[fp] = {"issue": issue, "state": state, "dirty": True}
                self.gh.add_labels(issue["number"], [cat, TRACKER_LABEL])
                self.gh.edit(issue["number"], "becomes the tracker: " + tracker_title(fault, f), title=tracker_title(fault, f))
                continue
            tracker, state = t["issue"], t["state"]
            add_report(state, issue)
            t["dirty"] = True
            self.gh.add_labels(issue["number"], [cat])
            self.gh.comment(issue["number"], f"Thanks for the report. Crashes like this one are tracked in #{tracker['number']} "
                                             f"({state['count']} reports so far), so this one is closed as its duplicate; "
                                             f"its details are counted there.")
            self.gh.edit(issue["number"], f"close as duplicate of #{tracker['number']}", state="closed", state_reason="duplicate")
            if tracker["state"] == "closed" and state.get("closed_by"):
                v = version(f.get("Mod build"))
                if state["closed_by"] != "old-build" or self.current(v):
                    state["closed_by"] = None
                    tracker["state"] = "open"
                    self.gh.edit(tracker["number"], "reopen: reported again", state="open")
                    self.gh.comment(tracker["number"], f"Crashed here again on {version_name(f.get('Mod build'))} (#{issue['number']}), "
                                                       f"so this is open again.")

    def written_duplicates(self):
        seen = {}
        for issue in self.open:
            if issue["state"] != "open" or TRACKER_LABEL in labels_of(issue) or parse_crash(issue["body"]):
                continue
            d = " ".join(re.sub(r"[^\w ]", " ", description(issue["body"]).lower()).split())
            if len(d) < 4:
                continue
            if d in seen:
                self.gh.comment(issue["number"], f"Thanks for the report. It reads the same as #{seen[d]}, so it is closed as its "
                                                 f"duplicate. Please add anything new there.")
                self.gh.edit(issue["number"], f"close as duplicate of #{seen[d]}", state="closed", state_reason="duplicate")
                issue["state"] = "closed"
            else:
                seen[d] = issue["number"]

    def sweep(self):
        grace = self.release and self.release["version"] and days_since(self.release["published"], self.now) >= RELEASE_GRACE_DAYS
        for t in self.trackers.values():
            issue, state = t["issue"], t["state"]
            if issue["state"] != "open" or not state["last"]:
                continue
            if days_since(state["last"] + "T00:00:00Z", self.now) >= STALE_DAYS:
                why, reason = "stale", f"No new crash report here for {STALE_DAYS} days, so this is closed."
            elif grace and not self.current(state["max_version"]):
                why, reason = "old-build", (f"These crashes have only been reported on builds older than {self.release['tag']}, so it "
                                            f"is closed.")
            else:
                continue
            state["closed_by"] = why
            t["dirty"] = True
            issue["state"] = "closed"
            self.gh.comment(issue["number"], reason + " It opens again by itself if a new crash report lands here.")
            self.gh.edit(issue["number"], f"close ({why})", state="closed", state_reason="not_planned")
        for issue in self.open:
            if (issue["state"] != "open" or TRACKER_LABEL in labels_of(issue) or parse_crash(issue["body"])
                    or days_since(issue["updated_at"], self.now) < STALE_DAYS):
                continue
            issue["state"] = "closed"
            self.gh.comment(issue["number"], f"Closing this report after {STALE_DAYS} days with no activity. If it still happens "
                                             f"on the latest version, please report it again from the game or reopen this issue.")
            self.gh.edit(issue["number"], "close (no activity)", state="closed", state_reason="not_planned")

    def save(self):
        for t in self.trackers.values():
            if t["dirty"]:
                issue = t["issue"]
                self.gh.edit(issue["number"], f"tracker summary: {t['state']['count']} reports",
                             body=with_summary(issue["body"], t["state"], issue["number"]))


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--sweep", action="store_true", help="also close stale trackers and reports")
    p.add_argument("--dry-run", action="store_true", help="print the changes, write nothing")
    a = p.parse_args(argv)
    token, repo = os.environ.get("GITHUB_TOKEN"), os.environ.get("GITHUB_REPOSITORY")
    if not token or not repo:
        sys.exit("GITHUB_TOKEN and GITHUB_REPOSITORY must be set")
    gh = GitHub(token, repo, a.dry_run)
    gh.ensure_label(TRACKER_LABEL, "5319e7", "One open issue per distinct crash; its duplicates are counted on it")
    tr = Triage(gh)
    tr.load()
    tr.sort_crashes()
    tr.written_duplicates()
    if a.sweep:
        tr.sweep()
    tr.save()


if __name__ == "__main__":
    main()
