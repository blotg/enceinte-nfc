#!/usr/bin/env python3
"""
API simulée de l'enceinte pour développer et tester l'interface web sur PC.

  python3 test/web/mock_api.py [--setup] [--port 8080]
puis ouvrir http://127.0.0.1:8080/

--setup : sortie d'usine (assistant de première configuration).
--ap : Wi-Fi de secours (point d'accès) alors que le réseau de la maison est de nouveau à portée.
"""
import argparse
import json
import os
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

WEB = os.path.join(os.path.dirname(__file__), "..", "..", "main", "web")
BUILD = "simulation"  # identifiant du firmware : le changer pendant que la page est ouverte la recharge

STATE = {"setup": False, "logged": True, "volume": 35, "playing": "play", "learning": False, "learn_at": 0,
         "resume_s": 600, "resume_after_other": False, "shuffle": False, "normalize": 2, "compress": 2,
         "max_volume": 80, "ap": False, "ip": {"mode": "dhcp", "address": "", "netmask": "", "gateway": "", "dns": ""},
         "ip_test_until": 0, "ip_test_address": "", "vol_touch": True, "touch_threshold_pct": 2.0,
         "touch_hold_ms": 800, "repeat": False, "stream": False}

FILES = {
    "": [("Comptines", True, 0), ("Histoires du soir", True, 0), ("Musique classique", True, 0),
         ("Podcasts", True, 0), ("Webradios", True, 0)],
    "Podcasts": [("Les Odyssées", True, 0)],
    "Podcasts/Les Odyssées": [("2026-10-01 06h00 - Le voyage d'Ulysse.mp3", False, 21_000_000),
                              ("2026-10-08 06h00 - Le cheval de Troie.mp3", False, 23_500_000)],
    "Webradios": [("France Inter", True, 0)],
    "Webradios/France Inter": [("webradio.m3u", False, 96)],
    "Comptines": [("01 - Une souris verte.mp3", False, 3_412_000), ("02 - Frère Jacques.mp3", False, 2_904_112),
                  ("10 - Il était un petit navire.mp3", False, 4_800_000), ("cover.jpg", False, 85_000)],
    "Histoires du soir": [("Contes", True, 0)],
    "Histoires du soir/Contes": [],
    "Musique classique": [],
}


RADIOS = {"Webradios/France Inter": "http://icecast.radiofrance.fr/franceinter-midfi.mp3"}
PODCASTS = {"Podcasts/Les Odyssées": {"url": "https://radiofrance-podcast.net/podcast09/rss_19721.xml",
                                      "title": "Les Odyssées", "keep": 10, "last_check": time.time() - 3600,
                                      "last_error": "", "sync_until": 0}}
LOG = []


def kind(path):
    return "podcast" if path in PODCASTS else "radio" if path in RADIOS else "folder"


def log(msg):
    LOG.append(f"I ({int(time.monotonic() * 1000)}) simulation: {msg}\n")


def unique(base, name):
    folder, i = f"{base}/{name}", 2
    while folder in FILES:
        folder, i = f"{base}/{name} ({i})", i + 1
    return folder


def create_folder(folder, files=()):
    parent, name = split(folder)
    if parent not in FILES:
        create_folder(parent)
    FILES[parent].append((name, True, 0))
    FILES[folder] = list(files)


def attach(card, folder):
    if isinstance(card, dict):
        card = dict(card, folder=folder, exists=True, kind=kind(folder))
        CARDS[:] = [c for c in CARDS if c["uid"] != card["uid"]] + [card]


def split(path):
    return (path.rsplit("/", 1) if "/" in path else ("", path))


def move(src, dst):
    sparent, sname = split(src)
    dparent, dname = split(dst)
    if dparent not in FILES:
        return "dossier de destination introuvable"
    if any(n == dname for n, _, _ in FILES[dparent]):
        return "ce nom est déjà utilisé à cet endroit"
    entry = next((e for e in FILES.get(sparent, []) if e[0] == sname), None)
    if not entry:
        return "introuvable"
    FILES[sparent].remove(entry)
    FILES[dparent].append((dname, entry[1], entry[2]))
    for key in [k for k in FILES if k == src or k.startswith(src + "/")]:
        FILES[dst + key[len(src):]] = FILES.pop(key)
    return None

