

import base64
import signal
import socket
import sys
import threading
import time
import zmq


# =============================================================================
# Configuration
# =============================================================================

# UDP ports
ARDUPILOT_LISTEN_PORT = 14560   # Bridge binds here, ArduPilot sends to this
QGC_TARGET_HOST = "127.0.0.1"
QGC_TARGET_PORT = 14550         # QGC default listen port

# ZMQ ports (must match uav-net-sim.cc)
ZMQ_PUB_TELEMETRY_PORT = 5600  # Bridge PUB -> ns-3 SUB (telemetry in)
ZMQ_PUB_COMMANDS_PORT = 5500   # Bridge PUB -> ns-3 SUB (commands in)
ZMQ_SUB_TELEMETRY_PORT = 5501  # ns-3 PUB -> Bridge SUB (telemetry out)
ZMQ_SUB_COMMANDS_PORT = 5601   # ns-3 PUB -> Bridge SUB (commands out)

# ZMQ message prefixes (must match C++ expectations)
UAV_PREFIX = "@@@U_000***"      # Telemetry: drone -> GCS
GCS_PREFIX = "@@@G_000***"      # Commands:  GCS -> drone
MSG_SUFFIX = "***"              # Trailing delimiter for clean token parsing

# Logging interval
STATS_INTERVAL = 10  # Print stats every N seconds

# Rate limiting for Thread A (ArduPilot -> ns-3): high packet rates from MAVProxy can
# crash ns-3's MyApp::SendMsg via rapid concurrent Simulator::Schedule calls from the
# ZMQ pthread.
RATE_LIMIT_SEC = 0.0025  # 1 packet per 2.5ms = 400 packets/sec

# UDP buffer size (MAVLink max is 280 bytes, but be generous)
UDP_BUFFER_SIZE = 4096


# =============================================================================
# Shared state
# =============================================================================

# ArduPilot's return address — captured from first recvfrom on Socket A.
# Thread A writes this once, Thread D reads it. Protected by a lock.
ardupilot_addr = None
ardupilot_addr_lock = threading.Lock()

# Packet counters for logging
stats = {
    "a_ardupilot_to_ns3": 0,
    "b_ns3_to_qgc": 0,
    "c_qgc_to_ns3": 0,
    "d_ns3_to_ardupilot": 0,
    "d_dropped_no_addr": 0,
    "decode_errors": 0,
}
stats_lock = threading.Lock()

# Shutdown flag
shutdown_event = threading.Event()

# Diagnostic instrumentation: parse the MAVLink stream headed to QGC to measure how many
# of the ~1300 parameters arrive intact and how many messages fail CRC.
import os as _os
_os.environ.setdefault("MAVLINK20", "1")
_os.environ.setdefault("MAVLINK_DIALECT", "ardupilotmega")
try:
    from pymavlink import mavutil as _mavutil
    _mav_parser = _mavutil.mavlink.MAVLink(None)
    _mav_parser.robust_parsing = True   # skip (and count) bad frames instead of raising
    _MEASURE_OK = True
except Exception as _e:
    _MEASURE_OK = False
    print("[MEASURE] pymavlink unavailable, measurement disabled: %s" % _e)
_measure = {"parsed": 0, "param_count": 0, "first_t": None, "last_t": None, "seen": set()}


# =============================================================================
# Helper functions
# =============================================================================

def encode_mavlink(prefix, raw_bytes):
    """Wrap raw MAVLink bytes in the ZMQ message format ns-3 expects.

    Format: @@@U_000***<base64>***  or  @@@G_000***<base64>***
    """
    b64 = base64.b64encode(raw_bytes).decode("ascii")
    return prefix + b64 + MSG_SUFFIX


def decode_ns3_message(zmq_bytes):
    """Extract raw MAVLink bytes from an ns-3 ZMQ message.

    Input:  b"@@@U_000***<base64>***<ingress>***<egress>***"
    Output: raw MAVLink bytes

    Splits on '***' and takes token[1] (the base64 payload).
    """
    msg_str = zmq_bytes.decode("ascii", errors="replace")
    tokens = msg_str.split("***")
    # tokens[0] = "@@@U_000" or "@@@G_000"
    # tokens[1] = base64 payload
    # tokens[2+] = timestamps from ns-3 (discard)
    if len(tokens) < 2 or not tokens[1]:
        return None
    try:
        return base64.b64decode(tokens[1])
    except Exception:
        return None


