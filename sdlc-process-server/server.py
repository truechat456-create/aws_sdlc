# server.py
#
# SDLC Process Server - Requirements traceability MCP server.
#
# Provides tools for an automotive supplier's SDLC process integration with Kiro.
# Tracks requirements, design documents, code commits and test results, and maps
# them onto a V-Cycle traceability matrix (requirement -> design -> implementation
# -> verification).
#
# Storage is persistent via SQLite so state survives server restarts.

from mcp.server import MCPServer

import json
import os
import re
import sqlite3
from contextlib import contextmanager
from datetime import datetime, timezone
from typing import Any, Iterator

mcp = MCPServer("sdlc-process-tools", version="2.0.0")

# ---------------------------------------------------------------------------
# Storage
# ---------------------------------------------------------------------------

# Allow the DB location to be overridden (useful for tests / deployment). Default
# is a file next to this module so state is predictable regardless of CWD.
DB_PATH = os.environ.get(
    "SDLC_DB_PATH",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "sdlc_traceability.db"),
)

VALID_PRIORITIES = ("low", "medium", "high", "critical")
PRIORITY_ORDER = {"critical": 0, "high": 1, "medium": 2, "low": 3}

# Requirement lifecycle states, ordered from least to most complete.
VALID_STATUSES = ("defined", "designed", "implemented", "verified")

# V-Cycle phases, left (decomposition) to right (integration/verification).
VCYCLE_PHASES = (
    "requirement",     # what the system must do
    "design",          # how it will be built (design docs / architecture)
    "implementation",  # code commits realizing the design
    "verification",    # tests confirming the requirement is met
)


def _now() -> str:
    """Return a timezone-aware UTC timestamp in ISO 8601 format."""
    return datetime.now(timezone.utc).isoformat()


@contextmanager
def _connect() -> Iterator[sqlite3.Connection]:
    """Yield a SQLite connection with foreign keys enabled and row access by name.

    Commits on success, rolls back on error, always closes.
    """
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA foreign_keys = ON")
    try:
        yield conn
        conn.commit()
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()


def _init_db() -> None:
    """Create tables if they do not already exist. Safe to call repeatedly."""
    with _connect() as conn:
        conn.executescript(
            """
            CREATE TABLE IF NOT EXISTS requirements (
                req_id      TEXT PRIMARY KEY,
                title       TEXT NOT NULL,
                description TEXT NOT NULL,
                priority    TEXT NOT NULL,
                status      TEXT NOT NULL,
                created     TEXT NOT NULL,
                updated     TEXT NOT NULL
            );

            CREATE TABLE IF NOT EXISTS design_docs (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                req_id      TEXT NOT NULL,
                doc_ref     TEXT NOT NULL,
                title       TEXT NOT NULL,
                description TEXT NOT NULL DEFAULT '',
                timestamp   TEXT NOT NULL,
                FOREIGN KEY (req_id) REFERENCES requirements(req_id) ON DELETE CASCADE,
                UNIQUE (req_id, doc_ref)
            );

            CREATE TABLE IF NOT EXISTS commits (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                req_id      TEXT NOT NULL,
                commit_hash TEXT NOT NULL,
                file_path   TEXT NOT NULL,
                description TEXT NOT NULL DEFAULT '',
                timestamp   TEXT NOT NULL,
                FOREIGN KEY (req_id) REFERENCES requirements(req_id) ON DELETE CASCADE,
                UNIQUE (req_id, commit_hash)
            );

            CREATE TABLE IF NOT EXISTS test_results (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                req_id      TEXT NOT NULL,
                test_name   TEXT NOT NULL,
                passed      INTEGER NOT NULL,
                details     TEXT NOT NULL DEFAULT '',
                timestamp   TEXT NOT NULL,
                FOREIGN KEY (req_id) REFERENCES requirements(req_id) ON DELETE CASCADE
            );

            CREATE INDEX IF NOT EXISTS idx_design_req ON design_docs(req_id);
            CREATE INDEX IF NOT EXISTS idx_commits_req ON commits(req_id);
            CREATE INDEX IF NOT EXISTS idx_tests_req ON test_results(req_id);
            """
        )