CARDS = [
    {"uid": "04A1B2C3D4E5F6", "folder": "Comptines", "exists": True, "resume_s": None, "resume_other": None,
     "shuffle": None, "normalize": None, "compress": None, "sleep_tracks": 3, "sleep_minutes": 0},
    {"uid": "0411223344", "folder": "Histoires du soir", "exists": True, "resume_s": 0, "resume_other": True,
     "shuffle": True, "normalize": 3, "compress": 2, "sleep_tracks": 0, "sleep_minutes": 45},
    {"uid": "04BADA55", "folder": "Comptines", "exists": True, "resume_s": 300, "resume_other": None,
     "shuffle": False, "normalize": None, "compress": None, "sleep_tracks": 0, "sleep_minutes": 0},
    {"uid": "04DEADBEEF", "folder": "Ancien dossier", "exists": False, "resume_s": None, "resume_other": None,
     "shuffle": None, "normalize": None, "compress": None, "sleep_tracks": 0, "sleep_minutes": 0},
    {"uid": "04F00D0001", "folder": "Webradios/France Inter", "exists": True, "resume_s": None, "resume_other": None,
     "shuffle": None, "repeat": None, "normalize": None, "compress": None, "sleep_tracks": 0, "sleep_minutes": 30},
    {"uid": "04F00D0002", "folder": "Podcasts/Les Odyssées", "exists": True, "resume_s": 0, "resume_other": True,
     "shuffle": None, "repeat": None, "normalize": None, "compress": None, "sleep_tracks": 0, "sleep_minutes": 0},
]


def ip_test_remaining():
    return max(0, int(STATE["ip_test_until"] - time.time()))


def status():
    ap = STATE["ap"]
    if STATE["stream"]:
        player = {"state": "play", "file": RADIOS["Webradios/France Inter"], "title": "Le 7/9 - Journal de 8 h",
                  "artist": "", "album": "", "elapsed": time.time() % 3600, "duration": 0, "seekable": False,
                  "stream": True, "song": 0, "queue_len": 1, "volume": STATE["volume"],
                  "max_volume": STATE["max_volume"], "repeat": False, "random": False, "error": "",
                  "normalize": STATE["normalize"], "compress": STATE["compress"], "sleep_tracks": 0, "sleep_s": 0,
                  "sleep_done": False}
        st = status_base(ap)
        st["player"] = player
        st["card"]["folder"] = "Webradios/France Inter"
        st["card"]["present"] = st["card"]["session"] = "04F00D0001"
        return st
    return status_base(ap)


