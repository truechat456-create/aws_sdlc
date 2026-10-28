"""mower_wifi_bridge - WiFi remote start/stop bridge for OpenMower (FR-9).

Host-side ROS package that exposes a token-authenticated WebSocket interface on
the Raspberry Pi and relays remote commands onto the mower's existing ROS
command/status interfaces. See
workshop-future-state-requirements/16-WiFi-Subsystem.md.
"""

from .protocol import (  # noqa: F401
    COMMAND_START,
    COMMAND_HOME,
    COMMAND_RESET_EMERGENCY,
    CommandResult,
    authorize,
    map_command,
    status_to_dict,
)

__all__ = [
    "COMMAND_START",
    "COMMAND_HOME",
    "COMMAND_RESET_EMERGENCY",
    "CommandResult",
    "authorize",
    "map_command",
    "status_to_dict",
]