# ns-3's TCP can coalesce several wrapped messages into one blob
# ("@@@U_000***b64A***...@@@U_000***b64B***..."). Unlike decode_ns3_message(), which keeps
# only the first message, this returns EVERY message in the blob.
def decode_ns3_messages(zmq_bytes):
    """Extract ALL raw MAVLink messages from an ns-3 ZMQ blob (handles coalescing).

    Returns a list of raw MAVLink byte strings (possibly empty).
    """
    msg_str = zmq_bytes.decode("ascii", errors="replace")
    out = []
    # Each message starts with the "@@@" header; base64 never contains '@', so
    # splitting on "@@@" cleanly separates coalesced messages.
    for chunk in msg_str.split("@@@"):
        if not chunk:
            continue
        tokens = chunk.split("***")
        # tokens[0] = "U_000"/"G_000", tokens[1] = base64 payload, rest = ns-3 timestamps
        if len(tokens) < 2 or not tokens[1]:
            continue
        try:
            out.append(base64.b64decode(tokens[1]))
        except Exception:
            continue
    return out


def inc_stat(key):
    """Thread-safe counter increment."""
    with stats_lock:
        stats[key] += 1


# =============================================================================
# Thread A: ArduPilot -> ns-3 (telemetry inbound)
#   READ  UDP 14560 (from ArduPilot)
#   WRITE ZMQ PUB 5600 (to ns-3 rcvTelemetry)
# =============================================================================

def thread_a_ardupilot_to_ns3(udp_sock_a, zmq_pub_telemetry):
    time.sleep(3)  # wait for ns-3 internal TCP handshake
    """Receive MAVLink from ArduPilot, encode, publish to ns-3."""
    global ardupilot_addr

    print("[Thread A] Listening for ArduPilot on UDP 14560...")

    while not shutdown_event.is_set():
        try:
            data, addr = udp_sock_a.recvfrom(UDP_BUFFER_SIZE)
        except socket.timeout:
            continue
        except OSError:
            break

        # Capture ArduPilot's return address on first packet
        with ardupilot_addr_lock:
            if ardupilot_addr is None:
                ardupilot_addr = addr
                print("[Thread A] ArduPilot address captured: {}:{}".format(
                    addr[0], addr[1]))

        # Encode and publish to ns-3
        zmq_msg = encode_mavlink(UAV_PREFIX, data)
        zmq_pub_telemetry.send_string(zmq_msg)
        inc_stat("a_ardupilot_to_ns3")

        # Rate limit: prevent flooding ns-3's event scheduler
        time.sleep(RATE_LIMIT_SEC)

    print("[Thread A] Stopped.")


# =============================================================================
# Thread B: ns-3 -> QGC (telemetry outbound)
#   READ  ZMQ SUB 5501 (from ns-3 publisherTm)
#   WRITE UDP -> 127.0.0.1:14550 (to QGC)
# =============================================================================

def thread_b_ns3_to_qgc(zmq_sub_telemetry, udp_sock_b):
    """Receive telemetry from ns-3, decode, forward to QGC."""

    print("[Thread B] Waiting for telemetry from ns-3 on ZMQ 5501...")

    while not shutdown_event.is_set():
        try:
            # Use poll so we can check shutdown_event periodically
            if zmq_sub_telemetry.poll(timeout=1000):
                zmq_bytes = zmq_sub_telemetry.recv()
            else:
                continue
        except zmq.ZMQError:
            break

        # Forward every message in the (possibly coalesced) blob.
        raw_list = decode_ns3_messages(zmq_bytes)
        if not raw_list:
            inc_stat("decode_errors")
            continue
        for raw_mavlink in raw_list:
            udp_sock_b.sendto(raw_mavlink, (QGC_TARGET_HOST, QGC_TARGET_PORT))
            inc_stat("b_ns3_to_qgc")
        # Diagnostic: parse the exact bytes sent to QGC and count CRC/parse failures.
        if _MEASURE_OK:
            for raw_mavlink in raw_list:
                try:
                    msgs = _mav_parser.parse_buffer(raw_mavlink) or []
                except Exception:
                    msgs = []
                for m in msgs:
                    _measure["parsed"] += 1
                    if m.get_type() == "PARAM_VALUE":
                        _measure["param_count"] = m.param_count
                        if m.param_index < m.param_count:
                            if _measure["first_t"] is None:
                                _measure["first_t"] = time.time()
                            _measure["last_t"] = time.time()
                            _measure["seen"].add(m.param_index)

    print("[Thread B] Stopped.")


# =============================================================================
# Thread C: QGC -> ns-3 (commands inbound)
#   READ  UDP from QGC (on Socket B, same socket Thread B writes to)
#   WRITE ZMQ PUB 5500 (to ns-3 rcvCommands)
# =============================================================================

