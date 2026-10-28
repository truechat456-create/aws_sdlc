"""Minimal, dependency-free WebSocket server for the FR-9 WiFi bridge.

Implements just enough of RFC 6455 (handshake + text/close/ping frames) to serve
the remote start/stop protocol over WiFi on a configurable port (FR-9.1), with no
third-party packages so it runs on a stock Raspberry Pi / OpenMowerOS image.

Threading model: one acceptor thread, one thread per client connection. The owner
registers callbacks for connect/message/disconnect and can broadcast() the 1 Hz
status frame (FR-9.6) to all connected clients.

This module has no ROS dependency and is unit-testable on its own.
"""

import base64
import hashlib
import socket
import struct
import threading

_WS_MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

# RFC 6455 opcodes
_OP_CONT = 0x0
_OP_TEXT = 0x1
_OP_BINARY = 0x2
_OP_CLOSE = 0x8
_OP_PING = 0x9
_OP_PONG = 0xA


def _accept_key(client_key):
    sha = hashlib.sha1((client_key + _WS_MAGIC).encode("ascii")).digest()
    return base64.b64encode(sha).decode("ascii")


def build_text_frame(payload):
    """Build a server->client unmasked text frame (payload: str)."""
    data = payload.encode("utf-8")
    header = bytearray()
    header.append(0x80 | _OP_TEXT)  # FIN + text
    length = len(data)
    if length < 126:
        header.append(length)
    elif length < 65536:
        header.append(126)
        header += struct.pack(">H", length)
    else:
        header.append(127)
        header += struct.pack(">Q", length)
    return bytes(header) + data


class _Client(object):
    def __init__(self, conn, addr):
        self.conn = conn
        self.addr = addr
        self._send_lock = threading.Lock()
        self.open = True

    def send_text(self, text):
        if not self.open:
            return
        try:
            with self._send_lock:
                self.conn.sendall(build_text_frame(text))
        except OSError:
            self.open = False

    def close(self):
        self.open = False
        try:
            self.conn.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            self.conn.close()
        except OSError:
            pass


class WebSocketServer(object):
    """A tiny multi-client WebSocket server.

    Callbacks (all optional):
      on_connect(client)            - new client after successful handshake
      on_message(client, text)      - a text frame payload (str)
      on_disconnect(client)         - client gone
    """

    def __init__(self, host, port, on_message=None, on_connect=None, on_disconnect=None, log=None):
        self.host = host
        self.port = port
        self.on_message = on_message
        self.on_connect = on_connect
        self.on_disconnect = on_disconnect
        self._log = log or (lambda level, msg: None)

        self._server_sock = None
        self._acceptor = None
        self._clients = set()
        self._clients_lock = threading.Lock()
        self._running = False

    # ---- lifecycle -------------------------------------------------------
    def start(self):
        self._server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._server_sock.bind((self.host, self.port))  # FR-9.1 configurable bind
        self._server_sock.listen(8)
        self._server_sock.settimeout(0.5)
        self._running = True
        self._acceptor = threading.Thread(target=self._accept_loop, name="wifi-ws-accept")
        self._acceptor.daemon = True
        self._acceptor.start()
        self._log("info", "WebSocket server listening on %s:%d" % (self.host, self.port))

    def stop(self):
        self._running = False
        with self._clients_lock:
            clients = list(self._clients)
        for c in clients:
            c.close()
        if self._server_sock is not None:
            try:
                self._server_sock.close()
            except OSError:
                pass

    # ---- broadcast (FR-9.6) ---------------------------------------------
    def broadcast(self, text):
        with self._clients_lock:
            clients = list(self._clients)
        for c in clients:
            c.send_text(text)

    def client_count(self):
        with self._clients_lock:
            return len(self._clients)

    # ---- internals -------------------------------------------------------
    def _accept_loop(self):
        while self._running:
            try:
                conn, addr = self._server_sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            t = threading.Thread(target=self._handle_client, args=(conn, addr), name="wifi-ws-client")
            t.daemon = True
            t.start()

    def _handle_client(self, conn, addr):
        client = _Client(conn, addr)
        try:
            if not self._handshake(conn):
                conn.close()
                return
        except OSError:
            conn.close()
            return

        with self._clients_lock:
            self._clients.add(client)
        if self.on_connect:
            self.on_connect(client)
        self._log("info", "WiFi client connected: %s" % (addr,))

        try:
            self._recv_loop(client)
        finally:
            client.open = False
            with self._clients_lock:
                self._clients.discard(client)
            if self.on_disconnect:
                self.on_disconnect(client)
            client.close()
            self._log("info", "WiFi client disconnected: %s" % (addr,))

    def _handshake(self, conn):
        conn.settimeout(10.0)
        request = b""
        while b"\r\n\r\n" not in request:
            chunk = conn.recv(1024)
            if not chunk:
                return False
            request += chunk
            if len(request) > 16384:
                return False
        headers = {}
        for line in request.decode("latin-1").split("\r\n")[1:]:
            if ": " in line:
                k, v = line.split(": ", 1)
                headers[k.lower()] = v
        key = headers.get("sec-websocket-key")
        if not key or headers.get("upgrade", "").lower() != "websocket":
            # Not a WebSocket upgrade; reject plainly.
            conn.sendall(b"HTTP/1.1 400 Bad Request\r\n\r\n")
            return False
        response = (
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: %s\r\n\r\n" % _accept_key(key)
        )
        conn.sendall(response.encode("ascii"))
        conn.settimeout(None)
        return True

    def _recv_exact(self, conn, n):
        buf = b""
        while len(buf) < n:
            chunk = conn.recv(n - len(buf))
            if not chunk:
                return None
            buf += chunk
        return buf

    def _recv_loop(self, client):
        conn = client.conn
        while self._running and client.open:
            header = self._recv_exact(conn, 2)
            if header is None:
                break
            b0, b1 = header[0], header[1]
            opcode = b0 & 0x0F
            masked = (b1 & 0x80) != 0
            length = b1 & 0x7F
            if length == 126:
                ext = self._recv_exact(conn, 2)
                if ext is None:
                    break
                length = struct.unpack(">H", ext)[0]
            elif length == 127:
                ext = self._recv_exact(conn, 8)
                if ext is None:
                    break
                length = struct.unpack(">Q", ext)[0]

            mask = b"\x00\x00\x00\x00"
            if masked:
                mask = self._recv_exact(conn, 4)
                if mask is None:
                    break

            payload = b""
            if length:
                payload = self._recv_exact(conn, length)
                if payload is None:
                    break
                if masked:
                    payload = bytes(payload[i] ^ mask[i % 4] for i in range(len(payload)))

            if opcode == _OP_CLOSE:
                break
            if opcode == _OP_PING:
                # reply pong
                try:
                    with client._send_lock:
                        frame = bytes([0x80 | _OP_PONG, len(payload)]) + payload
                        conn.sendall(frame)
                except OSError:
                    break
                continue
            if opcode in (_OP_TEXT, _OP_CONT):
                if self.on_message:
                    try:
                        self.on_message(client, payload.decode("utf-8", "replace"))
                    except Exception as exc:  # never let one message kill the client loop
                        self._log("warn", "on_message error: %s" % exc)
            # ignore binary/pong frames for this protocol
