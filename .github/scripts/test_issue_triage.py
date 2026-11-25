"""Tests for issue_triage.py with a fake GitHub. Run: python -m unittest discover -s .github/scripts"""
import unittest
from datetime import datetime, timezone

import issue_triage as it


def body(desc, mod="0\\.8\\.0 \\(1f3ea3af\\) \\#\u200bcdc840cc0426", game="a54b1dad09712000", ue4ss="3\\.0\\.1\\.0", mods="HSEnhancer"):
    return (f"**Filed from the game** (report `abc`)\n\n> {desc}\n\n| | |\n|---|---|\n| Mod build | {mod} |\n"
            f"| Game build | {game} |\n| UE4SS | {ue4ss} |\n| UE4SS mods | {mods} |\n\n2 log file(s) stored privately.\n")


def crash(module, offset="B0D8A2", op="reading"):
    m = it.re.sub(r"([.\-+:()])", r"\\\1", f"{module}+0x{offset}")
    return f"Crash \\(sent automatically\\)\\: crash\\: fault 0xC0000005 at {m} {op} 0x10 \\("


def issue(n, desc, day="2026-11-24", labels=("in-game-report",), state="open", updated=None, **kw):
    return {"number": n, "title": "[in-game] x", "body": body(desc, **kw), "state": state, "labels": [{"name": l} for l in labels],
            "created_at": day + "T10:00:00Z", "updated_at": (updated or day) + "T10:00:00Z"}


class FakeGitHub:
    def __init__(self, open_issues, trackers=(), release=("v0.8.0", "2026-11-24T07:00:00Z")):
        self._open, self._trackers, self._release = list(open_issues), list(trackers), release
        self.log = []

    def issues(self, state, labels):
        return self._trackers if labels == it.TRACKER_LABEL else self._open

    def latest_release(self):
        return {"tag_name": self._release[0], "published_at": self._release[1]} if self._release else None

    def comment(self, number, text):
        self.log.append((number, "comment", text))

    def edit(self, number, what, **fields):
        self.log.append((number, "edit", fields))

    def add_labels(self, number, labels):
        self.log.append((number, "labels", labels))

    def did(self, number, kind):
        return [x[2] for x in self.log if x[0] == number and x[1] == kind]


def run(gh, now="2026-11-25", sweep=False):
    t = it.Triage(gh, datetime.fromisoformat(now + "T12:00:00+00:00"))
    t.load()
    t.sort_crashes()
    t.written_duplicates()
    if sweep:
        t.sweep()
    t.save()
    return t


class ParseTest(unittest.TestCase):
    def test_crash_and_facts(self):
        b = body(crash("HalfSwordUE5-Win64-Shipping.exe", "141C6F6"))
        f = it.parse_crash(b)
        self.assertEqual((f["code"], f["module"], f["offset"], f["op"]), ("0xC0000005", "HalfSwordUE5-Win64-Shipping.exe", "141C6F6", "reading"))
        self.assertEqual(it.facts(b)["Mod build"], "0.8.0 (1f3ea3af) #cdc840cc0426")
        self.assertIsNone(it.parse_crash(body("my sword vanished")))

    def test_categories(self):
        self.assertEqual(it.category("HalfSwordUE5-Win64-Shipping.exe"), "crash: game")
        self.assertEqual(it.category("UE4SS.dll"), "crash: UE4SS")
        self.assertEqual(it.category("dwmapi.dll"), "crash: HSMMod")
        self.assertEqual(it.category("?"), "crash: HSMMod")
        for m in ("ntdll.dll", "KERNEL32.DLL", "nvwgf2umx.dll", "VCRUNTIME140.dll", "d3d12core.dll", "sl.dlss_g.dll", "tier0_s64.dll"):
            self.assertEqual(it.category(m), "crash: system/driver", m)
        for m in ("HSEnhancer.dll", "Trainer_49118_f360675f80.dll"):
            self.assertEqual(it.category(m), "crash: other mod", m)

    def test_fingerprint_follows_the_build(self):
        f = it.facts(body("x"))
        fault = lambda m, o="1": {"code": "0xC0000005", "module": m, "offset": o, "op": "reading"}
        self.assertEqual(it.fingerprint(fault("dwmapi.dll"), f), "dwmapi.dll|0.8.0 (1f3ea3af)")
        self.assertEqual(it.fingerprint(fault("HalfSwordUE5-Win64-Shipping.exe"), f), "halfswordue5-win64-shipping.exe|a54b1dad09712000")
        self.assertEqual(it.fingerprint(fault("UE4SS.dll"), f), "ue4ss.dll|3.0.1.0")
        self.assertEqual(it.fingerprint(fault("ntdll.dll", "1"), f), it.fingerprint(fault("ntdll.dll", "2"), f))
        self.assertEqual(it.fingerprint(fault("?", "7FF70B880FFF"), f), "?")

    def test_versions(self):
        self.assertLess(it.version("0.8.0-pre56 (x)"), it.version("0.8.0-beta1"))
        self.assertLess(it.version("0.8.0-beta1"), it.version("v0.8.0"))
        self.assertLess(it.version("v0.8.0"), it.version("0.8.1-beta1"))
        self.assertIsNone(it.version("unknown"))