def thread_c_qgc_to_ns3(udp_sock_b, zmq_pub_commands):
    """Receive commands from QGC, encode, publish to ns-3."""

    print("[Thread C] Listening for QGC commands on Socket B...")

    while not shutdown_event.is_set():
        try:
            data, addr = udp_sock_b.recvfrom(UDP_BUFFER_SIZE)
        except socket.timeout:
            continue
        except OSError:
            break

        # Encode and publish to ns-3
        zmq_msg = encode_mavlink(GCS_PREFIX, data)
        zmq_pub_commands.send_string(zmq_msg)
        inc_stat("c_qgc_to_ns3")

        # Rate limit: same protection as Thread A
        time.sleep(RATE_LIMIT_SEC)

    print("[Thread C] Stopped.")


# =============================================================================
# Thread D: ns-3 -> ArduPilot (commands outbound)
#   READ  ZMQ SUB 5601 (from ns-3 publisherCm)
#   WRITE UDP -> ArduPilot (dynamic address from Thread A)
# =============================================================================

def thread_d_ns3_to_ardupilot(zmq_sub_commands, udp_sock_a):
    """Receive commands from ns-3, decode, forward to ArduPilot."""

    print("[Thread D] Waiting for commands from ns-3 on ZMQ 5601...")

    while not shutdown_event.is_set():
        try:
            if zmq_sub_commands.poll(timeout=1000):
                zmq_bytes = zmq_sub_commands.recv()
            else:
                continue
        except zmq.ZMQError:
            break

        raw_mavlink = decode_ns3_message(zmq_bytes)
        if raw_mavlink is None:
            inc_stat("decode_errors")
            continue

        with ardupilot_addr_lock:
            target = ardupilot_addr

        if target is None:
            # ArduPilot hasn't sent anything yet — drop silently
            inc_stat("d_dropped_no_addr")
            continue

        udp_sock_a.sendto(raw_mavlink, target)
        inc_stat("d_ns3_to_ardupilot")

    print("[Thread D] Stopped.")


# =============================================================================
# Stats printer
# =============================================================================

def stats_printer():
    """Print packet counters every STATS_INTERVAL seconds."""
    while not shutdown_event.is_set():
        shutdown_event.wait(STATS_INTERVAL)
        if shutdown_event.is_set():
            break
        with stats_lock:
            s = dict(stats)
        print("\n[STATS] "
              "A(AP->ns3): {a_ardupilot_to_ns3}  "
              "B(ns3->QGC): {b_ns3_to_qgc}  "
              "C(QGC->ns3): {c_qgc_to_ns3}  "
              "D(ns3->AP): {d_ns3_to_ardupilot}  "
              "D(dropped): {d_dropped_no_addr}  "
              "DecodeErr: {decode_errors}".format(**s))
        # Diagnostic: report parameter-arrival and CRC-error counts.
        if _MEASURE_OK:
            seen = len(_measure["seen"])
            total = _measure["param_count"]
            dur = (_measure["last_t"] - _measure["first_t"]) if _measure["first_t"] else 0.0
            print("[MEASURE] params %d/%d unique in %.1fs | MAVLink parsed=%d  CRC/parse-errors=%d"
                  % (seen, total, dur, _measure["parsed"], _mav_parser.total_receive_errors))


# =============================================================================
# Main
# =============================================================================

