"""
UDP discovery responder.

Lets the ESP32-S3 master node find this Django instance's LAN IP without a
hardcoded address baked into firmware -- the address that changes every time
the board or this PC joins a different WiFi network.

Protocol (mirrored in hardware/esp32s3_master/src/main.cpp discoverDjangoHost()):
  - The master node broadcasts REQUEST_MAGIC to <subnet-broadcast>:DISCOVERY_PORT.
  - This responder listens on 0.0.0.0:DISCOVERY_PORT. On any packet whose
    payload matches REQUEST_MAGIC, it unicasts RESPONSE_MAGIC back to the
    sender.
  - The master node reads the reply's *source IP* -- not the payload -- as
    the Django host to POST telemetry to. The payload only needs to prove
    "a poultry-telemetry Django instance answered this", so a spoofed/stray
    UDP packet from something else on the LAN can't be mistaken for it.
"""
import logging
import socket
import threading

logger = logging.getLogger(__name__)

DISCOVERY_PORT = 42424
REQUEST_MAGIC = b"POULTRY_TELEMETRY_DISCOVER_V1"
RESPONSE_MAGIC = b"POULTRY_TELEMETRY_HERE_V1"

_started = False
_lock = threading.Lock()


def _serve() -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.bind(("0.0.0.0", DISCOVERY_PORT))
    except OSError:
        logger.warning(
            "Discovery responder: UDP port %d unavailable -- skipping "
            "(another instance already running?).", DISCOVERY_PORT,
        )
        return

    logger.info("Discovery responder listening on UDP :%d", DISCOVERY_PORT)
    while True:
        try:
            data, addr = sock.recvfrom(256)
        except OSError:
            break
        if data.strip() == REQUEST_MAGIC:
            sock.sendto(RESPONSE_MAGIC, addr)
            logger.info("Discovery request from %s -- replied.", addr[0])


def start() -> None:
    """Idempotently start the UDP discovery responder in a background thread."""
    global _started
    with _lock:
        if _started:
            return
        _started = True
    threading.Thread(target=_serve, name="poultry-discovery", daemon=True).start()
