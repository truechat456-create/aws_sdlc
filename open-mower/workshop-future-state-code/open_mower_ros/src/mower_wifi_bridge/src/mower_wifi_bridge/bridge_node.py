"""ROS node implementation for the FR-9 WiFi remote start/stop bridge.

`mower_wifi_bridge` is a thin, token-authenticated bridge (16-WiFi-Subsystem.md):
it translates the external WebSocket JSON protocol into the mower's EXISTING ROS
command/status interfaces and never touches motors or the firmware link directly.

Requirement coverage:
  FR-9.1  WebSocket server on a configurable port (default 8765)
  FR-9.2  START           -> mower_service/high_level_control COMMAND_START
  FR-9.3  STOP            -> mower_service/high_level_control COMMAND_HOME
  FR-9.4  EMERGENCY       -> ll/_service/emergency (issued immediately, < 1 s)
  FR-9.5  per-message token authentication (reject + log on mismatch, no state change)
  FR-9.6  1 Hz status feedback from /mower_logic/current_state
  SR-7    RESET_EMERGENCY -> COMMAND_RESET_EMERGENCY (asserts, cannot clear LL latch)
  NFR-6   token comes from a ROS param, never hard-coded
"""

import json
import threading

import rospy

from mower_msgs.srv import HighLevelControlSrv, HighLevelControlSrvRequest
from mower_msgs.srv import EmergencyStopSrv, EmergencyStopSrvRequest
from mower_msgs.msg import HighLevelStatus

from .protocol import CommandResult, authorize, map_command, status_to_dict
from .ws_server import WebSocketServer

HIGH_LEVEL_CONTROL_SERVICE = "mower_service/high_level_control"
EMERGENCY_SERVICE = "ll/_service/emergency"
CURRENT_STATE_TOPIC = "/mower_logic/current_state"


class WifiBridgeNode(object):
    def __init__(self):
        # --- parameters (NFR-6: token from config, never hard-coded) ---------
        self.port = int(rospy.get_param("~port", 8765))        # FR-9.1
        self.bind_ip = rospy.get_param("~bind_ip", "0.0.0.0")
        self.token = rospy.get_param("~token", "")             # FR-9.5 / NFR-6
        self.status_rate_hz = float(rospy.get_param("~status_rate_hz", 1.0))  # FR-9.6

        if not self.token:
            rospy.logwarn(
                "mower_wifi_bridge: no '~token' configured - all remote commands "
                "will be REJECTED (FR-9.5). Set a strong token in params/wifi.yaml."
            )

        # --- cached latched status for the 1 Hz pump (FR-9.6) -----------------
        self._latest_status = None
        self._status_lock = threading.Lock()

        # --- ROS interfaces (reuse existing services/topics only) -------------
        rospy.loginfo("mower_wifi_bridge: waiting for high-level control service...")
        rospy.wait_for_service(HIGH_LEVEL_CONTROL_SERVICE)
        self._high_level = rospy.ServiceProxy(HIGH_LEVEL_CONTROL_SERVICE, HighLevelControlSrv)

        rospy.loginfo("mower_wifi_bridge: waiting for emergency service...")
        rospy.wait_for_service(EMERGENCY_SERVICE)
        self._emergency = rospy.ServiceProxy(EMERGENCY_SERVICE, EmergencyStopSrv)

        self._status_sub = rospy.Subscriber(
            CURRENT_STATE_TOPIC, HighLevelStatus, self._on_current_state, queue_size=1
        )

        # --- WebSocket server (FR-9.1) ----------------------------------------
        self._server = WebSocketServer(
            host=self.bind_ip,
            port=self.port,
            on_message=self._on_ws_message,
            on_connect=lambda c: rospy.loginfo("WiFi client connected: %s" % (c.addr,)),
            on_disconnect=lambda c: rospy.loginfo("WiFi client gone: %s" % (c.addr,)),
            log=self._ws_log,
        )

    # ------------------------------------------------------------------ setup
    def start(self):
        self._server.start()
        period = 1.0 / self.status_rate_hz if self.status_rate_hz > 0 else 1.0
        self._status_timer = rospy.Timer(rospy.Duration(period), self._on_status_timer)  # FR-9.6
        rospy.loginfo("mower_wifi_bridge up on port %d (status %.1f Hz)", self.port, self.status_rate_hz)

    def shutdown(self):
        try:
            self._server.stop()
        except Exception:
            pass

    def _ws_log(self, level, msg):
        getattr(rospy, "log" + ("warn" if level == "warn" else "info"))("ws: %s" % msg)

    # -------------------------------------------------------------- ROS hooks
    def _on_current_state(self, msg):
        with self._status_lock:
            self._latest_status = msg

    def _on_status_timer(self, _event):
        # FR-9.6: republish cached HighLevelStatus as a JSON status frame at 1 Hz.
        with self._status_lock:
            status = self._latest_status
        frame = status_to_dict(status, stamp=rospy.get_time())
        self._server.broadcast(json.dumps(frame))

    # ---------------------------------------------------------- WS -> ROS map
    def _on_ws_message(self, client, text):
        try:
            message = json.loads(text)
        except ValueError:
            client.send_text(json.dumps({"error": "bad_json"}))
            return
        if not isinstance(message, dict):
            client.send_text(json.dumps({"error": "bad_message"}))
            return

        # FR-9.5 / SR-8: authenticate BEFORE acting on anything.
        if not authorize(message, self.token):
            rospy.logwarn("mower_wifi_bridge: rejected unauthorized command from %s", client.addr)
            client.send_text(json.dumps({"error": "unauthorized"}))
            return

        result = map_command(message)
        if result.kind == CommandResult.ERROR:
            client.send_text(json.dumps({"error": result.error}))
            return

        ok, detail = self._execute(result)
        client.send_text(json.dumps({"ok": ok, "command": message.get("command"), "detail": detail}))

    def _execute(self, result):
        try:
            if result.kind == CommandResult.EMERGENCY:  # FR-9.4 - immediate, < 1 s
                req = EmergencyStopSrvRequest()
                req.emergency = 1
                self._emergency(req)
                rospy.logwarn("mower_wifi_bridge: remote EMERGENCY asserted (FR-9.4)")
                return True, "emergency_asserted"
            if result.kind == CommandResult.HIGH_LEVEL:  # FR-9.2 / FR-9.3 / SR-7
                req = HighLevelControlSrvRequest()
                req.command = result.command
                self._high_level(req)
                return True, "command=%d" % result.command
        except rospy.ServiceException as exc:
            rospy.logerr("mower_wifi_bridge: service call failed: %s", exc)
            return False, "service_error"
        return False, "unhandled"


def main():
    rospy.init_node("mower_wifi_bridge")
    node = WifiBridgeNode()
    rospy.on_shutdown(node.shutdown)
    node.start()
    rospy.spin()