# ---------------------------------------------------------------------------
# Validation helpers
# ---------------------------------------------------------------------------

# Requirement IDs such as "FR-9.1", "SYS_REQ_002", "ASIL-D-12". Alphanumerics plus
# a few separators, kept deliberately permissive but bounded.
_REQ_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._\-]{0,63}$")

# Git hashes: 7-40 hex chars (short or full SHA-1), or a full SHA-256 (64).
_COMMIT_RE = re.compile(r"^[0-9a-fA-F]{7,64}$")


class ValidationError(ValueError):
    """Raised when a tool receives invalid input."""


def _require_str(value: Any, field: str, *, max_len: int = 2000, allow_empty: bool = False) -> str:
    """Validate that ``value`` is a string within bounds; return it stripped."""
    if not isinstance(value, str):
        raise ValidationError(f"'{field}' must be a string, got {type(value).__name__}")
    stripped = value.strip()
    if not allow_empty and not stripped:
        raise ValidationError(f"'{field}' must not be empty")
    if len(value) > max_len:
        raise ValidationError(f"'{field}' exceeds maximum length of {max_len} characters")
    return stripped


def _validate_req_id(req_id: Any) -> str:
    rid = _require_str(req_id, "req_id", max_len=64)
    if not _REQ_ID_RE.match(rid):
        raise ValidationError(
            "'req_id' must be 1-64 chars, start alphanumeric, and contain only "
            "letters, digits, '.', '_' or '-'"
        )
    return rid


def _validate_commit_hash(commit_hash: Any) -> str:
    ch = _require_str(commit_hash, "commit_hash", max_len=64)
    if not _COMMIT_RE.match(ch):
        raise ValidationError("'commit_hash' must be 7-64 hexadecimal characters")
    return ch.lower()


def _validate_priority(priority: Any) -> str:
    p = _require_str(priority, "priority", max_len=16).lower()
    if p not in VALID_PRIORITIES:
        raise ValidationError(
            f"'priority' must be one of {', '.join(VALID_PRIORITIES)}; got '{p}'"
        )
    return p


def _ok(**payload: Any) -> str:
    """Serialize a successful tool response."""
    return json.dumps({"ok": True, **payload}, indent=2, default=str)


def _err(message: str, **extra: Any) -> str:
    """Serialize an error tool response. Tools never raise to the transport."""
    return json.dumps({"ok": False, "error": message, **extra}, indent=2, default=str)


def _require_requirement_exists(conn: sqlite3.Connection, req_id: str) -> sqlite3.Row:
    row = conn.execute(
        "SELECT * FROM requirements WHERE req_id = ?", (req_id,)
    ).fetchone()
    if row is None:
        raise ValidationError(f"Requirement '{req_id}' not found; track it first")
    return row


def _recompute_status(conn: sqlite3.Connection, req_id: str) -> str:
    """Derive and persist a requirement's status from its linked artifacts.

    Follows the V-Cycle: a requirement advances to 'verified' only when it has at
    least one passing test and no failing tests; 'implemented' when it has commits;
    'designed' when it has design docs; otherwise 'defined'.
    """
    has_design = conn.execute(
        "SELECT 1 FROM design_docs WHERE req_id = ? LIMIT 1", (req_id,)
    ).fetchone() is not None
    has_commits = conn.execute(
        "SELECT 1 FROM commits WHERE req_id = ? LIMIT 1", (req_id,)
    ).fetchone() is not None

    tests = conn.execute(
        "SELECT passed FROM test_results WHERE req_id = ?", (req_id,)
    ).fetchall()
    has_tests = len(tests) > 0
    all_passed = has_tests and all(bool(t["passed"]) for t in tests)

    if all_passed and has_commits:
        status = "verified"
    elif has_commits:
        status = "implemented"
    elif has_design:
        status = "designed"
    else:
        status = "defined"

    conn.execute(
        "UPDATE requirements SET status = ?, updated = ? WHERE req_id = ?",
        (status, _now(), req_id),
    )
    return status


# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------

@mcp.tool()
async def track_requirement(req_id: str, title: str, description: str, priority: str = "medium") -> str:
    """Track a new requirement or update an existing one.

    Args:
        req_id: Requirement identifier (e.g., "FR-9.1"). 1-64 chars, alphanumeric
            with '.', '_' or '-'.
        title: Short title of the requirement.
        description: Detailed description.
        priority: Priority level (low, medium, high, critical).

    Returns:
        JSON confirmation with requirement status and total count.
    """
    try:
        req_id = _validate_req_id(req_id)
        title = _require_str(title, "title", max_len=256)
        description = _require_str(description, "description", max_len=8000)
        priority = _validate_priority(priority)
    except ValidationError as e:
        return _err(str(e))

    try:
        now = _now()
        with _connect() as conn:
            existing = conn.execute(
                "SELECT created, status FROM requirements WHERE req_id = ?", (req_id,)
            ).fetchone()
            if existing is None:
                conn.execute(
                    """INSERT INTO requirements
                       (req_id, title, description, priority, status, created, updated)
                       VALUES (?, ?, ?, ?, 'defined', ?, ?)""",
                    (req_id, title, description, priority, now, now),
                )
                action = "created"
            else:
                conn.execute(
                    """UPDATE requirements
                       SET title = ?, description = ?, priority = ?, updated = ?
                       WHERE req_id = ?""",
                    (title, description, priority, now, req_id),
                )
                action = "updated"
            # Status may change if artifacts already exist for this req_id.
            status = _recompute_status(conn, req_id)
            total = conn.execute("SELECT COUNT(*) AS c FROM requirements").fetchone()["c"]
        return _ok(action=action, req_id=req_id, status=status, total_requirements=total)
    except sqlite3.Error as e:
        return _err(f"database error: {e}")


@mcp.tool()
async def link_design_doc(req_id: str, doc_ref: str, title: str, description: str = "") -> str:
    """Link a design document to a requirement (the left side of the V-Cycle).

    Args:
        req_id: Requirement identifier this design satisfies.
        doc_ref: Document reference/identifier (e.g., "DD-9.1", a URL, or path).
        title: Short title of the design document.
        description: What this design covers.

    Returns:
        JSON confirmation of the design linkage.
    """
    try:
        req_id = _validate_req_id(req_id)
        doc_ref = _require_str(doc_ref, "doc_ref", max_len=512)
        title = _require_str(title, "title", max_len=256)
        description = _require_str(description, "description", max_len=8000, allow_empty=True)
    except ValidationError as e:
        return _err(str(e))

    try:
        with _connect() as conn:
            _require_requirement_exists(conn, req_id)
            try:
                conn.execute(
                    """INSERT INTO design_docs (req_id, doc_ref, title, description, timestamp)
                       VALUES (?, ?, ?, ?, ?)""",
                    (req_id, doc_ref, title, description, _now()),
                )
            except sqlite3.IntegrityError:
                return _err(
                    f"design doc '{doc_ref}' is already linked to '{req_id}'",
                    req_id=req_id, doc_ref=doc_ref,
                )
            status = _recompute_status(conn, req_id)
        return _ok(action="linked_design", req_id=req_id, doc_ref=doc_ref, status=status)
    except ValidationError as e:
        return _err(str(e))
    except sqlite3.Error as e:
        return _err(f"database error: {e}")


