#!/usr/bin/env python3
"""investigate.py — Strands agent that triages the eCall location-missing signal.

The agent connects to the local `ecall-mcp` MCP server over stdio, pulls the
triage signals and field data, tests four hypotheses about why eCall sessions
arrive with no location, checks the TCU spec for GNSS-loss coverage, classifies
the finding, and (if it is a requirement gap) drafts EARS requirements and saves
a change case for human approval.

Every MCP tool call and a summary of its result are printed to stdout so the
flow is visible during the demo.

Environment:
    BEDROCK_MODEL_ID   (required)  Bedrock model id to drive the agent.
    AWS credentials/region configured for Bedrock in us-east-1.

Run:
    export BEDROCK_MODEL_ID=...
    python agent/investigate.py
"""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path

from mcp import StdioServerParameters, stdio_client
from strands import Agent
from strands.hooks import AfterToolCallEvent, BeforeToolCallEvent, HookProvider, HookRegistry
from strands.models import BedrockModel
from strands.tools.mcp import MCPClient

# ---------------------------------------------------------------------------
# Paths — the MCP server lives next to this agent package.
# ---------------------------------------------------------------------------
HERE = Path(__file__).resolve().parent
SERVER_PATH = (HERE.parent / "ecall-mcp" / "server.py").resolve()


# ---------------------------------------------------------------------------
# Demo visibility: print every tool call and a short result summary.
# ---------------------------------------------------------------------------
def _summarize(value: object, limit: int = 600) -> str:
    """Render a tool result compactly for the demo log."""
    try:
        text = json.dumps(value, default=str, ensure_ascii=False)
    except (TypeError, ValueError):
        text = str(value)
    text = " ".join(text.split())
    if len(text) > limit:
        text = text[:limit] + f"... [+{len(text) - limit} chars]"
    return text


class ToolCallPrinter(HookProvider):
    """Hook provider that logs each tool invocation and its result summary."""

    def register_hooks(self, registry: HookRegistry, **kwargs) -> None:
        registry.add_callback(BeforeToolCallEvent, self._before)
        registry.add_callback(AfterToolCallEvent, self._after)

    @staticmethod
    def _tool_use(event) -> dict:
        return getattr(event, "tool_use", None) or {}

    def _before(self, event: BeforeToolCallEvent) -> None:
        tu = self._tool_use(event)
        name = tu.get("name", "<unknown>")
        args = tu.get("input", {})
        print(f"\n>>> TOOL CALL: {name}({_summarize(args, 200)})", flush=True)

    def _after(self, event: AfterToolCallEvent) -> None:
        tu = self._tool_use(event)
        name = tu.get("name", "<unknown>")
        result = getattr(event, "result", None)
        # Tool results are MCP content blocks; pull text where possible.
        summary = result
        if isinstance(result, dict):
            content = result.get("content")
            if isinstance(content, list):
                parts = [c.get("text", c) for c in content if isinstance(c, dict)]
                summary = parts if parts else content
        print(f"<<< TOOL RESULT: {name} -> {_summarize(summary)}", flush=True)