def main():
    print("=" * 60)
    print("  FlyNetSim MAVLink-ZMQ Bridge")
    print("=" * 60)

    # ------------------------------------------------------------------
    # Step 1: Create ZMQ sockets
    # ------------------------------------------------------------------
    zmq_context = zmq.Context()

    # PUB sockets — bridge BINDS, ns-3 connects
    zmq_pub_telemetry = zmq_context.socket(zmq.PUB)
    zmq_pub_telemetry.bind("tcp://*:{}".format(ZMQ_PUB_TELEMETRY_PORT))
    print("[ZMQ] PUB bound on tcp://*:{} (telemetry -> ns-3)".format(
        ZMQ_PUB_TELEMETRY_PORT))

    zmq_pub_commands = zmq_context.socket(zmq.PUB)
    zmq_pub_commands.bind("tcp://*:{}".format(ZMQ_PUB_COMMANDS_PORT))
    print("[ZMQ] PUB bound on tcp://*:{} (commands -> ns-3)".format(
        ZMQ_PUB_COMMANDS_PORT))

    # SUB sockets — bridge CONNECTS, ns-3 binds
    zmq_sub_telemetry = zmq_context.socket(zmq.SUB)
    zmq_sub_telemetry.connect("tcp://localhost:{}".format(
        ZMQ_SUB_TELEMETRY_PORT))
    zmq_sub_telemetry.setsockopt_string(zmq.SUBSCRIBE, "")
    print("[ZMQ] SUB connected to tcp://localhost:{} (telemetry <- ns-3)".format(
        ZMQ_SUB_TELEMETRY_PORT))

    zmq_sub_commands = zmq_context.socket(zmq.SUB)
    zmq_sub_commands.connect("tcp://localhost:{}".format(
        ZMQ_SUB_COMMANDS_PORT))
    zmq_sub_commands.setsockopt_string(zmq.SUBSCRIBE, "")
    print("[ZMQ] SUB connected to tcp://localhost:{} (commands <- ns-3)".format(
        ZMQ_SUB_COMMANDS_PORT))

    # ------------------------------------------------------------------
    # Step 2: Create UDP sockets
    # ------------------------------------------------------------------

    # Socket A — ArduPilot side (bind to 14560)
    udp_sock_a = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_sock_a.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    udp_sock_a.bind(("127.0.0.1", ARDUPILOT_LISTEN_PORT))
    udp_sock_a.settimeout(1.0)  # Allow periodic shutdown checks
    print("[UDP] Socket A bound on 127.0.0.1:{} (ArduPilot side)".format(
        ARDUPILOT_LISTEN_PORT))

    # Socket B — QGC side (unbound, OS assigns ephemeral port)
    # QGC replies to whatever source port it sees in incoming packets.
    udp_sock_b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_sock_b.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    udp_sock_b.settimeout(1.0)
    print("[UDP] Socket B created (ephemeral port, QGC side)")

    # ------------------------------------------------------------------
    # Step 3: All sockets open — bridge is ready
    # ------------------------------------------------------------------
    print("")
    print("=" * 60)
    print("  Bridge ready — all sockets open")
    print("  Waiting for ArduPilot on UDP {}".format(ARDUPILOT_LISTEN_PORT))
    print("  Will forward telemetry to QGC on UDP {}".format(QGC_TARGET_PORT))
    print("=" * 60)
    print("")

    # ------------------------------------------------------------------
    # Step 4: Launch threads
    # ------------------------------------------------------------------
    threads = [
        threading.Thread(
            target=thread_a_ardupilot_to_ns3,
            args=(udp_sock_a, zmq_pub_telemetry),
            name="Thread-A-AP-to-ns3",
            daemon=True,
        ),
        threading.Thread(
            target=thread_b_ns3_to_qgc,
            args=(zmq_sub_telemetry, udp_sock_b),
            name="Thread-B-ns3-to-QGC",
            daemon=True,
        ),
        threading.Thread(
            target=thread_c_qgc_to_ns3,
            args=(udp_sock_b, zmq_pub_commands),
            name="Thread-C-QGC-to-ns3",
            daemon=True,
        ),
        threading.Thread(
            target=thread_d_ns3_to_ardupilot,
            args=(zmq_sub_commands, udp_sock_a),
            name="Thread-D-ns3-to-AP",
            daemon=True,
        ),
        threading.Thread(
            target=stats_printer,
            name="Thread-Stats",
            daemon=True,
        ),
    ]

    for t in threads:
        t.start()
        print("[Main] Started {}".format(t.name))

    # ------------------------------------------------------------------
    # Step 5: Wait for shutdown signal
    # ------------------------------------------------------------------
    def signal_handler(sig, frame):
        print("\n[Main] Caught signal {}, shutting down...".format(sig))
        shutdown_event.set()

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    # Block main thread until shutdown
    try:
        while not shutdown_event.is_set():
            shutdown_event.wait(1.0)
    except KeyboardInterrupt:
        shutdown_event.set()

    # ------------------------------------------------------------------
    # Step 6: Cleanup
    # ------------------------------------------------------------------
    print("\n[Main] Closing sockets...")
    udp_sock_a.close()
    udp_sock_b.close()
    zmq_pub_telemetry.close()
    zmq_pub_commands.close()
    zmq_sub_telemetry.close()
    zmq_sub_commands.close()
    zmq_context.term()

    # Print final stats
    with stats_lock:
        s = dict(stats)
    print("\n[FINAL STATS] "
          "A(AP->ns3): {a_ardupilot_to_ns3}  "
          "B(ns3->QGC): {b_ns3_to_qgc}  "
          "C(QGC->ns3): {c_qgc_to_ns3}  "
          "D(ns3->AP): {d_ns3_to_ardupilot}  "
          "D(dropped): {d_dropped_no_addr}  "
          "DecodeErr: {decode_errors}".format(**s))

    print("[Main] Bridge stopped.")


if __name__ == "__main__":
    main()
