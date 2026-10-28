"""Pure protocol/mapping logic for the FR-9 WiFi remote bridge.

This module is intentionally free of ROS and socket dependencies so it can be
unit-tested on any machine. It covers:

  FR-9.2  START           -> mower_msgs/HighLevelControlSrv COMMAND_START (1)
  FR-9.3  STOP            -> mower_msgs/HighLevelControlSrv COMMAND_HOME  (2)
  FR-9.4  EMERGENCY       -> mower_msgs/EmergencyStopSrv{emergency=1}
  SR-7    RESET_EMERGENCY -> mower_msgs/HighLevelControlSrv COMMAND_RESET_EMERGENCY (254)
  FR-9.5  token authentication (constant-time compare)
  FR-9.6  status dict mirroring mower_msgs/HighLevelStatus

See workshop-future-state-requirements/05-Communication-Protocols.md
("External WiFi Remote Protocol") and 16-WiFi-Subsystem.md.
"""

import hmac

# mower_msgs/HighLevelControlSrv command constants (mirrored here so the pure
# protocol layer has no ROS import; kept in lock-step with the .srv definition).
COMMAND_START = 1            # FR-9.2
COMMAND_HOME = 2             # FR-9.3 (STOP -> go home / stop autonomous op)
COMMAND_RESET_EMERGENCY = 254  # SR-7

# WiFi command tokens accepted on the wire (client -> node).
CMD_START = "START"
CMD_STOP = "STOP"
CMD_EMERGENCY = "EMERGENCY"
CMD_RESET_EMERGENCY = "RESET_EMERGENCY"


class CommandResult(object):
    """Outcome of mapping a decoded WiFi message to a ROS action.

    kind is one of:
      - "high_level": call HighLevelControlSrv with .command
      - "emergency":  call EmergencyStopSrv{emergency=1}  (FR-9.4)
      - "error":      reject; .error holds a short machine-readable reason
    """

    HIGH_LEVEL = "high_level"
    EMERGENCY = "emergency"
    ERROR = "error"

    def __init__(self, kind, command=None, error=None):
        self.kind = kind
        self.command = command
        self.error = error

    def __eq__(self, other):
        return (
            isinstance(other, CommandResult)
            and self.kind == other.kind
            and self.command == other.command
            and self.error == other.error
        )

    def __repr__(self):
        return "CommandResult(kind=%r, command=%r, error=%r)" % (
            self.kind,
            self.command,
            self.error,
        )


def authorize(message, configured_token):
    """FR-9.5 / SR-8: validate the per-message token before any relay.

    Uses a constant-time comparison to avoid leaking the token via timing.
    Returns True only when the message carries a token exactly matching the
    configured secret. A missing/empty configured token rejects everything.
    """
    if not configured_token:
        return False
    supplied = message.get("token")
    if not isinstance(supplied, str):
        return False
    # hmac.compare_digest is constant-time for equal-length inputs.
    return hmac.compare_digest(str(supplied), str(configured_token))


def map_command(message):
    """Map an authenticated WiFi message onto a ROS action (FR-9.2..9.4, SR-7).

    The caller is responsible for authorization first (see authorize); this only
    translates the already-trusted command field.
    """
    command = message.get("command")
    if command == CMD_START:  # FR-9.2
        return CommandResult(CommandResult.HIGH_LEVEL, command=COMMAND_START)
    if command == CMD_STOP:  # FR-9.3
        return CommandResult(CommandResult.HIGH_LEVEL, command=COMMAND_HOME)
    if command == CMD_EMERGENCY:  # FR-9.4 - issued immediately, < 1 s path
        return CommandResult(CommandResult.EMERGENCY)
    if command == CMD_RESET_EMERGENCY:  # SR-7
        return CommandResult(CommandResult.HIGH_LEVEL, command=COMMAND_RESET_EMERGENCY)
    return CommandResult(CommandResult.ERROR, error="unknown_command")


def status_to_dict(status, stamp=None):
    """FR-9.6: render a mower_msgs/HighLevelStatus into the JSON status frame.

    `status` may be an actual ROS message or any object exposing the same
    attributes, so this is testable without ROS. Missing attributes default
    gracefully.
    """

    def attr(name, default):
        return getattr(status, name, default) if status is not None else default

    return {
        "state": int(attr("state", 0)),
        "state_name": str(attr("state_name", "")),
        "sub_state_name": str(attr("sub_state_name", "")),
        "battery_percent": float(attr("battery_percent", 0.0)),
        "gps_quality_percent": float(attr("gps_quality_percent", 0.0)),
        "is_charging": bool(attr("is_charging", False)),
        "emergency": bool(attr("emergency", False)),
        "stamp": float(stamp) if stamp is not None else 0.0,
    }