@mcp.tool()
async def link_commit(req_id: str, commit_hash: str, file_path: str, description: str) -> str:
    """Link a code commit to a requirement for traceability (implementation phase).

    Args:
        req_id: Requirement identifier this commit addresses.
        commit_hash: Git commit hash (7-64 hex characters).
        file_path: Primary file changed.
        description: What this commit implements.

    Returns:
        JSON traceability link confirmation.
    """
    try:
        req_id = _validate_req_id(req_id)
        commit_hash = _validate_commit_hash(commit_hash)
        file_path = _require_str(file_path, "file_path", max_len=1024)
        description = _require_str(description, "description", max_len=8000, allow_empty=True)
    except ValidationError as e:
        return _err(str(e))

    try:
        with _connect() as conn:
            _require_requirement_exists(conn, req_id)
            try:
                conn.execute(
                    """INSERT INTO commits (req_id, commit_hash, file_path, description, timestamp)
                       VALUES (?, ?, ?, ?, ?)""",
                    (req_id, commit_hash, file_path, description, _now()),
                )
            except sqlite3.IntegrityError:
                return _err(
                    f"commit '{commit_hash}' is already linked to '{req_id}'",
                    req_id=req_id, commit_hash=commit_hash,
                )
            status = _recompute_status(conn, req_id)
        return _ok(action="linked_commit", req_id=req_id, commit_hash=commit_hash, status=status)
    except ValidationError as e:
        return _err(str(e))
    except sqlite3.Error as e:
        return _err(f"database error: {e}")


@mcp.tool()
async def record_test_result(req_id: str, test_name: str, passed: bool, details: str = "") -> str:
    """Record a test result against a requirement (verification phase).

    Args:
        req_id: Requirement being verified.
        test_name: Name of the test.
        passed: Whether the test passed.
        details: Additional details or failure reason.

    Returns:
        JSON test result confirmation.
    """
    try:
        req_id = _validate_req_id(req_id)
        test_name = _require_str(test_name, "test_name", max_len=256)
        details = _require_str(details, "details", max_len=8000, allow_empty=True)
        if not isinstance(passed, bool):
            raise ValidationError("'passed' must be a boolean")
    except ValidationError as e:
        return _err(str(e))

    try:
        with _connect() as conn:
            _require_requirement_exists(conn, req_id)
            conn.execute(
                """INSERT INTO test_results (req_id, test_name, passed, details, timestamp)
                   VALUES (?, ?, ?, ?, ?)""",
                (req_id, test_name, 1 if passed else 0, details, _now()),
            )
            status = _recompute_status(conn, req_id)
        return _ok(
            action="recorded_test", req_id=req_id, test=test_name,
            passed=passed, status=status,
        )
    except ValidationError as e:
        return _err(str(e))
    except sqlite3.Error as e:
        return _err(f"database error: {e}")


@mcp.tool()
async def get_requirement_status(req_id: str) -> str:
    """Query the current status of a specific requirement, with linked artifacts.

    Args:
        req_id: Requirement identifier to look up.

    Returns:
        JSON requirement details including design docs, commits and test results.
    """
    try:
        req_id = _validate_req_id(req_id)
    except ValidationError as e:
        return _err(str(e))

    try:
        with _connect() as conn:
            req = conn.execute(
                "SELECT * FROM requirements WHERE req_id = ?", (req_id,)
            ).fetchone()
            if req is None:
                return _err(f"Requirement '{req_id}' not found")

            designs = conn.execute(
                "SELECT doc_ref, title, description, timestamp FROM design_docs WHERE req_id = ? ORDER BY id",
                (req_id,),
            ).fetchall()
            commits = conn.execute(
                "SELECT commit_hash, file_path, description, timestamp FROM commits WHERE req_id = ? ORDER BY id",
                (req_id,),
            ).fetchall()
            tests = conn.execute(
                "SELECT test_name, passed, details, timestamp FROM test_results WHERE req_id = ? ORDER BY id",
                (req_id,),
            ).fetchall()

        payload = dict(req)
        payload["design_docs"] = [dict(d) for d in designs]
        payload["commits"] = [dict(c) for c in commits]
        payload["test_results"] = [
            {**dict(t), "passed": bool(t["passed"])} for t in tests
        ]
        return _ok(requirement=payload)
    except sqlite3.Error as e:
        return _err(f"database error: {e}")