# ---------------------------------------------------------------------------
# System prompt — the investigation playbook.
# ---------------------------------------------------------------------------
SYSTEM_PROMPT = """
You are an eCall triage engineer investigating field signals against the TCU
baseline specification. Work methodically and use the MCP tools for every fact —
never invent data.

STEP 1 — Pick the signal.
Call get_signals. Choose the single HIGHEST-PRIORITY change signal using this
ordering: safety-critical first, then volume (post/report count), then trend
(rising beats flat). State which signal you picked and why. Ignore unrelated
cosmetic/low-priority change requests.

STEP 2 — Gather evidence.
Call query_ecall_events(location_missing=True) to get the events where the MSD
position was missing. For each such event, call get_location_sources(event_id)
to retrieve the last GNSS fix before impact and the breadcrumb trail. Note the
total event count and the missing-location count from query_ecall_events().

STEP 3 — Test four hypotheses against the evidence. For each, state VERDICT
(supported / rejected / inconclusive) with the specific evidence used:
  H1  Antenna/ECU physically damaged at impact.
  H2  No GNSS fix available at tunnel exit (signal reacquisition lag).
  H3  The backend drops/strips the location after receipt.
  H4  Customer misunderstanding (the location was actually sent).
Rules:
  - REJECT H3 if the backend payload equals the vehicle payload for the missing
    events (identical payload hashes => the backend did not alter/drop location;
    the position was already NOT_AVAILABLE on the wire).
  - Use the event data (severity, crash_type, location_api_response=NO_FIX,
    gnss_fix_age) and the location-source data (a valid fix existed 5-60 s
    before impact) to judge H1, H2 and H4.
  - A recent valid last-known fix plus NO_FIX at trigger supports H2 (loss of
    live fix) and weighs against H1 (hardware would not have logged a fix
    seconds earlier) and H4 (the location truly was not transmitted).

STEP 4 — Check spec coverage.
Call search_spec with keywords such as "GNSS", "fallback", "NOT_AVAILABLE",
"last-known", "position". Determine whether the baseline specifies any behavior
for GNSS loss other than emitting NOT_AVAILABLE. The baseline is live-GNSS-only
with NO fallback — confirm this from the spec text.

STEP 5 — Classify the finding as exactly one:
  P1 = defect: software violates an existing requirement.
  P2 = requirement gap: software meets the spec, but the spec itself fails to
       cover a real safety need (here: no GNSS-loss fallback / last-known
       position). This is the expected outcome if H2 holds and H1/H3/H4 are
       rejected.
  P3 = planned change: already covered by a planned/accepted change.
  P0 = no software change needed.
State the classification and the one-line justification.

STEP 6 — If and only if the classification is P2, draft EARS-style requirements
with IDs ECALL-LOC-01 through ECALL-LOC-09 that introduce a GNSS-loss fallback
(e.g. use a recent last-known fix within a bounded age, mark position as
not-trusted, record fix age, never emit 0,0, log the substitution, bound the
staleness window, and expose the behavior in config). Use proper EARS phrasing
("When <trigger>, the TCU shall <response>." / "While <state>, the TCU shall
<response>." / "The TCU shall <response>."). Keep each requirement atomic and
testable.

STEP 7 — Assemble a Markdown change case and save it by calling
save_change_case(markdown=...). The document MUST contain, in this order:
  1. Title and a one-paragraph summary.
  2. "## Signals" — the signals considered and the one selected (with priority
     reasoning).
  3. "## Hypotheses" — H1..H4 each with verdict and the concrete evidence
     (cite event ids, payload-hash equality for H3, fix ages for H2).
  4. "## Classification" — P0/P1/P2/P3 with justification.
  5. "## Requirements" — the EARS requirements ECALL-LOC-01..09 (only if P2).
  6. A final line exactly: "Status: awaiting human approval".
After saving, briefly confirm the path returned by the tool. Do not take any
further action — this is a draft for a human to approve.
"""


def build_model() -> BedrockModel:
    model_id = os.environ["BEDROCK_MODEL_ID"]
    # streaming is opt-out via BEDROCK_STREAMING=0/false; some Bedrock roles are
    # authorized for Converse but not ConverseStream, so default to non-streaming.
    streaming = os.environ.get("BEDROCK_STREAMING", "false").lower() in {
        "1", "true", "yes", "y"
    }
    return BedrockModel(
        model_id=model_id, region_name="us-east-1", streaming=streaming
    )


def build_mcp_client() -> MCPClient:
    return MCPClient(
        lambda: stdio_client(
            StdioServerParameters(
                command=sys.executable,
                args=[str(SERVER_PATH)],
            )
        )
    )


def main() -> int:
    if "BEDROCK_MODEL_ID" not in os.environ:
        print("ERROR: set BEDROCK_MODEL_ID (e.g. export BEDROCK_MODEL_ID=...).",
              file=sys.stderr)
        return 2

    model = build_model()
    mcp_client = build_mcp_client()

    print(f"Connecting to ecall-mcp over stdio: {SERVER_PATH}", flush=True)
    # Manual context management so tools are live for the whole run.
    with mcp_client:
        tools = mcp_client.list_tools_sync()
        print("Loaded tools:", ", ".join(getattr(t, "tool_name", str(t)) for t in tools),
              flush=True)

        agent = Agent(
            model=model,
            tools=tools,
            system_prompt=SYSTEM_PROMPT,
            hooks=[ToolCallPrinter()],
        )

        print("\n" + "=" * 72)
        print("Starting investigation")
        print("=" * 72, flush=True)

        result = agent(
            "Investigate the highest-priority change signal end to end, following "
            "your system instructions, and save the change case."
        )

        print("\n" + "=" * 72)
        print("Investigation complete")
        print("=" * 72)
        print(result)
    return 0


if __name__ == "__main__":
    sys.exit(main())
