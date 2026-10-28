# lambda_function.py
import json
import os
from datetime import datetime
from decimal import Decimal

import boto3

TABLE_NAME = os.environ.get("TABLE_NAME", "sdlc-process-tracker")
table = boto3.resource("dynamodb").Table(TABLE_NAME)


def _resolve_tool_name(context):
    """AgentCore Gateway passes the invoked tool name in the client context.

    The Gateway prefixes the tool with the target name, e.g.
    "SdlcTools___track_requirement", so we strip the prefix.
    """
    try:
        tool_name = context.client_context.custom["bedrockAgentCoreToolName"]
    except (AttributeError, KeyError, TypeError):
        return None
    return tool_name.split("___")[-1] if "___" in tool_name else tool_name


# --- Tool implementations (logic ported from Module 1's server.py) ---

def track_requirement(event):
    """Track a new requirement or update an existing one."""
    req_id = event["req_id"]
    item = {
        "req_id": req_id,
        "title": event["title"],
        "description": event["description"],
        "priority": event.get("priority", "medium"),
        "status": "defined",
        "created": datetime.utcnow().isoformat(),
        "commits": [],
        "test_results": [],
    }
    table.put_item(Item=item)
    total = table.scan(Select="COUNT")["Count"]
    return {"status": "tracked", "req_id": req_id, "total_requirements": total}


def link_commit(event):
    """Link a code commit to a requirement for traceability."""
    req_id = event["req_id"]
    item = table.get_item(Key={"req_id": req_id}).get("Item")
    if not item:
        return {"error": f"Requirement {req_id} not found"}
    commits = item.get("commits", [])
    commits.append(event["commit_hash"])
    table.update_item(
        Key={"req_id": req_id},
        UpdateExpression="SET commits = :c, #s = :s",
        ExpressionAttributeNames={"#s": "status"},
        ExpressionAttributeValues={":c": commits, ":s": "implemented"},
    )
    return {"status": "linked", "req_id": req_id, "commit": event["commit_hash"]}


def record_test_result(event):
    """Record a test result against a requirement."""
    req_id = event["req_id"]
    item = table.get_item(Key={"req_id": req_id}).get("Item")
    if not item:
        return {"error": f"Requirement {req_id} not found"}
    result = {
        "test_name": event["test_name"],
        "passed": bool(event["passed"]),
        "details": event.get("details", ""),
        "timestamp": datetime.utcnow().isoformat(),
    }
    results = item.get("test_results", [])
    results.append(result)
    status = "verified" if result["passed"] else item.get("status", "implemented")
    table.update_item(
        Key={"req_id": req_id},
        UpdateExpression="SET test_results = :t, #s = :s",
        ExpressionAttributeNames={"#s": "status"},
        ExpressionAttributeValues={":t": results, ":s": status},
    )
    return {"status": "recorded", "req_id": req_id,
            "test": result["test_name"], "passed": result["passed"]}


def get_coverage_report(event):
    """Generate a requirements traceability and coverage report."""
    items = table.scan().get("Items", [])
    report = {"total_requirements": len(items), "implemented": 0,
              "tested": 0, "verified": 0, "gaps": [], "requirements": {}}
    for req in items:
        has_commits = len(req.get("commits", [])) > 0
        tests = req.get("test_results", [])
        has_tests = len(tests) > 0
        all_passed = has_tests and all(t["passed"] for t in tests)
        if has_commits:
            report["implemented"] += 1
        if has_tests:
            report["tested"] += 1
        if all_passed:
            report["verified"] += 1
        if not has_commits or not has_tests:
            report["gaps"].append({"req_id": req["req_id"],
                                    "missing_implementation": not has_commits,
                                    "missing_tests": not has_tests})
        report["requirements"][req["req_id"]] = {
            "title": req.get("title"), "status": req.get("status"),
            "commits": len(req.get("commits", [])), "tests": len(tests)}
    return report


def get_requirement_status(event):
    """Query the current status of a specific requirement."""
    item = table.get_item(Key={"req_id": event["req_id"]}).get("Item")
    if not item:
        return {"error": f"Requirement {event['req_id']} not found"}
    return item


def get_gaps(event):
    """Identify requirements that are missing implementations or tests."""
    gaps = []
    for req in table.scan().get("Items", []):
        missing = []
        if not req.get("commits"):
            missing.append("implementation")
        if not req.get("test_results"):
            missing.append("tests")
        if missing:
            gaps.append({"req_id": req["req_id"], "title": req.get("title"),
                         "priority": req.get("priority", "medium"), "missing": missing})
    order = {"critical": 0, "high": 1, "medium": 2, "low": 3}
    gaps.sort(key=lambda g: order.get(g["priority"], 4))
    return {"total_gaps": len(gaps), "gaps": gaps}


TOOLS = {
    "track_requirement": track_requirement,
    "link_commit": link_commit,
    "record_test_result": record_test_result,
    "get_coverage_report": get_coverage_report,
    "get_requirement_status": get_requirement_status,
    "get_gaps": get_gaps,
}


class DecimalEncoder(json.JSONEncoder):
    """DynamoDB returns numbers as Decimal; make them JSON-serializable."""
    def default(self, o):
        if isinstance(o, Decimal):
            return int(o) if o % 1 == 0 else float(o)
        return super().default(o)


def lambda_handler(event, context):
    tool_name = _resolve_tool_name(context)
    handler = TOOLS.get(tool_name)
    if handler is None:
        return {"statusCode": 400,
                "body": json.dumps({"error": f"Unknown tool: {tool_name}"})}
    try:
        result = handler(event or {})
        return {"statusCode": 200, "body": json.dumps(result, cls=DecimalEncoder)}
    except KeyError as exc:
        return {"statusCode": 400,
                "body": json.dumps({"error": f"Missing required field: {exc}"})}
    except Exception as exc:  # noqa: BLE001 - surface errors to the Gateway
        return {"statusCode": 500, "body": json.dumps({"error": str(exc)})}