"""ecall-mcp — FastMCP server exposing synthetic eCall triage data.

This server surfaces the data an engineer needs to investigate the
"eCall connects but operator has no location" problem and to draft a change
case against the TCU baseline spec. All data served here is synthetic.

Tools
-----
- get_signals()                      -> clustered public-opinion / Jira / CR signals
- query_ecall_events(location_missing)-> field events, optionally filtered
- get_location_sources(event_id)     -> last GNSS fix + breadcrumbs for a missing event
- search_spec(keyword)               -> matching lines from ../ecall-tcu/docs/ecall_spec.md
- save_change_case(markdown)         -> writes docs/change_case.md

Run:  python server.py   (stdio transport)
"""

from __future__ import annotations

import csv
import json
from pathlib import Path
from typing import Any, Optional

from fastmcp import FastMCP

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
HERE = Path(__file__).resolve().parent
DATA_DIR = HERE / "data"
DOCS_DIR = HERE / "docs"
SIGNALS_PATH = DATA_DIR / "signals.json"
EVENTS_PATH = DATA_DIR / "ecall_events.csv"
LOCATION_SOURCES_PATH = DATA_DIR / "location_sources.json"
# Spec lives in the sibling ecall-tcu project. In this workshop layout the
# shared copy actually sits under ../docs, so fall back to it if the primary
# location is absent.
_SPEC_CANDIDATES = (
    HERE.parent / "ecall-tcu" / "docs" / "ecall_spec.md",
    HERE.parent / "docs" / "ecall_spec.md",
)


def _resolve_spec_path() -> Path:
    for candidate in _SPEC_CANDIDATES:
        if candidate.exists():
            return candidate.resolve()
    # Default to the task-specified path for reporting when none exist.
    return _SPEC_CANDIDATES[0].resolve()


CHANGE_CASE_PATH = DOCS_DIR / "change_case.md"

mcp = FastMCP("ecall-mcp")

# ---------------------------------------------------------------------------
# Loaders
# ---------------------------------------------------------------------------
def _load_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as fh:
        return json.load(fh)


def _str_to_bool(value: str) -> bool:
    return str(value).strip().lower() in {"true", "1", "yes", "y"}


def _load_events() -> list[dict[str, Any]]:
    """Read the events CSV, coercing a few columns to their natural types."""
    events: list[dict[str, Any]] = []
    with EVENTS_PATH.open("r", encoding="utf-8", newline="") as fh:
        for row in csv.DictReader(fh):
            row["location_missing"] = _str_to_bool(row.get("location_missing", "false"))
            try:
                row["gnss_fix_age_s"] = int(row["gnss_fix_age_s"])
            except (KeyError, ValueError):
                pass
            row["backend_matches_vehicle"] = (
                row.get("backend_payload_hash") == row.get("vehicle_payload_hash")
            )
            events.append(row)
    return events


# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------
@mcp.tool
def get_signals() -> dict[str, Any]:
    """Return the clustered triage signals.

    Includes the rising, safety-critical public-opinion cluster
    (PO-2026-041, "eCall connects but operator has no location"), the related
    Jira defect ("eCall received without coordinates"), and an unrelated
    low-priority change request ("add vehicle color to MSD").
    """
    return _load_json(SIGNALS_PATH)


@mcp.tool
def query_ecall_events(location_missing: Optional[bool] = None) -> dict[str, Any]:
    """Query synthetic field eCall events.

    Args:
        location_missing: If True, return only events where the MSD position was
            missing (NOT_AVAILABLE). If False, return only events with a position.
            If omitted/None, return all events.

    Returns a summary count plus the matching event records.
    """
    events = _load_events()
    if location_missing is not None:
        events = [e for e in events if e["location_missing"] is bool(location_missing)]
    return {
        "count": len(events),
        "filter": {"location_missing": location_missing},
        "events": events,
    }


@mcp.tool
def get_location_sources(event_id: str) -> dict[str, Any]:
    """Return available location sources for a location-missing event.

    For a given event_id (e.g. "EV-2026-0003") this returns the last valid GNSS
    fix recorded shortly before impact (5-60 s prior) and the 3-5 breadcrumb
    fixes leading up to it. These show a recent last-known position existed that
    the live-GNSS-only baseline discards.

    Args:
        event_id: The event identifier to look up.
    """
    data = _load_json(LOCATION_SOURCES_PATH)
    sources = data.get("sources", {})
    entry = sources.get(event_id)
    if entry is None:
        return {
            "event_id": event_id,
            "found": False,
            "message": (
                "No location sources for this event_id. Location sources exist "
                "only for location_missing events."
            ),
            "available_event_ids": sorted(sources.keys()),
        }
    return {"event_id": event_id, "found": True, **entry}


@mcp.tool
def search_spec(keyword: str) -> dict[str, Any]:
    """Search the TCU baseline spec for a keyword (case-insensitive).

    Reads ../ecall-tcu/docs/ecall_spec.md and returns each matching line with
    its line number.

    Args:
        keyword: Substring to search for.
    """
    spec_path = _resolve_spec_path()
    if not spec_path.exists():
        return {
            "keyword": keyword,
            "spec_path": str(spec_path),
            "error": "spec file not found",
            "matches": [],
        }
    needle = keyword.lower()
    matches: list[dict[str, Any]] = []
    with spec_path.open("r", encoding="utf-8") as fh:
        for lineno, line in enumerate(fh, start=1):
            if needle in line.lower():
                matches.append({"line": lineno, "text": line.rstrip("\n")})
    return {
        "keyword": keyword,
        "spec_path": str(spec_path),
        "match_count": len(matches),
        "matches": matches,
    }


@mcp.tool
def save_change_case(markdown: str) -> dict[str, Any]:
    """Persist a drafted change case to docs/change_case.md.

    Args:
        markdown: The full change-case document, in Markdown.

    Returns the path written and the byte count.
    """
    DOCS_DIR.mkdir(parents=True, exist_ok=True)
    CHANGE_CASE_PATH.write_text(markdown, encoding="utf-8")
    return {
        "saved": True,
        "path": str(CHANGE_CASE_PATH),
        "bytes": len(markdown.encode("utf-8")),
    }


if __name__ == "__main__":
    mcp.run()