def status_base(ap):
    return {
        "player": {"state": STATE["playing"], "file": "Comptines/02 - Frère Jacques.mp3", "title": "Frère Jacques",
                   "artist": "Chorale des enfants", "album": "Comptines", "elapsed": (time.time() % 150),
                   "duration": 150.0, "seekable": True, "song": 1, "queue_len": 3, "volume": STATE["volume"],
                   "max_volume": STATE["max_volume"], "repeat": False, "random": False, "error": "",
                   "normalize": STATE["normalize"], "compress": STATE["compress"],
                   "sleep_tracks": 2, "sleep_s": 0, "sleep_done": False, "stream": False},
        "card": {"reader_ok": True, "reader": "PN5180, firmware 4.0", "present": "04A1B2C3D4E5F6", "session": "04A1B2C3D4E5F6",
                 "folder": "Comptines", "resume_remaining": 0, "last_unknown": ""},
        "wifi": {"connected": not ap, "ssid": "Maison", "ip": "" if ap else "192.168.1.42", "rssi": -58, "ap": ap,
                 "ap_ssid": "Enceinte-3F2A", "hostname": "enceinte", "sta_available": ap, "on_ap": ap,
                 "ip_test_remaining": ip_test_remaining(), "ip_test_address": STATE["ip_test_address"]},
        "sd": {"mounted": True, "total": 15_931_539_456, "free": 12_002_000_000},
        "ota": {"state": 0, "message": "à jour (dernière version : 1.0.0)", "current": "1.0.0", "available": "",
                "progress": 0, "last_check": time.time()},
        "uptime": 7322, "heap": 152000, "build": BUILD,
    }


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def send_json(self, obj, code=200):
        data = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def static(self, name, ctype, subst=None):
        with open(os.path.join(WEB, name), "rb") as f:
            data = f.read()
        for k, v in (subst or {}).items():
            data = data.replace(k.encode(), v.encode())
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b""
        try:
            return json.loads(raw or b"{}")
        except ValueError:
            return {}

    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        if u.path in ("/", "/index.html"):
            return self.static("index.html", "text/html; charset=utf-8", {"{{build}}": BUILD})
        if u.path == "/app.js":
            return self.static("app.js", "application/javascript")
        if u.path == "/style.css":
            return self.static("style.css", "text/css")
        if u.path == "/api/state":
            return self.send_json({"setup_required": STATE["setup"], "logged_in": STATE["logged"],
                                   "version": "1.0.0", "build": BUILD, "hostname": "enceinte",
                                   "on_ap": STATE["setup"]})
        if u.path == "/api/wifi/scan":
            time.sleep(0.3)
            return self.send_json({"networks": [{"ssid": "Maison", "rssi": -48, "secure": True},
                                                {"ssid": "Voisin", "rssi": -80, "secure": True},
                                                {"ssid": "Café", "rssi": -66, "secure": False}]})
        if not STATE["logged"]:
            return self.send_json({"error": "connexion requise"}, 401)
        if u.path == "/api/status":
            return self.send_json(status())
        if u.path == "/api/logs":
            since = int(q.get("since", ["0"])[0])
            text = "".join(LOG)
            reset = since > len(text)
            return self.send_json({"next": len(text), "reset": reset, "text": text if reset else text[since:],
                                   "uptime_ms": int(time.monotonic() * 1000), "time": int(time.time()),
                                   "memory": {"internal_free": 61_000, "internal_largest": 31_000,
                                              "internal_min": 42_000, "psram_free": 7_800_000,
                                              "psram_total": 8_388_608}})
        if u.path == "/api/radio":
            folder = q.get("folder", [""])[0]
            if folder not in RADIOS:
                return self.send_json({"error": "pas une webradio"}, 404)
            return self.send_json({"folder": folder, "name": split(folder)[1], "url": RADIOS[folder]})
        if u.path == "/api/podcast":
            folder = q.get("folder", [""])[0]
            pc = PODCASTS.get(folder)
            if not pc:
                return self.send_json({"error": "pas un podcast"}, 404)
            syncing = time.time() < pc["sync_until"]
            if not syncing and pc["sync_until"]:
                pc["sync_until"], pc["last_check"] = 0, time.time()
                FILES[folder].append(("2026-10-15 06h00 - Le retour à Ithaque.mp3", False, 22_000_000))
            eps = len([f for f in FILES.get(folder, []) if not f[1] and f[0].endswith(".mp3")])
            return self.send_json({"folder": folder, "url": pc["url"], "title": pc["title"], "keep": pc["keep"],
                                   "episodes": eps, "last_check": pc["last_check"], "last_error": pc["last_error"],
                                   "syncing": syncing, "progress": int((time.time() * 20) % 100) if syncing else -1})
        if u.path == "/api/cards":
            for c in CARDS:
                c["kind"] = kind(c["folder"])
            learned = "04C0FFEE11" if STATE["learning"] and time.time() - STATE["learn_at"] > 2 else ""
            learning = STATE["learning"] and not learned
            return self.send_json({"cards": CARDS, "learning": learning, "learn_remaining": 55, "learned": learned,
                                   "present": "04A1B2C3D4E5F6", "last_unknown": "", "reader_ok": True})
        if u.path == "/api/files":
            path = q.get("path", [""])[0]
            entries = [{"name": n, "dir": d, "size": s, "audio": n.endswith(".mp3"),
                        **({"kind": kind(f"{path}/{n}" if path else n)} if d else {})}
                       for n, d, s in FILES.get(path, [])]
            return self.send_json({"path": path, "kind": kind(path), "entries": entries, "total": 15_931_539_456,
                                   "free": 12_002_000_000})
        if u.path == "/api/touch":  # doigt simulé sur « + » une seconde sur trois
            plus = 4.6 if int(time.time()) % 3 == 0 else 0.2
            thr = STATE["touch_threshold_pct"]
            keys = [{"name": "+", "delta_pct": plus, "value": int(30000 * (1 + plus / 100)), "baseline": 30000,
                     "touched": plus > thr},
                    {"name": "-", "delta_pct": 0.1, "value": 29800, "baseline": 29770, "touched": False}]
            return self.send_json({"touch": STATE["vol_touch"], "ok": STATE["vol_touch"], "threshold_pct": thr,
                                   "keys": keys})
        if u.path == "/api/config/export":
            doc = {"format": "enceinte-reglages", "version": 1, "device": "A0B1C2D3E4F5",
                   "settings": {"hostname": "enceinte", "wifi": {"ssid": "Maison"}, "ip": STATE["ip"],
                                "max_volume": STATE["max_volume"], "resume_s": STATE["resume_s"],
                                "resume_after_other": STATE["resume_after_other"], "shuffle": STATE["shuffle"],
                                "normalize": STATE["normalize"], "compress": STATE["compress"], "https": False,
                                "ota_url": "", "ota_interval_h": 24},
                   "cards": [{k: c[k] for k in ("uid", "folder", "resume_s", "resume_other", "shuffle", "normalize",
                                                 "compress")} for c in CARDS]}
            if q.get("secrets", [""])[0] == "1":
                doc["settings"]["wifi"]["password"] = "motdepasse"
                doc["settings"]["admin_password_hash"] = "ECcAAA=="
            data = json.dumps(doc, indent=2, ensure_ascii=False).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Disposition", 'attachment; filename="reglages-enceinte.json"')
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        if u.path == "/api/settings":
            return self.send_json({"hostname": "enceinte", "wifi_ssid": "Maison", "ota_url": "",
                                   "ota_default_url": "https://github.com/blotg/enceinte-nfc",
                                   "ota_interval_h": 24, "max_volume": STATE["max_volume"], "mpd_password_set": False,
                                   "normalize": STATE["normalize"], "compress": STATE["compress"], "ip": STATE["ip"],
                                   "ip_current": {"address": "192.168.1.42", "netmask": "255.255.255.0",
                                                  "gateway": "192.168.1.1", "dns": "192.168.1.1"},
                                   "ip_test_s": 300, "vol_touch": STATE["vol_touch"],
                                   "touch_threshold_pct": STATE["touch_threshold_pct"],
                                   "touch_hold_ms": STATE["touch_hold_ms"],
                                   "resume_s": STATE["resume_s"], "resume_after_other": STATE["resume_after_other"],
                                   "shuffle": STATE["shuffle"], "repeat": STATE["repeat"],
                                   "https_enabled": STATE.get("https", False), "https_active": STATE.get("https", False),
                                   "https_pending": False,
                                   "mpd_port": 6600})
        self.send_json({"error": "introuvable"}, 404)

    def do_POST(self):
        if self.headers.get("X-Requested-With") != "enceinte":
            return self.send_json({"error": "requête refusée"}, 403)
        u = urlparse(self.path)
        b = self.body()
        if u.path == "/api/_mock/build":  # simule une mise à jour du firmware
            global BUILD
            BUILD = b.get("build", "nouvelle")
            return self.send_json({"ok": True})
        if u.path == "/api/setup":
            STATE["setup"] = False
            STATE["logged"] = True
            return self.send_json({"ok": True})
        if u.path == "/api/login":
            if b.get("password") != "secret":
                return self.send_json({"error": "mot de passe incorrect"}, 401)
            STATE["logged"] = True
            return self.send_json({"ok": True})
        if u.path == "/api/logout":
            STATE["logged"] = False
            return self.send_json({"ok": True})
        if u.path == "/api/player":
            if b.get("action") == "volume":
                STATE["volume"] = max(0, min(STATE["max_volume"], int(b.get("value", 0))))
            if b.get("action") == "toggle":
                STATE["playing"] = "pause" if STATE["playing"] == "play" else "play"
            return self.send_json({"ok": True})
        if u.path == "/api/https":
            STATE["https"] = bool(b.get("enabled"))
            return self.send_json({"ok": True})
        if u.path == "/api/files/rename":
            err = move(b.get("from", ""), b.get("to", ""))
            return self.send_json({"error": err}, 400) if err else self.send_json({"ok": True})
        if u.path == "/api/files/mkdir":
            parent, name = split(b.get("path", ""))
            FILES.setdefault(parent, []).append((name, True, 0))
            FILES[b["path"]] = []
            return self.send_json({"ok": True})
        if u.path == "/api/_mock/stream":
            STATE["stream"] = bool(b.get("on"))
            return self.send_json({"ok": True})
        if u.path == "/api/radio":
            url = b.get("url", "")
            if not url.startswith(("http://", "https://")):
                return self.send_json({"error": "adresse de flux invalide (http:// ou https://)"}, 400)
            folder = b.get("folder") or unique("Webradios", (b.get("name") or "Webradio").strip())
            if folder not in FILES:
                create_folder(folder, [("webradio.m3u", False, 96)])
            RADIOS[folder] = url
            log(f"webradio {folder} : {url}")
            attach(b.get("card"), folder)
            return self.send_json({"folder": folder})
        if u.path == "/api/podcasts":
            url = b.get("url", "")
            if not url.startswith(("http://", "https://")):
                return self.send_json({"error": "adresse de flux invalide"}, 400)
            name = (b.get("name") or "").strip() or "Nouveau podcast"
            folder = unique("Podcasts", name)
            create_folder(folder)
            PODCASTS[folder] = {"url": url, "title": b.get("name") or "", "keep": b.get("keep", 10), "last_check": 0,
                                "last_error": "", "sync_until": time.time() + 6}
            log(f"abonnement : {url} -> {folder}")
            attach(b.get("card"), folder)
            return self.send_json({"folder": folder})
        if u.path == "/api/podcast":
            pc = PODCASTS.get(b.get("folder"))
            if not pc:
                return self.send_json({"error": "pas un podcast"}, 400)
            action = b.get("action")
            if action == "sync":
                pc["sync_until"] = time.time() + 5
            elif action == "unsubscribe":
                del PODCASTS[b["folder"]]
            elif action == "update":
                pc["url"] = b.get("url") or pc["url"]
                pc["keep"] = b.get("keep") or pc["keep"]
            return self.send_json({"ok": True})
        if u.path == "/api/settings":
            for k in ("repeat", "resume_s", "resume_after_other", "shuffle", "normalize", "compress", "max_volume", "vol_touch",
                      "touch_threshold_pct", "touch_hold_ms"):
                if k in b:
                    STATE[k] = b[k]
            STATE["volume"] = min(STATE["volume"], STATE["max_volume"])
            return self.send_json({"ok": True})
        if u.path == "/api/network":
            if b.get("mode") == "static" and not b.get("address", "").count(".") == 3:
                return self.send_json({"error": "adresse IP mal écrite (exemple : 192.168.1.50)"}, 400)
            STATE["ip_test_until"] = time.time() + 300
            STATE["ip_test_address"] = b.get("address", "") if b.get("mode") == "static" else ""
            return self.send_json({"ok": True, "test_s": 300})
        if u.path == "/api/wifi/switch":
            STATE["ap"] = False
            return self.send_json({"ok": True})
        if u.path == "/api/config/import":
            if b.get("format") != "enceinte-reglages":
                return self.send_json({"error": "ce fichier n'est pas un fichier de réglages d'enceinte"}, 400)
            st = b.get("settings", {})
            for k in ("resume_s", "resume_after_other", "shuffle", "normalize", "compress", "max_volume"):
                if k in st:
                    STATE[k] = st[k]
            msg = "Réglages importés"
            if isinstance(b.get("cards"), list):
                CARDS[:] = [dict(c, exists=True) for c in b["cards"]]
                msg += f", {len(CARDS)} cartes associées"
            ip_test = st.get("ip", {}).get("mode") == "static" and st["ip"] != STATE["ip"]
            return self.send_json({"ok": True, "message": msg, "ip_test": ip_test})
        if u.path == "/api/cards":
            card = {"uid": b.get("uid"), "folder": b.get("folder"), "exists": True, "resume_s": b.get("resume_s"),
                    "resume_other": b.get("resume_other"), "shuffle": b.get("shuffle"),
                    "normalize": b.get("normalize"), "compress": b.get("compress"), "repeat": b.get("repeat"),
                    "sleep_tracks": b.get("sleep_tracks") or 0, "sleep_minutes": b.get("sleep_minutes") or 0}
            CARDS[:] = [c for c in CARDS if c["uid"] != card["uid"]] + [card]
            return self.send_json({"ok": True})
        if u.path == "/api/cards/delete":
            CARDS[:] = [c for c in CARDS if c["uid"] != b.get("uid")]
            return self.send_json({"ok": True})
        if u.path == "/api/cards/learn":
            STATE["learning"] = b.get("action") != "cancel"
            STATE["learn_at"] = time.time()
            return self.send_json({"ok": True})
        return self.send_json({"ok": True})

    def do_PUT(self):
        n = int(self.headers.get("Content-Length") or 0)
        self.rfile.read(n)
        self.send_json({"ok": True})


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--setup", action="store_true")
    ap.add_argument("--logged-out", action="store_true")
    ap.add_argument("--ap", action="store_true")
    ap.add_argument("--port", type=int, default=8080)
    args = ap.parse_args()
    STATE["setup"] = args.setup
    STATE["ap"] = args.ap
    STATE["logged"] = not (args.setup or args.logged_out)
    for line in ("Enceinte NFC version 1.0.0", "démarrage après : mise sous tension", "carte SD montée (61056 Mo)",
                 "lecteur prêt : PN5180, firmware 4.0", "connecté, adresse 192.168.1.42"):
        log(line)
    ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()