@mcp.tool()
async def get_coverage_report() -> str:
    """Generate a requirements traceability and coverage summary.

    Returns:
        JSON report showing per-requirement status plus implementation and test
        coverage totals and gaps.
    """
    try:
        with _connect() as conn:
            reqs = conn.execute(
                "SELECT req_id, title, status FROM requirements ORDER BY req_id"
            ).fetchall()

            report: dict[str, Any] = {
                "total_requirements": len(reqs),
                "designed": 0, "implemented": 0, "tested": 0, "verified": 0,
                "gaps": [], "requirements": {},
            }

            for req in reqs:
                req_id = req["req_id"]
                n_design = conn.execute(
                    "SELECT COUNT(*) AS c FROM design_docs WHERE req_id = ?", (req_id,)
                ).fetchone()["c"]
                n_commits = conn.execute(
                    "SELECT COUNT(*) AS c FROM commits WHERE req_id = ?", (req_id,)
                ).fetchone()["c"]
                tests = conn.execute(
                    "SELECT passed FROM test_results WHERE req_id = ?", (req_id,)
                ).fetchall()
                n_tests = len(tests)
                all_passed = n_tests > 0 and all(bool(t["passed"]) for t in tests)

                if n_design:
                    report["designed"] += 1
                if n_commits:
                    report["implemented"] += 1
                if n_tests:
                    report["tested"] += 1
                if all_passed and n_commits:
                    report["verified"] += 1

                missing = []
                if not n_design:
                    missing.append("design")
                if not n_commits:
                    missing.append("implementation")
                if not n_tests:
                    missing.append("tests")
                if missing:
                    report["gaps"].append({"req_id": req_id, "missing": missing})

                report["requirements"][req_id] = {
                    "title": req["title"], "status": req["status"],
                    "design_docs": n_design, "commits": n_commits, "tests": n_tests,
                }
        return _ok(report=report)
    except sqlite3.Error as e:
        return _err(f"database error: {e}")


@mcp.tool()
async def get_gaps() -> str:
    """Identify requirements missing design, implementation or tests, by priority.

    Returns:
        JSON list of requirements with coverage gaps, highest priority first.
    """
    try:
        with _connect() as conn:
            reqs = conn.execute(
                "SELECT req_id, title, priority FROM requirements"
            ).fetchall()

            gaps = []
            for req in reqs:
                req_id = req["req_id"]
                has_design = conn.execute(
                    "SELECT 1 FROM design_docs WHERE req_id = ? LIMIT 1", (req_id,)
                ).fetchone() is not None
                has_commits = conn.execute(
                    "SELECT 1 FROM commits WHERE req_id = ? LIMIT 1", (req_id,)
                ).fetchone() is not None
                has_tests = conn.execute(
                    "SELECT 1 FROM test_results WHERE req_id = ? LIMIT 1", (req_id,)
                ).fetchone() is not None

                missing = []
                if not has_design:
                    missing.append("design")
                if not has_commits:
                    missing.append("implementation")
                if not has_tests:
                    missing.append("tests")
                if missing:
                    gaps.append({
                        "req_id": req_id, "title": req["title"],
                        "priority": req["priority"], "missing": missing,
                    })

        gaps.sort(key=lambda g: PRIORITY_ORDER.get(g["priority"], 4))
        return _ok(total_gaps=len(gaps), gaps=gaps)
    except sqlite3.Error as e:
        return _err(f"database error: {e}")