class TriageTest(unittest.TestCase):
    def test_first_report_becomes_the_tracker_and_the_rest_close_into_it(self):
        gh = FakeGitHub([issue(10, crash("UE4SS.dll")), issue(11, crash("UE4SS.dll", "C80999")), issue(12, crash("ntdll.dll"))])
        run(gh)
        self.assertIn(["crash: UE4SS", it.TRACKER_LABEL], gh.did(10, "labels"))
        self.assertTrue(any(f.get("title") == "[crash] In UE4SS 3.0.1.0" for f in gh.did(10, "edit")))
        self.assertIn({"state": "closed", "state_reason": "duplicate"}, gh.did(11, "edit"))
        self.assertIn("#10", gh.did(11, "comment")[0])
        self.assertIn(["crash: system/driver", it.TRACKER_LABEL], gh.did(12, "labels"))
        summary = [f["body"] for f in gh.did(10, "edit") if "body" in f][0]
        self.assertIn("| Reports | 2 |", summary)
        self.assertIn("access violation reading at UE4SS.dll+0xC80999", summary)
        self.assertEqual(it.read_state(summary)["count"], 2)

    def test_a_tracker_is_found_again_and_not_counted_twice(self):
        gh = FakeGitHub([issue(10, crash("UE4SS.dll"))])
        run(gh)
        b = [f["body"] for f in gh.did(10, "edit") if "body" in f][0]
        tracker = issue(10, "x", labels=("in-game-report", it.TRACKER_LABEL))
        tracker["body"] = b
        gh2 = FakeGitHub([tracker, issue(13, crash("UE4SS.dll", "1"))], trackers=[tracker])
        run(gh2)
        b2 = [f["body"] for f in gh2.did(10, "edit") if "body" in f][0]
        self.assertEqual(it.read_state(b2)["count"], 2)
        self.assertEqual(b2.count("crash-triage:begin"), 1)
        self.assertNotIn(10, [x[0] for x in gh2.log if x[1] == "comment"])

    def test_written_duplicates_and_other_reports(self):
        gh = FakeGitHub([issue(20, "R menu stays!"), issue(21, "r menu   stays"), issue(22, "bugged into door")])
        run(gh)
        self.assertIn({"state": "closed", "state_reason": "duplicate"}, gh.did(21, "edit"))
        self.assertEqual(gh.did(20, "edit") + gh.did(22, "edit"), [])

    def test_sweep_closes_stale_and_old_build_trackers_and_quiet_reports(self):
        gh = FakeGitHub([issue(30, crash("dwmapi.dll"), day="2026-11-01", mod="0\\.8\\.0\\-pre53 \\(x\\)"),
                         issue(31, crash("dwmapi.dll"), day="2026-11-26", mod="0\\.8\\.0\\-pre54 \\(y\\)"),
                         issue(32, crash("dwmapi.dll"), day="2026-11-26"),
                         issue(33, "lost my sword", updated="2026-11-01"), issue(34, "lag", updated="2026-11-20")])
        run(gh, now="2026-11-28", sweep=True)
        self.assertIn({"state": "closed", "state_reason": "not_planned"}, gh.did(30, "edit"))   # stale
        self.assertIn({"state": "closed", "state_reason": "not_planned"}, gh.did(31, "edit"))   # only pre54 < v0.8.0
        self.assertNotIn({"state": "closed", "state_reason": "not_planned"}, gh.did(32, "edit"))
        self.assertIn({"state": "closed", "state_reason": "not_planned"}, gh.did(33, "edit"))
        self.assertEqual(gh.did(34, "edit"), [])

    def test_no_old_build_close_inside_the_release_grace(self):
        gh = FakeGitHub([issue(31, crash("dwmapi.dll"), day="2026-11-25", mod="0\\.8\\.0\\-pre54 \\(y\\)")])
        run(gh, now="2026-11-25", sweep=True)
        self.assertNotIn({"state": "closed", "state_reason": "not_planned"}, gh.did(31, "edit"))

    def test_a_bot_closed_tracker_reopens_on_a_new_report(self):
        gh = FakeGitHub([issue(40, crash("ntdll.dll"), day="2026-11-01")])
        run(gh, now="2026-11-20", sweep=True)
        tracker = issue(40, "x", state="closed", labels=("in-game-report", it.TRACKER_LABEL))
        tracker["body"] = [f["body"] for f in gh.did(40, "edit") if "body" in f][0]
        self.assertEqual(it.read_state(tracker["body"])["closed_by"], "stale")
        gh2 = FakeGitHub([issue(41, crash("ntdll.dll"), day="2026-11-21")], trackers=[tracker])
        run(gh2, now="2026-11-21")
        self.assertIn({"state": "open"}, gh2.did(40, "edit"))
        self.assertIsNone(it.read_state([f["body"] for f in gh2.did(40, "edit") if "body" in f][0])["closed_by"])

    def test_a_tracker_closed_by_hand_stays_closed(self):
        tracker = issue(50, "x", state="closed", labels=("in-game-report", it.TRACKER_LABEL))
        state = it.new_state("ntdll.dll|", "crash: system/driver")
        it.add_report(state, issue(50, crash("ntdll.dll")))
        tracker["body"] = it.with_summary(tracker["body"], state, 50)
        gh = FakeGitHub([issue(51, crash("ntdll.dll"))], trackers=[tracker])
        run(gh)
        self.assertNotIn({"state": "open"}, gh.did(50, "edit"))
        self.assertIn({"state": "closed", "state_reason": "duplicate"}, gh.did(51, "edit"))


if __name__ == "__main__":
    unittest.main()
