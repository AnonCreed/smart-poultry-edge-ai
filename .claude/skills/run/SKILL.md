---
description: Launch the poultry-telemetry Django dev server (and optionally the sensor simulator) for this project. Use whenever asked to run/start/fire up the local server or dashboard.
---

# Running poultry_telemetry locally

Two independent pieces: the Django backend/dashboard, and (optionally) a
virtual sensor that feeds it fake data.

## Backend + dashboard

```bash
source .venv/bin/activate
python manage.py migrate          # no-op if already up to date
python manage.py runserver 0.0.0.0:8000
```

**Always bind `0.0.0.0`, never the default `127.0.0.1`.** The physical
ESP32-S3 master node reaches this server over WiFi and finds the LAN IP
itself via UDP broadcast discovery (see
`hardware/esp32s3_master/README.md`) — there is no IP hardcoded on either
side. If the server only listens on loopback, the dashboard sits on
"Awaiting data" forever even while the hardware is actively transmitting,
because every POST to `/api/telemetry/submit/` never reaches the socket.
This already burned once in this project — don't repeat it.

`ALLOWED_HOSTS = ["*"]` in `config/settings.py` is intentional for the same
reason (dev-only server; never expose beyond a trusted LAN).

Run it in the background and verify both addresses answer:

```bash
nohup python manage.py runserver 0.0.0.0:8000 > /tmp/.../runserver.log 2>&1 &
disown
curl -s -o /dev/null -w "loopback: HTTP %{http_code}\n" http://127.0.0.1:8000/
curl -s -o /dev/null -w "LAN IP:   HTTP %{http_code}\n" http://$(hostname -I | awk '{print $1}'):8000/
```

If a firewall is active on this machine, port 8000/tcp must be allowed for
LAN traffic too, or hardware still won't get through even though the app
itself is listening correctly.

Dashboard: `http://127.0.0.1:8000/` (this machine) or
`http://<LAN-IP>:8000/` (phone/other device on the same WiFi).

## Virtual sensor (no hardware needed)

```bash
python sensor_simulator.py                # edge-AI forecast fields populated
python sensor_simulator.py --no-edge-ai   # nulls, matches pre-ESP32S3 deployment state
```

Transmits synthetic readings every 5s directly to `127.0.0.1:8000` (same
machine), so it works regardless of the bind address above.

## Sanity check data is actually landing

The dashboard reads a *trailing time window* (default 24h). Stale data
older than the window silently renders as "no data" even though rows
exist in the DB — don't assume the DB is empty just because the chart is:

```bash
python manage.py shell -c "
from telemetry.models import PoultryTelemetry
from django.utils import timezone
print('latest:', PoultryTelemetry.objects.order_by('-timestamp').first().timestamp)
print('now:   ', timezone.now())
"
```