@mcp.tool()
async def get_vcycle_matrix(req_id: str = "") -> str:
    """Generate a V-Cycle traceability matrix.

    Maps each requirement across the V-Cycle phases: requirement -> design docs ->
    implementation commits -> verification tests. Each row reports which phases are
    satisfied, the artifacts attached to each phase, and where the trace is broken.

    Args:
        req_id: Optional. Limit the matrix to a single requirement. If omitted or
            empty, the full matrix for all requirements is returned.

    Returns:
        JSON matrix with one row per requirement, per-phase artifacts and coverage
        flags, plus overall phase totals.
    """
    try:
        filter_id = _validate_req_id(req_id) if req_id and req_id.strip() else None
    except ValidationError as e:
        return _err(str(e))

    try:
        with _connect() as conn:
            if filter_id is not None:
                reqs = conn.execute(
                    "SELECT * FROM requirements WHERE req_id = ?", (filter_id,)
                ).fetchall()
                if not reqs:
                    return _err(f"Requirement '{filter_id}' not found")
            else:
                reqs = conn.execute(
                    "SELECT * FROM requirements ORDER BY req_id"
                ).fetchall()

            rows = []
            totals = {phase: 0 for phase in VCYCLE_PHASES}

            for req in reqs:
                rid = req["req_id"]

                designs = conn.execute(
                    "SELECT doc_ref, title FROM design_docs WHERE req_id = ? ORDER BY id",
                    (rid,),
                ).fetchall()
                commits = conn.execute(
                    "SELECT commit_hash, file_path FROM commits WHERE req_id = ? ORDER BY id",
                    (rid,),
                ).fetchall()
                tests = conn.execute(
                    "SELECT test_name, passed FROM test_results WHERE req_id = ? ORDER BY id",
                    (rid,),
                ).fetchall()

                n_tests = len(tests)
                n_passed = sum(1 for t in tests if t["passed"])
                all_passed = n_tests > 0 and n_passed == n_tests

                phases = {
                    "requirement": {
                        "satisfied": True,
                        "artifacts": [{"ref": rid, "title": req["title"]}],
                    },
                    "design": {
                        "satisfied": len(designs) > 0,
                        "artifacts": [{"ref": d["doc_ref"], "title": d["title"]} for d in designs],
                    },
                    "implementation": {
                        "satisfied": len(commits) > 0,
                        "artifacts": [
                            {"ref": c["commit_hash"], "file": c["file_path"]} for c in commits
                        ],
                    },
                    "verification": {
                        # Verification is only "satisfied" when tests exist AND all pass.
                        "satisfied": all_passed,
                        "artifacts": [
                            {"test": t["test_name"], "passed": bool(t["passed"])} for t in tests
                        ],
                        "tests_passed": n_passed,
                        "tests_total": n_tests,
                    },
                }

                for phase in VCYCLE_PHASES:
                    if phases[phase]["satisfied"]:
                        totals[phase] += 1

                # The trace is complete when every phase is satisfied. The first
                # unsatisfied phase (in V-Cycle order) is where the chain breaks.
                broken_at = next(
                    (p for p in VCYCLE_PHASES if not phases[p]["satisfied"]), None
                )

                rows.append({
                    "req_id": rid,
                    "title": req["title"],
                    "priority": req["priority"],
                    "status": req["status"],
                    "phases": phases,
                    "trace_complete": broken_at is None,
                    "trace_broken_at": broken_at,
                })

            matrix = {
                "generated": _now(),
                "phase_order": list(VCYCLE_PHASES),
                "total_requirements": len(rows),
                "fully_traced": sum(1 for r in rows if r["trace_complete"]),
                "phase_coverage": totals,
                "rows": rows,
            }
        return _ok(matrix=matrix)
    except sqlite3.Error as e:
        return _err(f"database error: {e}")


# Initialize storage at import time so the schema exists before any tool runs.
_init_db()


if __name__ == "__main__":
    mcp.run(transport="stdio")
