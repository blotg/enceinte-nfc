#!/usr/bin/env python3
"""
API simulée de l'enceinte pour développer et tester l'interface web sur PC.

  python3 test/web/mock_api.py [--setup] [--port 8080]
puis ouvrir http://127.0.0.1:8080/

--setup : sortie d'usine (assistant de première configuration).
"""
import argparse
import json
import os
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

WEB = os.path.join(os.path.dirname(__file__), "..", "..", "main", "web")

STATE = {"setup": False, "logged": True, "volume": 35, "playing": "play", "learning": False, "learn_at": 0,
         "resume_s": 600, "resume_after_other": False, "shuffle": False}

FILES = {
    "": [("Comptines", True, 0), ("Histoires du soir", True, 0), ("Musique classique", True, 0)],
    "Comptines": [("01 - Une souris verte.mp3", False, 3_412_000), ("02 - Frère Jacques.mp3", False, 2_904_112),
                  ("10 - Il était un petit navire.mp3", False, 4_800_000), ("cover.jpg", False, 85_000)],
    "Histoires du soir": [("Contes", True, 0)],
    "Histoires du soir/Contes": [],
    "Musique classique": [],
}


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
     "shuffle": None},
    {"uid": "0411223344", "folder": "Histoires du soir", "exists": True, "resume_s": 0, "resume_other": True,
     "shuffle": True},
    {"uid": "04BADA55", "folder": "Comptines", "exists": True, "resume_s": 300, "resume_other": None,
     "shuffle": False},
    {"uid": "04DEADBEEF", "folder": "Ancien dossier", "exists": False, "resume_s": None, "resume_other": None,
     "shuffle": None},
]


def status():
    return {
        "player": {"state": STATE["playing"], "file": "Comptines/02 - Frère Jacques.mp3", "title": "Frère Jacques",
                   "artist": "Chorale des enfants", "album": "Comptines", "elapsed": (time.time() % 150),
                   "duration": 150.0, "seekable": True, "song": 1, "queue_len": 3, "volume": STATE["volume"],
                   "max_volume": 80, "repeat": False, "random": False, "error": ""},
        "card": {"reader_ok": True, "present": "04A1B2C3D4E5F6", "session": "04A1B2C3D4E5F6",
                 "folder": "Comptines", "resume_remaining": 0, "last_unknown": ""},
        "wifi": {"connected": True, "ssid": "Maison", "ip": "192.168.1.42", "rssi": -58, "ap": False,
                 "ap_ssid": "Enceinte-3F2A", "hostname": "enceinte"},
        "sd": {"mounted": True, "total": 15_931_539_456, "free": 12_002_000_000},
        "ota": {"state": 0, "message": "à jour (dernière version : 1.0.0)", "current": "1.0.0", "available": "",
                "progress": 0, "last_check": time.time()},
        "uptime": 7322, "heap": 152000,
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

    def static(self, name, ctype):
        with open(os.path.join(WEB, name), "rb") as f:
            data = f.read()
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
            return self.static("index.html", "text/html; charset=utf-8")
        if u.path == "/app.js":
            return self.static("app.js", "application/javascript")
        if u.path == "/style.css":
            return self.static("style.css", "text/css")
        if u.path == "/api/state":
            return self.send_json({"setup_required": STATE["setup"], "logged_in": STATE["logged"],
                                   "version": "1.0.0", "hostname": "enceinte", "on_ap": STATE["setup"]})
        if u.path == "/api/wifi/scan":
            time.sleep(0.3)
            return self.send_json({"networks": [{"ssid": "Maison", "rssi": -48, "secure": True},
                                                {"ssid": "Voisin", "rssi": -80, "secure": True},
                                                {"ssid": "Café", "rssi": -66, "secure": False}]})
        if not STATE["logged"]:
            return self.send_json({"error": "connexion requise"}, 401)
        if u.path == "/api/status":
            return self.send_json(status())
        if u.path == "/api/cards":
            learned = "04C0FFEE11" if STATE["learning"] and time.time() - STATE["learn_at"] > 2 else ""
            learning = STATE["learning"] and not learned
            return self.send_json({"cards": CARDS, "learning": learning, "learn_remaining": 55, "learned": learned,
                                   "present": "04A1B2C3D4E5F6", "last_unknown": "", "reader_ok": True})
        if u.path == "/api/files":
            path = q.get("path", [""])[0]
            entries = [{"name": n, "dir": d, "size": s, "audio": n.endswith(".mp3")} for n, d, s in FILES.get(path, [])]
            return self.send_json({"path": path, "entries": entries, "total": 15_931_539_456, "free": 12_002_000_000})
        if u.path == "/api/settings":
            return self.send_json({"hostname": "enceinte", "wifi_ssid": "Maison", "ota_url": "",
                                   "ota_interval_h": 24, "max_volume": 80, "mpd_password_set": False,
                                   "resume_s": STATE["resume_s"], "resume_after_other": STATE["resume_after_other"],
                                   "shuffle": STATE["shuffle"],
                                   "https_enabled": STATE.get("https", False), "https_active": STATE.get("https", False),
                                   "https_pending": False,
                                   "mpd_port": 6600})
        self.send_json({"error": "introuvable"}, 404)

    def do_POST(self):
        if self.headers.get("X-Requested-With") != "enceinte":
            return self.send_json({"error": "requête refusée"}, 403)
        u = urlparse(self.path)
        b = self.body()
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
                STATE["volume"] = int(b.get("value", 0))
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
        if u.path == "/api/settings":
            for k in ("resume_s", "resume_after_other", "shuffle"):
                if k in b:
                    STATE[k] = b[k]
            return self.send_json({"ok": True})
        if u.path == "/api/cards":
            card = {"uid": b.get("uid"), "folder": b.get("folder"), "exists": True, "resume_s": b.get("resume_s"),
                    "resume_other": b.get("resume_other"), "shuffle": b.get("shuffle")}
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
    ap.add_argument("--port", type=int, default=8080)
    args = ap.parse_args()
    STATE["setup"] = args.setup
    STATE["logged"] = not (args.setup or args.logged_out)
    ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()
