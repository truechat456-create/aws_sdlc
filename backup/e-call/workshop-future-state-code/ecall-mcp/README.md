# ecall-mcp

A [FastMCP](https://gofastmcp.com) server that surfaces **synthetic** eCall
triage data for investigating the "eCall connects but operator has no location"
problem and drafting a change case against the TCU baseline spec.

All data in `data/` is synthetic.

## Layout

```
ecall-mcp/
├── server.py              # FastMCP server + tools
├── selftest.py            # direct-call self-test (no transport needed)
├── requirements.txt
├── data/
│   ├── signals.json       # public-opinion cluster + Jira defect + unrelated CR
│   ├── ecall_events.csv   # 40 field events (~8 location_missing)
│   └── location_sources.json  # last GNSS fix + breadcrumbs per missing event
└── docs/
    └── change_case.md     # written by save_change_case() (generated)
```

## Tools

| Tool | Purpose |
|------|---------|
| `get_signals()` | Clustered signals: rising safety-critical public-opinion cluster `PO-2026-041`, related Jira defect, and an unrelated low-priority CR. |
| `query_ecall_events(location_missing=None)` | All 40 events, or only those with/without a missing position. |
| `get_location_sources(event_id)` | Last valid GNSS fix (5-60 s pre-impact) + 3-5 breadcrumbs for a missing event. |
| `search_spec(keyword)` | Case-insensitive line search over `../ecall-tcu/docs/ecall_spec.md`. |
| `save_change_case(markdown)` | Writes `docs/change_case.md`. |

## Setup

```bash
python -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

## Run

```bash
python server.py          # stdio transport
```

Example MCP client config (`.kiro/settings/mcp.json`):

```json
{
  "mcpServers": {
    "ecall-mcp": {
      "command": "python",
      "args": ["server.py"],
      "cwd": "workshop-future-state-code/ecall-mcp",
      "disabled": false
    }
  }
}
```

## Self-test

```bash
python selftest.py
```

Validates the data contract (40 events, ~8 missing and all severe + rollover/
tunnel-exit + `NO_FIX` + backend payload == vehicle payload; recent fix and
breadcrumbs for every missing event) and exercises all five tools. Exits 0 on
success.
