#!/usr/bin/env python3
"""Self-test for the ecall-mcp server.

Exercises every tool against the synthetic data and asserts the data contract
described in the task. Run:  python selftest.py

Exits 0 on success, 1 on failure. No MCP transport or network needed — the
tool callables are invoked directly.
"""

from __future__ import annotations

import sys
import traceback

import server


def _call(tool):
    """Return the underlying Python function for a FastMCP tool or plain callable."""
    return getattr(tool, "fn", tool)


def check(cond: bool, msg: str) -> None:
    if not cond:
        raise AssertionError(msg)


def test_signals() -> None:
    data = _call(server.get_signals)()
    signals = {s["id"]: s for s in data["signals"]}
    check("PO-2026-041" in signals, "missing cluster PO-2026-041")
    po = signals["PO-2026-041"]
    check(po["source"] == "public_opinion_agent", "cluster wrong source")
    check(po["classification"] == "safety_critical", "cluster not safety_critical")
    check(po["trend"] == "rising", "cluster not rising")
    check(po["post_count"] == 37, "cluster post_count != 37")
    check(po["window_weeks"] == 6, "cluster window != 6 weeks")
    check(len(po["sample_posts"]) == 4, "cluster != 4 sample posts")
    check(
        po["theme"] == "eCall connects but operator has no location",
        "cluster theme mismatch",
    )

    jira = next((s for s in data["signals"] if s["source"] == "jira"), None)
    check(jira is not None, "missing Jira defect")
    check(
        jira["summary"] == "eCall received without coordinates",
        "jira summary mismatch",
    )

    cr = next((s for s in data["signals"] if s["type"] == "enhancement"), None)
    check(cr is not None, "missing change request")
    check(cr["summary"] == "add vehicle color to MSD", "CR summary mismatch")
    check(cr["priority"] == "low", "CR not low priority")
    print("  [ok] get_signals: cluster + jira + CR present and well-formed")


def test_events() -> None:
    q = _call(server.query_ecall_events)
    all_events = q()["events"]
    check(len(all_events) == 40, f"expected 40 events, got {len(all_events)}")

    missing = q(location_missing=True)["events"]
    present = q(location_missing=False)["events"]
    check(len(missing) + len(present) == 40, "missing + present != 40")
    check(7 <= len(missing) <= 9, f"expected ~8 missing, got {len(missing)}")

    for e in missing:
        check(e["impact_severity"] == "severe", f"{e['event_id']} missing but not severe")
        check(
            e["crash_type"] in {"rollover", "tunnel_exit"},
            f"{e['event_id']} crash_type not rollover/tunnel_exit",
        )
        check(
            e["location_api_response"] == "NO_FIX",
            f"{e['event_id']} location_api_response != NO_FIX",
        )
        check(
            e["backend_matches_vehicle"],
            f"{e['event_id']} backend payload != vehicle payload",
        )
    print(f"  [ok] query_ecall_events: 40 events, {len(missing)} missing all severe/NO_FIX")


def test_location_sources() -> None:
    q = _call(server.query_ecall_events)
    gls = _call(server.get_location_sources)
    missing = q(location_missing=True)["events"]
    for e in missing:
        eid = e["event_id"]
        src = gls(eid)
        check(src["found"], f"no location sources for {eid}")
        fix = src["last_valid_fix"]
        age = fix["age_before_impact_s"]
        check(5 <= age <= 60, f"{eid} last fix age {age}s not in 5-60s")
        check("lat" in fix and "lon" in fix, f"{eid} last fix missing coords")
        crumbs = src["breadcrumbs"]
        check(3 <= len(crumbs) <= 5, f"{eid} has {len(crumbs)} breadcrumbs, want 3-5")
        for c in crumbs:
            check(
                "timestamp_utc" in c and "lat" in c and "lon" in c,
                f"{eid} breadcrumb missing fields",
            )

    # Non-missing event should have no sources.
    present = q(location_missing=False)["events"]
    if present:
        none_src = gls(present[0]["event_id"])
        check(not none_src["found"], "present event unexpectedly has location sources")
    print(f"  [ok] get_location_sources: all {len(missing)} missing events have fix + breadcrumbs")


def test_search_spec() -> None:
    ss = _call(server.search_spec)
    res = ss("GNSS")
    check(res.get("error") is None, f"search_spec error: {res.get('error')}")
    check(res["match_count"] > 0, "expected GNSS matches in spec")
    check(all("line" in m and "text" in m for m in res["matches"]), "bad match shape")

    empty = ss("zzz-no-such-token-zzz")
    check(empty["match_count"] == 0, "expected zero matches for junk keyword")
    print(f"  [ok] search_spec: {res['match_count']} 'GNSS' hits, 0 for junk")


def test_save_change_case() -> None:
    scc = _call(server.save_change_case)
    sample = "# Change Case (self-test)\n\nSynthetic self-test write.\n"
    res = scc(sample)
    check(res["saved"], "save_change_case did not report success")
    written = server.CHANGE_CASE_PATH.read_text(encoding="utf-8")
    check(written == sample, "written change case content mismatch")
    print(f"  [ok] save_change_case: wrote {res['bytes']} bytes to {res['path']}")


def main() -> int:
    tests = [
        ("signals", test_signals),
        ("events", test_events),
        ("location_sources", test_location_sources),
        ("search_spec", test_search_spec),
        ("save_change_case", test_save_change_case),
    ]
    failures = 0
    print("ecall-mcp self-test")
    for name, fn in tests:
        try:
            fn()
        except Exception as exc:  # noqa: BLE001 - report and continue
            failures += 1
            print(f"  [FAIL] {name}: {exc}")
            traceback.print_exc()
    if failures:
        print(f"\nFAILED: {failures} test group(s) failed.")
        return 1
    print("\nPASSED: all tool and data-contract checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
