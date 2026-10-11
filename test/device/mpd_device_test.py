#!/usr/bin/env python3
"""
Test complet du serveur MPD d'une vraie enceinte, par le réseau (Python standard seul).

Usage : test/device/mpd_device_test.py [ADRESSE] [--password MDP] [--folder DOSSIER] [--volume N] [--yes] [-v]

  ADRESSE  adresse IP de l'enceinte (défaut : enceinte.local), port 6600 (--port)
  MDP      mot de passe MPD s'il y en a un (sinon variable d'environnement MPD_PASSWORD)
  DOSSIER  dossier de la carte SD avec au moins 3 morceaux de plus de 30 s (défaut : le premier
           trouvé)

Chaque commande annoncée par l'enceinte (« commands ») est essayée avec ses cas d'erreur, puis
les scénarios des applications : clic sur un morceau dans M.A.L.P., listes de commandes, idle
sur plusieurs connexions, fin de morceau avec repeat/single/consume/random, plusieurs clients en
même temps. L'enceinte joue de courts extraits au volume --volume (5 par défaut). Rien n'est
écrit sur la carte SD ; la file d'attente, le volume, les options et la lecture en cours sont
remis comme avant à la fin.

L'enceinte accepte 3 connexions MPD : fermer les applications MPD (M.A.L.P....) avant le test.
Code de sortie : 0 si tout est bon, 1 sinon.
"""
import argparse
import os
import re
import select
import socket
import sys
import threading
import time

AUDIO_EXT = (".mp3", ".flac", ".wav", ".ogg", ".opus", ".m4a", ".aac")
COVER_NAMES = ("cover.jpg", "cover.png", "folder.jpg", "folder.png", "front.jpg", "front.png")
SLOW_S = 1.0  # au-delà, la commande est signalée comme lente


def q(s):
    """Argument entre guillemets, comme le font les clients MPD."""
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def fq(s):
    """Valeur entre apostrophes dans une expression de filtre, elle-même entre guillemets."""
    return "'" + s.replace("\\", "\\\\").replace("'", "\\'") + "'"


class Resp:
    def __init__(self, pairs, ack, binary):
        self.pairs = pairs  # [(clé, valeur)] dans l'ordre reçu
        self.ack = ack  # ligne "ACK [...]..." ou None
        self.binary = binary
        self.elapsed = 0.0

    @property
    def ok(self):
        return self.ack is None

    def ack_code(self):
        m = re.match(r"ACK \[(\d+)@(\d+)\]", self.ack or "")
        return int(m.group(1)) if m else None

    def get(self, key, default=None):
        key = key.lower()
        for k, v in self.pairs:
            if k.lower() == key:
                return v
        return default

    def values(self, key):
        key = key.lower()
        return [v for k, v in self.pairs if k.lower() == key]

    def records(self, starts=("file", "directory", "playlist")):
        """Découpe la réponse en entrées (clés en minuscules), une par file/directory/playlist."""
        out = []
        for k, v in self.pairs:
            k = k.lower()
            if k in starts:
                out.append({k: v})
            elif out:
                out[-1].setdefault(k, v)
        return out

    def songs(self):
        return [r for r in self.records() if "file" in r]

    def __repr__(self):
        return self.ack or f"OK ({len(self.pairs)} lignes)"


class Conn:
    def __init__(self, t, timeout=60):
        self.t = t
        self.sock = socket.create_connection((t.host, t.port), timeout=timeout)
        self.buf = b""
        self.greeting = self.line()

    def _fill(self):
        data = self.sock.recv(65536)
        if not data:
            raise ConnectionError("connexion fermée par l'enceinte")
        self.buf += data

    def line(self):
        while b"\n" not in self.buf:
            self._fill()
        raw, _, self.buf = self.buf.partition(b"\n")
        return raw.decode("utf-8", errors="replace")

    def read_exact(self, n):
        while len(self.buf) < n:
            self._fill()
        data, self.buf = self.buf[:n], self.buf[n:]
        return data

    def send(self, text):
        self.sock.sendall((text + "\n").encode())

    def read(self):
        pairs, binary = [], b""
        while True:
            line = self.line()
            if line == "OK":
                return Resp(pairs, None, binary)
            if line.startswith("ACK "):
                return Resp(pairs, line, binary)
            k, _, v = line.partition(": ")
            pairs.append((k, v))
            if k == "binary":
                binary += self.read_exact(int(v))
                self.read_exact(1)  # "\n" après les données

    def cmd(self, text):
        t0 = time.monotonic()
        self.send(text)
        r = self.read()
        r.elapsed = time.monotonic() - t0
        self.t.record(text, r.elapsed)
        return r

    def cmd_list(self, cmds, ok_mode=False):
        t0 = time.monotonic()
        self.send("\n".join(["command_list_ok_begin" if ok_mode else "command_list_begin", *cmds,
                             "command_list_end"]))
        r = self.read()
        r.elapsed = time.monotonic() - t0
        for c in cmds:
            self.t.used.add(c.split(None, 1)[0] if c.split() else "")
        self.t.record("command_list (%d)" % len(cmds), r.elapsed)
        return r

    def status(self):
        r = self.cmd("status")
        return {k.lower(): v for k, v in r.pairs}

    def has_data(self, wait):
        return bool(self.buf) or bool(select.select([self.sock], [], [], wait)[0])

    def closed_by_server(self, wait=2.0):
        """True si l'enceinte ferme la connexion dans le délai."""
        end = time.monotonic() + wait
        while time.monotonic() < end:
            if not select.select([self.sock], [], [], end - time.monotonic())[0]:
                return False
            try:
                if not self.sock.recv(4096):
                    return True
            except OSError:
                return True
        return False

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


class Tester:
    def __init__(self, args):
        self.host, self.port, self.password = args.host, args.port, args.password
        self.verbose, self.volume = args.verbose, args.volume
        self.folder_arg = args.folder
        self.failures, self.notes, self.checks = [], [], 0
        self.timings = []  # (durée, commande)
        self.used = set()
        self.section_name = ""

    # ---------- Outils ----------

    def record(self, text, elapsed):
        name = text.split(None, 1)[0] if text.split() else ""
        self.used.add(name)
        self.timings.append((elapsed, text if len(text) < 90 else text[:87] + "..."))

    def section(self, name):
        self.section_name = name
        print(f"\n== {name} ==")

    def check(self, cond, msg, detail=""):
        self.checks += 1
        if cond:
            if self.verbose:
                print(f"  ok      {msg}")
        else:
            text = f"{msg}" + (f" — {detail}" if detail != "" else "")
            self.failures.append(f"[{self.section_name}] {text}")
            print(f"  ÉCHEC   {text}")
        return bool(cond)

    def note(self, msg):
        self.notes.append(f"[{self.section_name}] {msg}")
        print(f"  note    {msg}")

    def ok(self, r, msg):
        return self.check(r.ok, msg, r.ack)

    def ack(self, r, code, msg):
        return self.check(r.ack_code() == code, f"{msg} → ACK {code}", r.ack or "OK")

    def connect(self, auth=True):
        c = Conn(self)
        if c.greeting.startswith("ACK"):
            c.close()
            raise ConnectionRefusedError(c.greeting)
        if auth and self.password:
            r = c.cmd("password " + q(self.password))
            if not r.ok:
                raise PermissionError("mot de passe refusé : " + r.ack)
        return c

    @staticmethod
    def wait_until(pred, timeout=8.0, step=0.2):
        end = time.monotonic() + timeout
        while True:
            v = pred()
            if v or time.monotonic() > end:
                return v
            time.sleep(step)

    def wait_status(self, c, pred, timeout=8.0):
        """Attend un statut qui vérifie pred ; retourne le dernier statut lu."""
        last = {}

        def probe():
            nonlocal last
            last = c.status()
            return pred(last)

        self.wait_until(probe, timeout)
        return last

    @staticmethod
    def elapsed(st):
        return float(st.get("elapsed", -1))

    def queue_files(self, c):
        return [s["file"] for s in c.cmd("playlistinfo").songs()]

    # ---------- Sauvegarde et remise en état ----------

    def save_state(self, c):
        st = c.status()
        self.saved = {"status": st, "files": self.queue_files(c)}
        what = f"{len(self.saved['files'])} morceaux en file, état {st.get('state')}, volume {st.get('volume')}"
        print(f"État sauvegardé : {what}")

    def restore_state(self):
        s = self.saved
        st = s["status"]
        try:
            c = self.connect()
        except Exception as e:  # noqa: BLE001
            print(f"Remise en état impossible ({e}) : file d'attente d'origine perdue", file=sys.stderr)
            return
        try:
            c.cmd("stop")
            c.cmd("clear")
            lost = [f for f in s["files"] if not c.cmd("add " + q(f)).ok]
            for opt in ("repeat", "random", "single", "consume"):
                c.cmd(f"{opt} {st.get(opt, '0')}")
            state, song = st.get("state"), st.get("song")
            if state in ("play", "pause") and song is not None:
                c.cmd("setvol 0" if state == "pause" else f"setvol {st['volume']}")
                f = s["files"][int(song)] if int(song) < len(s["files"]) else ""
                if "://" in f:
                    c.cmd(f"play {song}")
                else:
                    c.cmd(f"seek {song} {float(st.get('elapsed', 0)):.3f}")
                if state == "pause":
                    self.wait_status(c, lambda x: x.get("state") == "play", 3)
                    c.cmd("pause 1")
            c.cmd(f"setvol {st['volume']}")
            end = c.status()
            print(f"Remis en état : {end.get('playlistlength')} morceaux en file, état {end.get('state')}, "
                  f"volume {end.get('volume')}" + (f" ; introuvables : {lost}" if lost else ""))
        finally:
            c.close()

    # ---------- Choix des morceaux de test ----------

    def discover(self, c):
        """Parcourt la carte : dossier de test, dossier avec pochette, dossiers de premier niveau."""
        root = c.cmd("lsinfo")
        self.top_dirs = [r["directory"] for r in root.records() if "directory" in r]
        queue, seen, self.cover_dir, candidates = list(self.top_dirs), 0, None, []
        if self.folder_arg:
            queue.insert(0, self.folder_arg)
        while queue and seen < 80:
            d = queue.pop(0)
            seen += 1
            r = c.cmd("lsinfo " + q(d))
            if not r.ok:
                continue
            recs = r.records()
            queue += [x["directory"] for x in recs if "directory" in x]
            songs = [x for x in recs if "file" in x and float(x.get("duration", 0) or 0) >= 30]
            if len(songs) >= 3 and (not self.folder_arg or d == self.folder_arg):
                candidates.append((d, songs))
            if not self.cover_dir:
                names = [v.lower() for v in c.cmd("listfiles " + q(d)).values("file")]
                if any(n in COVER_NAMES for n in names) and any(x for x in recs if "file" in x):
                    self.cover_dir = d
            if candidates and self.cover_dir and not self.folder_arg:
                break
        if not candidates:
            sys.exit("Aucun dossier avec 3 morceaux de plus de 30 s (option --folder)")
        self.folder, songs = candidates[0]
        self.songs = songs[:3]
        self.paths = [s["file"] for s in self.songs]
        self.durations = [float(s["duration"]) for s in self.songs]
        self.top = self.folder.split("/")[0]
        print(f"Dossier de test : {self.folder} ({len(songs)} morceaux de plus de 30 s)")
        print(f"Pochette : {self.cover_dir or 'aucun dossier avec cover.jpg/png'}")

    # ======================= Connexion et protocole =======================

    def t_connection(self):
        self.section("Connexion et protocole")
        c = Conn(self)
        self.check(re.match(r"OK MPD \d+\.\d+\.\d+$", c.greeting), "message d'accueil OK MPD x.y.z", c.greeting)
        if self.password:
            self.ack(c.cmd("status"), 4, "status sans mot de passe")
            names = c.cmd("commands").values("command")
            self.check("password" in names and "ping" in names and "status" not in names,
                       "commands sans mot de passe : seulement les commandes ouvertes", names)
            self.check("status" in c.cmd("notcommands").values("command"), "notcommands liste status")
            self.ack(c.cmd("password " + q(self.password + "x")), 3, "mauvais mot de passe")
            self.ok(c.cmd("password " + q(self.password)), "bon mot de passe")
            self.check(c.cmd("notcommands").values("command") == [], "notcommands vide une fois connecté")
        else:
            r = c.cmd("status")
            self.ok(r, "status sans mot de passe (aucun mot de passe réglé)")
            self.check(c.cmd("notcommands").values("command") == [], "notcommands vide sans mot de passe")
            c.cmd("password quelconque")
            self.note("pas de mot de passe MPD : tests du mot de passe ignorés")
        self.ok(c.cmd("ping"), "ping")
        self.commands = c.cmd("commands").values("command")
        self.check({"idle", "noidle", "status", "play"} <= set(self.commands), "commands liste idle, noidle...")
        r = c.cmd("xyzzy")
        self.check(r.ack_code() == 5 and "unknown command" in r.ack, "commande inconnue → ACK 5", r.ack)
        self.ack(c.cmd("status 1"), 2, "trop d'arguments")
        self.ack(c.cmd("play 1 2 3"), 2, "trop d'arguments pour play")
        self.check(c.cmd('lsinfo "pas fermé').ack, "guillemet non fermé → ACK")
        self.check(c.cmd("").ack, "ligne vide → ACK")
        c.send("ping\r")
        self.check(c.read().ok, "fin de ligne \\r\\n acceptée")
        c.send("\n".join(["ping", "status", "currentsong"] * 10))
        rs = [c.read() for _ in range(30)]
        self.check(all(r.ok for r in rs), "30 commandes envoyées d'un bloc (pipeline)")
        c.send("noidle\nping")
        self.check(c.read().ok and not c.has_data(0.3), "noidle hors idle ignoré sans réponse")
        r = c.cmd("ping " + "x" * 50)
        self.ack(r, 2, "argument en trop pour ping")
        c.send("close")
        self.used.add("close")
        self.check(c.closed_by_server(), "close ferme la connexion")
        c.close()

        c = self.connect()
        c.send("lsinfo " + q("a" * 3000))
        closed = c.closed_by_server(3)
        self.check(closed, "ligne de 3000 caractères : connexion fermée proprement")
        c.close()
        c = self.connect()
        self.ok(c.cmd("ping"), "nouvelle connexion après une ligne trop longue")
        c.close()

        # Nombre de connexions limité : la suivante reçoit une erreur, puis la place se libère.
        conns, refused = [], None
        for _ in range(6):
            try:
                x = Conn(self)
            except OSError as e:
                refused = str(e)
                break
            if x.greeting.startswith("ACK"):
                refused = x.greeting
                x.close()
                break
            conns.append(x)
        self.check(refused and "too many clients" in refused, "connexion de trop refusée avec un message",
                   refused)
        self.check(len(conns) >= 2, f"au moins 2 connexions simultanées acceptées ({len(conns)})")
        for x in conns:
            x.close()
        ok = self.wait_until(lambda: self._try_ping(), 5, 0.3)
        self.check(ok, "place libérée après la fermeture des connexions")

    def _try_ping(self):
        try:
            c = self.connect()
        except (ConnectionRefusedError, OSError):
            return False
        r = c.cmd("ping")
        c.close()
        return r.ok

    # ======================= Divers =======================

    def t_misc(self, c):
        self.section("Commandes diverses")
        st = c.cmd("stats")
        self.check(st.ok and st.get("uptime") and st.get("db_update"), "stats : uptime, db_update", st.pairs)
        if st.get("songs") == "0":
            self.note("stats donne songs: 0 (pas de base de données) ; mpc et certaines applis l'affichent")
        out = c.cmd("outputs")
        self.check(out.get("outputid") == "0" and out.get("outputenabled") == "1", "outputs : sortie 0 active")
        for cmd in ("enableoutput 0", "disableoutput 0", "toggleoutput 0"):
            self.ok(c.cmd(cmd), cmd)
        self.check(c.cmd("outputs").get("outputenabled") == "1", "la sortie reste active (une seule sortie)")
        self.ack(c.cmd("enableoutput 3"), 50, "enableoutput 3")
        dec = c.cmd("decoders").values("suffix")
        self.check({"mp3", "flac", "ogg", "opus"} <= set(dec), "decoders : mp3, flac, ogg, opus", dec)
        self.ok(c.cmd("urlhandlers"), "urlhandlers")
        self.check(c.cmd("listpartitions").get("partition") == "default", "listpartitions")
        self.ack(c.cmd("config"), 4, "config (réservée aux connexions locales)")
        for cmd in ("channels", "readmessages", "subscribe test", "unsubscribe test", "sendmessage test bonjour",
                    "crossfade 0", "mixrampdb 0", "mixrampdelay 0", "replay_gain_mode off", "clearerror"):
            self.ok(c.cmd(cmd), cmd)
        self.check(c.cmd("replay_gain_status").get("replay_gain_mode") == "off", "replay_gain_status")
        self.ok(c.cmd("binarylimit 1024"), "binarylimit 1024")
        self.ack(c.cmd("binarylimit 10"), 2, "binarylimit trop petit")
        c.cmd("binarylimit 8192")

        tags = c.cmd("tagtypes").values("tagtype")
        self.check({"Artist", "Album", "Title"} <= set(tags), "tagtypes : Artist, Album, Title", tags)
        song = self.paths[0]
        self.ok(c.cmd("tagtypes disable Title"), "tagtypes disable Title")
        r = c.cmd("lsinfo " + q(song))
        self.check(r.get("title") is None and "Title" not in c.cmd("tagtypes").values("tagtype"),
                   "Title masqué après disable", r.pairs)
        self.ok(c.cmd("tagtypes clear"), "tagtypes clear")
        r = c.cmd("lsinfo " + q(song))
        self.check(r.get("file") == song and r.get("artist") is None and r.get("duration"),
                   "tagtypes clear : file et durée seulement", r.pairs)
        self.ok(c.cmd("tagtypes enable Title"), "tagtypes enable Title")
        self.check(c.cmd("tagtypes").values("tagtype") == ["Title"], "seul Title après enable")
        self.ok(c.cmd("tagtypes all"), "tagtypes all")
        self.check(c.cmd("tagtypes").values("tagtype") == tags, "tagtypes all : liste complète")
        self.ack(c.cmd("tagtypes bidule"), 2, "tagtypes sous-commande inconnue")

    # ======================= Bibliothèque =======================

    def t_library(self, c):
        self.section("Bibliothèque (lecture seule)")
        root = c.cmd("lsinfo")
        self.check(root.ok and self.top in [r.get("directory") for r in root.records()], "lsinfo racine")
        self.check(c.cmd('lsinfo ""').pairs == root.pairs, 'lsinfo "" = racine')
        self.check(c.cmd('lsinfo "/"').pairs == root.pairs, 'lsinfo "/" = racine')
        self.check(all(r.get("last-modified") for r in root.records()), "dossiers avec Last-Modified")

        r = c.cmd("lsinfo " + q(self.folder))
        songs = r.songs()
        self.check(all(re.match(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$", s.get("last-modified", "")) for s in songs),
                   "morceaux avec Last-Modified ISO 8601")
        self.check(all(s.get("time") and s.get("duration") for s in songs[:3]), "morceaux avec Time et duration")
        self.check(all(s["file"].startswith(self.folder + "/") for s in songs), "chemins complets depuis la racine")
        one = c.cmd("lsinfo " + q(self.paths[0])).songs()
        self.check(len(one) == 1 and one[0]["file"] == self.paths[0], "lsinfo d'un fichier", one)
        self.check(one and one[0].get("title") == self.songs[0].get("title"), "mêmes tags que dans le dossier")
        self.ack(c.cmd("lsinfo " + q(self.folder + "/inexistant")), 50, "lsinfo dossier inexistant")
        self.check(c.cmd('lsinfo "../.."').ack_code() in (2, 50), "lsinfo ../.. refusé")
        self.ack(c.cmd('lsinfo "http://example.com/"'), 50, "lsinfo URL")

        lf = c.cmd("listfiles " + q(self.folder))
        recs = lf.records(("file", "directory"))
        base = self.paths[0].rsplit("/", 1)[1]
        mine = [x for x in recs if x.get("file") == base]
        self.check(mine and int(mine[0].get("size", 0)) > 0 and mine[0].get("last-modified"),
                   "listfiles : nom, taille, date", mine)
        self.ack(c.cmd("listfiles " + q(self.folder + "/inexistant")), 50, "listfiles dossier inexistant")

        la = c.cmd("listall " + q(self.folder)).values("file")
        self.check(set(self.paths) <= set(la), "listall du dossier contient ses morceaux")
        self.check(c.cmd("listall " + q(self.paths[0])).values("file") == [self.paths[0]], "listall d'un fichier")
        lai = c.cmd("listallinfo " + q(self.folder)).songs()
        self.check([s["file"] for s in lai] == la, "listallinfo = listall (même ordre)")
        t = {s["file"]: s.get("title") for s in lai}
        self.check(all(t.get(s["file"]) == s.get("title") for s in self.songs), "listallinfo : mêmes titres")
        self.ack(c.cmd("listall " + q(self.folder + "/inexistant")), 50, "listall inexistant")

        full = c.cmd("listallinfo")
        all_songs = full.songs()
        self.total = len(all_songs)
        cnt = c.cmd("count")
        self.check(cnt.ok and int(cnt.get("songs", -1)) == self.total, "count = nombre de morceaux de listallinfo",
                   f"{cnt.get('songs')} / {self.total}")
        all_files = c.cmd("listall").values("file")
        self.check(len(all_files) == self.total, "listall complet = listallinfo", f"{len(all_files)} / {self.total}")
        print(f"  info    bibliothèque : {self.total} morceaux, listallinfo en {full.elapsed:.2f} s")

        for tag in ("artist", "album", "albumartist", "genre", "date", "title", "track"):
            self.ok(c.cmd("list " + tag), f"list {tag}")
        if c.cmd("list composer").ack:
            self.note("list composer → ACK (tag inconnu de l'enceinte ; MPD renverrait une liste vide)")
        files = c.cmd("list file").values("file")
        self.check(len(files) == self.total, "list file = tous les morceaux", f"{len(files)} / {self.total}")
        self.ack(c.cmd("list bidule"), 2, "list tag inconnu")
        self.ack(c.cmd("list any"), 2, "list any")

        song = self.songs[0]
        artist, album, title = song.get("artist"), song.get("album"), song.get("title")
        if artist:
            albums = c.cmd("list album " + q(artist)).values("album")  # ancienne syntaxe
            self.check(not album or album in albums, "list album ARTISTE (ancienne syntaxe)", albums)
            # Syntaxe de M.A.L.P. : expression doublement échappée, puis regroupements.
            expr = '"(artist == \\"' + artist.replace("\\", "\\\\\\\\").replace('"', '\\\\\\"') + '\\")"'
            r = c.cmd(f"list album {expr} group albumartist group date")
            self.check(r.ok and (not album or album in r.values("album")), "list album (expr) group … (M.A.L.P.)",
                       r)
            if r.ok and not r.values("albumartist") and not r.values("date"):
                self.note("list … group : regroupement ignoré (albums sans artiste ni date dans M.A.L.P.)")
            found = c.cmd("find artist " + q(artist)).songs()
            self.check(song["file"] in [s["file"] for s in found], "find artist", len(found))
            found2 = c.cmd("find " + q(f"(artist == {fq(artist)})")).songs()
            self.check([s["file"] for s in found2] == [s["file"] for s in found], "find (artist == …) = find artist")
            part = artist[1:-1].lower() if len(artist) > 3 else artist.lower()
            srch = c.cmd("search artist " + q(part)).songs()
            self.check(len(srch) >= len(found), "search artist (partiel, minuscules)", f"{len(srch)} < {len(found)}")
            w = c.cmd("find artist " + q(artist) + " window 0:1").songs()
            self.check(len(w) == 1, "find … window 0:1", len(w))
            self.ok(c.cmd("find artist " + q(artist) + " sort Title"), "find … sort Title")
            self.ok(c.cmd("count artist " + q(artist)), "count artist")
            r = c.cmd("count group artist")
            self.ok(r, "count group artist")
        else:
            self.note(f"{song['file']} sans tag artiste : tests par artiste ignorés")
        if title:
            r = c.cmd("search title " + q(title.upper())).songs()
            self.check(song["file"] in [s["file"] for s in r], "search title EN MAJUSCULES")
            r = c.cmd("find title " + q(title)).songs()
            self.check(song["file"] in [s["file"] for s in r], "find title exact")
            r = c.cmd("search any " + q(title[:4])).songs()
            self.check(song["file"] in [s["file"] for s in r], "search any")
        r = c.cmd("find " + q(f"(file == {fq(song['file'])})")).songs()
        self.check([s["file"] for s in r] == [song["file"]], "find (file == …)", r)
        r = c.cmd("find " + q(f"(base {fq(self.folder)})")).songs()
        self.check([s["file"] for s in r] == la, "find (base …) = listall du dossier", f"{len(r)} / {len(la)}")
        cnt = c.cmd("count " + q(f"(base {fq(self.folder)})"))
        self.check(int(cnt.get("songs", -1)) == len(la), "count (base …)", cnt.pairs)
        r = c.cmd("find " + q(f"((base {fq(self.folder)}) AND (file == {fq(song['file'])}))")).songs()
        self.check(len(r) == 1, "find avec AND", len(r))
        self.ack(c.cmd('find "(artist =="'), 2, "expression de filtre invalide")
        self.ack(c.cmd("find bidule x"), 2, "find tag inconnu")

        # Pochettes
        if self.cover_dir:
            r = c.cmd("albumart " + q(self.cover_dir) + " 0")
            size = int(r.get("size", 0))
            data, chunks = r.binary, 1
            while r.ok and len(data) < size and chunks < 2000:
                r = c.cmd("albumart " + q(self.cover_dir) + f" {len(data)}")
                if not r.binary:
                    break
                data += r.binary
                chunks += 1
            self.check(size > 0 and len(data) == size, "albumart : image complète par morceaux",
                       f"{len(data)} / {size} octets, {chunks} morceaux")
            self.check(data[:3] == b"\xff\xd8\xff" or data[:8] == b"\x89PNG\r\n\x1a\n", "albumart : JPEG ou PNG")
            c.cmd("binarylimit 1024")
            r = c.cmd("albumart " + q(self.cover_dir) + " 0")
            self.check(len(r.binary) == min(1024, size), "albumart respecte binarylimit", len(r.binary))
            c.cmd("binarylimit 8192")
            self.ack(c.cmd("albumart " + q(self.cover_dir) + f" {size + 10}"), 2, "albumart décalage trop grand")
        else:
            self.note("aucun dossier avec pochette : albumart non vérifié en détail")
        r = c.cmd("albumart " + q(self.paths[0]) + " 0")
        self.check(r.ok or r.ack_code() == 50, "albumart d'un morceau (image ou ACK 50)", r.ack)
        self.ok(c.cmd("readpicture " + q(self.paths[0]) + " 0"), "readpicture")
        self.ack(c.cmd("albumart " + q(self.folder + "/inexistant.mp3") + " 0"), 50, "albumart fichier inexistant")

        # Listes de lecture = dossiers de premier niveau
        pls = c.cmd("listplaylists").values("playlist")
        self.check(set(pls) == set(self.top_dirs), "listplaylists = dossiers de premier niveau", pls)
        lp = c.cmd("listplaylist " + q(self.top)).values("file")
        self.check(set(self.paths) <= set(lp), "listplaylist contient les morceaux du dossier", len(lp))
        lpi = c.cmd("listplaylistinfo " + q(self.top)).songs()
        self.check([s["file"] for s in lpi] == lp and all(s.get("duration") for s in lpi[:5]),
                   "listplaylistinfo : mêmes fichiers, avec tags")
        self.ack(c.cmd('listplaylist "inexistant-xyz"'), 50, "listplaylist inexistante")
        for cmd in ("save x", "rm x", "rename x y", "playlistadd x " + q(self.paths[0]), "playlistclear x",
                    "playlistdelete x 0", "playlistmove x 0 1"):
            self.ack(c.cmd(cmd), 4, cmd.split()[0] + " (listes en lecture seule)")

        self.check(c.cmd("update").get("updating_db"), "update → updating_db")
        self.check(c.cmd("rescan " + q(self.folder)).get("updating_db"), "rescan dossier → updating_db")

    # ======================= File d'attente =======================

    def t_queue(self, c):
        self.section("File d'attente")
        p0, p1, p2 = self.paths
        c.cmd("stop")
        self.ok(c.cmd("clear"), "clear")
        st = c.status()
        self.check(st.get("playlistlength") == "0", "file vide après clear")
        v0 = int(st["playlist"])
        self.ok(c.cmd("add " + q(p0)), "add fichier")
        self.check(int(c.status()["playlist"]) > v0, "numéro de version de la file augmenté")
        c.cmd("add " + q(p1))
        r = c.cmd("addid " + q(p2))
        id2 = r.get("id")
        self.check(r.ok and id2 and id2.isdigit(), "addid → Id", r.pairs)
        r = c.cmd("addid " + q(p2) + "  1")  # deux espaces : comme M.A.L.P.
        self.check(r.ok and r.get("id"), "addid avec position (deux espaces, comme M.A.L.P.)", r)
        self.ok(c.cmd("add " + q(p1) + " 0"), "add avec position")
        files = self.queue_files(c)
        self.check(files == [p1, p0, p2, p1, p2], "ordre de la file après insertions", files)
        pl = c.cmd("playlist").pairs
        self.check([k for k, _ in pl] == ["0:file", "1:file", "2:file", "3:file", "4:file"], "playlist", pl)
        info = c.cmd("playlistinfo").songs()
        ids = [s.get("id") for s in info]
        self.check([s.get("pos") for s in info] == ["0", "1", "2", "3", "4"] and len(set(ids)) == 5,
                   "playlistinfo : Pos 0..4 et Id uniques", ids)
        self.check([s["file"] for s in c.cmd("playlistinfo 1").songs()] == [p0], "playlistinfo 1")
        self.check(len(c.cmd("playlistinfo 1:3").songs()) == 2, "playlistinfo 1:3")
        self.check(len(c.cmd("playlistinfo 3:").songs()) == 2, "playlistinfo 3:")
        self.ack(c.cmd("playlistinfo 9"), 2, "playlistinfo hors file")
        self.check(len(c.cmd("playlistid").songs()) == 5, "playlistid sans argument = toute la file")
        r = c.cmd(f"playlistid {id2}").songs()
        self.check(len(r) == 1 and r[0]["file"] == p2 and r[0].get("pos") == "4", "playlistid Id", r)
        self.ack(c.cmd("playlistid 999999"), 50, "playlistid inconnu")
        self.check(len(c.cmd("plchanges 0").songs()) >= 5, "plchanges 0")
        self.ok(c.cmd(f"plchanges {c.status()['playlist']}"), "plchanges version courante")
        self.ok(c.cmd("plchanges 0 1:3"), "plchanges avec plage")
        r = c.cmd("plchangesposid 0")
        self.check(r.values("cpos") == ["0", "1", "2", "3", "4"] and r.values("Id") == ids, "plchangesposid",
                   r.pairs)
        r = c.cmd("playlistfind file " + q(p2)).songs()
        self.check([s.get("pos") for s in r] == ["2", "4"], "playlistfind file (2 fois dans la file)", r)
        t = self.songs[2].get("title")
        if t:
            r = c.cmd("playlistsearch title " + q(t[:5].lower())).songs()
            self.check(len(r) >= 2, "playlistsearch title partiel", len(r))
        self.ok(c.cmd("playlistfind " + q(f"(file == {fq(p0)})")), "playlistfind expression")

        # Déplacements et suppressions : file = [p1, p0, p2, p1, p2]
        self.ok(c.cmd("move 0 4"), "move 0 4")
        self.check(self.queue_files(c) == [p0, p2, p1, p2, p1], "ordre après move")
        self.ok(c.cmd("move 0:2 3"), "move plage 0:2 3")
        self.check(self.queue_files(c) == [p1, p2, p1, p0, p2], "ordre après move de plage")
        last_id = c.cmd("playlistinfo 4").get("id")
        self.ok(c.cmd(f"moveid {last_id} 0"), "moveid")
        self.check(self.queue_files(c) == [p2, p1, p2, p1, p0], "ordre après moveid")
        self.ok(c.cmd("swap 0 4"), "swap 0 4")
        self.check(self.queue_files(c) == [p0, p1, p2, p1, p2], "ordre après swap")
        a, b = c.cmd("playlistinfo 0").get("id"), c.cmd("playlistinfo 2").get("id")
        self.ok(c.cmd(f"swapid {a} {b}"), "swapid")
        self.check(self.queue_files(c) == [p2, p1, p0, p1, p2], "ordre après swapid")
        self.ok(c.cmd("delete 3"), "delete 3")
        self.ok(c.cmd("delete 0:1"), "delete plage 0:1")
        self.check(self.queue_files(c) == [p1, p0, p2], "file après delete", self.queue_files(c))
        self.ok(c.cmd(f"deleteid {c.cmd('playlistinfo 0').get('id')}"), "deleteid")
        self.check(self.queue_files(c) == [p0, p2], "file après deleteid")
        self.ack(c.cmd("delete 9"), 2, "delete hors file")
        self.ack(c.cmd("deleteid 999999"), 50, "deleteid inconnu")
        self.ack(c.cmd("move 9 0"), 2, "move hors file")
        self.ack(c.cmd("moveid 999999 0"), 50, "moveid inconnu")
        self.ack(c.cmd("swap 0 9"), 2, "swap hors file")
        self.ack(c.cmd("add " + q(p0) + " 99"), 2, "add position hors file")
        for cmd in ("prio 10 0:1", f"prioid 10 {c.cmd('playlistinfo 0').get('id')}",
                    f"rangeid {c.cmd('playlistinfo 0').get('id')} 1.0:2.0"):
            self.ok(c.cmd(cmd), cmd.split()[0])
        before = sorted(self.queue_files(c))
        self.ok(c.cmd("shuffle"), "shuffle")
        self.check(sorted(self.queue_files(c)) == before, "shuffle garde les mêmes morceaux")
        self.ok(c.cmd("shuffle 0:2"), "shuffle avec plage")

        # Ajouts groupés, erreurs
        c.cmd("clear")
        n_folder = len(c.cmd("listall " + q(self.folder)).values("file"))
        self.ok(c.cmd("add " + q(self.folder)), "add dossier")
        self.check(int(c.status()["playlistlength"]) == n_folder, "add dossier : tous ses morceaux",
                   f"{c.status()['playlistlength']} / {n_folder}")
        self.ack(c.cmd("addid " + q(self.folder)), 2, "addid dossier")
        self.ack(c.cmd("add " + q(self.folder + "/inexistant.mp3")), 50, "add fichier inexistant")
        self.check(c.cmd('add "../x.mp3"').ack_code() in (2, 50), "add ../x.mp3 refusé")
        c.cmd("clear")
        url = "http://example.invalid/radio.mp3"
        self.ok(c.cmd("add " + q(url)), "add URL de webradio")
        r = c.cmd("addid " + q(url))
        self.check(r.ok and r.get("id"), "addid URL", r)
        self.check(self.queue_files(c) == [url, url], "URL dans la file telle quelle")
        self.ok(c.cmd(f"deleteid {r.get('id')}"), "deleteid de l'URL")
        c.cmd("clear")
        n_top = len(c.cmd("listplaylist " + q(self.top)).values("file"))
        self.ok(c.cmd("load " + q(self.top)), "load dossier de premier niveau")
        self.check(int(c.status()["playlistlength"]) == n_top, "load : tous les morceaux", n_top)
        c.cmd("clear")
        c.cmd("add " + q(p2))
        self.ok(c.cmd("load " + q(self.top) + " 0:2"), "load avec plage")
        self.ok(c.cmd("load " + q(self.top) + " 0:1 0"), "load avec plage et position")
        files = self.queue_files(c)
        self.check(len(files) == 4 and files[1] == p2, "load : plage insérée en tête", files)
        self.ack(c.cmd('load "inexistant-xyz"'), 50, "load inexistant")
        c.cmd("clear")
        self.ok(c.cmd("findadd " + q(f"(base {fq(self.folder)})")), "findadd")
        self.check(int(c.status()["playlistlength"]) == n_folder, "findadd : tous les morceaux du dossier")
        t = self.songs[0].get("title")
        if t:
            c.cmd("clear")
            n = len(c.cmd("search title " + q(t)).songs())
            self.ok(c.cmd("searchadd title " + q(t)), "searchadd")
            self.check(int(c.status()["playlistlength"]) == n, "searchadd : autant que search", n)
        c.cmd("clear")
        # Ajout d'une liste de morceaux d'un coup, comme M.A.L.P. pour un album.
        adds = ["add " + q(self.paths[i % 3]) for i in range(30)]
        self.ok(c.cmd_list(adds), "liste de 30 add (ajout d'un album dans M.A.L.P.)")
        self.check(c.status()["playlistlength"] == "30", "30 morceaux dans la file")
        self.ok(c.cmd("delete 0:"), "delete 0: (jusqu'au bout)")
        self.check(c.status()["playlistlength"] == "0", "file vide après delete 0:")

    # ======================= Lecture =======================

    def setup_queue(self, c):
        c.cmd("stop")
        c.cmd("clear")
        for p in self.paths:
            c.cmd("add " + q(p))
        for opt in ("repeat 0", "random 0", "single 0", "consume 0"):
            c.cmd(opt)

    def playing(self, c, song, timeout=8.0):
        """Attend la lecture du morceau à la position song (temps écoulé qui avance)."""
        st = self.wait_status(c, lambda s: s.get("state") == "play" and s.get("song") == str(song), timeout)
        if st.get("state") != "play":
            return st
        e1 = self.elapsed(st)
        st2 = self.wait_status(c, lambda s: self.elapsed(s) > e1 + 0.3, 4)
        return st2

    def t_playback(self, c):
        self.section("Lecture")
        c.cmd(f"setvol {self.volume}")
        self.setup_queue(c)
        self.ok(c.cmd("play"), "play (file arrêtée)")
        st = self.playing(c, 0)
        self.check(st.get("state") == "play" and st.get("song") == "0", "lecture du premier morceau", st)
        self.check(re.match(r"\d+:\d+$", st.get("time", "")) and st.get("duration") and st.get("bitrate")
                   and re.match(r"\d+:\d+:\d+$", st.get("audio", "")), "status : time, duration, bitrate, audio", st)
        self.check(abs(float(st.get("duration", 0)) - self.durations[0]) < 1.5, "durée identique à lsinfo")
        self.check(st.get("nextsong") == "1" and st.get("nextsongid"), "status : nextsong 1")
        cur = c.cmd("currentsong").songs()
        self.check(cur and cur[0]["file"] == self.paths[0] and cur[0].get("pos") == "0" and cur[0].get("id"),
                   "currentsong", cur)

        self.ok(c.cmd("pause 1"), "pause 1")
        st = self.wait_status(c, lambda s: s.get("state") == "pause", 3)
        e1 = self.elapsed(st)
        time.sleep(1.5)
        st = c.status()
        self.check(st.get("state") == "pause" and abs(self.elapsed(st) - e1) < 0.3, "en pause : le temps s'arrête",
                   f"{e1} → {st.get('elapsed')}")
        self.ok(c.cmd("pause 0"), "pause 0")
        st = self.playing(c, 0)
        self.check(st.get("state") == "play" and self.elapsed(st) > e1, "reprise après pause 0", st.get("elapsed"))
        c.cmd("pause")
        self.check(self.wait_status(c, lambda s: s.get("state") == "pause", 3).get("state") == "pause",
                   "pause sans argument : bascule en pause")
        c.cmd("pause")
        self.check(self.wait_status(c, lambda s: s.get("state") == "play", 3).get("state") == "play",
                   "pause sans argument : bascule en lecture")
        self.ok(c.cmd("play"), "play pendant la lecture")
        self.check(c.status().get("song") == "0", "play sans argument ne change pas de morceau")

        self.ok(c.cmd("play 2"), "play 2")
        self.check(self.playing(c, 2).get("song") == "2", "morceau 2 joué")
        self.ok(c.cmd("previous"), "previous")
        self.check(self.playing(c, 1).get("song") == "1", "previous → morceau 1")
        self.ok(c.cmd("next"), "next")
        self.check(self.playing(c, 2).get("song") == "2", "next → morceau 2")
        self.ok(c.cmd("next"), "next sur le dernier morceau")
        self.check(self.wait_status(c, lambda s: s.get("state") == "stop", 4).get("state") == "stop",
                   "next sur le dernier morceau (sans repeat) → arrêt")
        id1 = c.cmd("playlistinfo 1").get("id")
        self.ok(c.cmd(f"playid {id1}"), "playid")
        self.check(self.playing(c, 1).get("song") == "1", "playid → morceau 1")
        self.ok(c.cmd("playid"), "playid sans argument")

        # Positions
        self.ok(c.cmd("seek 1 20"), "seek 1 20")
        st = self.wait_status(c, lambda s: 18 <= self.elapsed(s) <= 24, 4)
        self.check(18 <= self.elapsed(st) <= 24, "seek → environ 20 s", st.get("elapsed"))
        self.ok(c.cmd(f"seekid {id1} 10.5"), "seekid")
        st = self.wait_status(c, lambda s: 8.5 <= self.elapsed(s) <= 14, 4)
        self.check(8.5 <= self.elapsed(st) <= 14, "seekid → environ 10,5 s", st.get("elapsed"))
        self.ok(c.cmd("seekcur 25"), "seekcur 25")
        st = self.wait_status(c, lambda s: 23 <= self.elapsed(s) <= 29, 4)
        self.check(23 <= self.elapsed(st) <= 29, "seekcur → environ 25 s", st.get("elapsed"))
        e = self.elapsed(c.status())
        self.ok(c.cmd("seekcur +5"), "seekcur +5")
        st = self.wait_status(c, lambda s: e + 3 <= self.elapsed(s) <= e + 9, 4)
        self.check(e + 3 <= self.elapsed(st) <= e + 9, "seekcur +5 avance", f"{e} → {st.get('elapsed')}")
        e = self.elapsed(st)
        self.ok(c.cmd("seekcur -10"), "seekcur -10")
        st = self.wait_status(c, lambda s: e - 12 <= self.elapsed(s) <= e - 7, 4)
        self.check(e - 12 <= self.elapsed(st) <= e - 7, "seekcur -10 recule", f"{e} → {st.get('elapsed')}")
        c.cmd("pause 1")
        self.wait_status(c, lambda s: s.get("state") == "pause", 3)
        self.ok(c.cmd("seek 2 15"), "seek en pause")
        st = self.wait_status(c, lambda s: s.get("song") == "2", 4)
        self.check(st.get("state") == "pause" and st.get("song") == "2" and 13 <= self.elapsed(st) <= 18,
                   "seek en pause : reste en pause au bon endroit", st)
        st = self.wait_status(c, lambda s: float(s.get("duration", 0)) > 0, 3)
        self.check(abs(float(st.get("duration", 0)) - self.durations[2]) < 1.5, "en pause après un saut : durée connue",
                   st.get("duration"))
        self.ok(c.cmd("seek 2 25"), "second saut en pause")
        st = self.wait_status(c, lambda s: 23 <= self.elapsed(s) <= 28, 3)
        self.check(st.get("state") == "pause" and 23 <= self.elapsed(st) <= 28, "second saut en pause : position",
                   st.get("elapsed"))
        c.cmd("pause 0")
        self.ack(c.cmd("play 99"), 2, "play hors file")
        self.ack(c.cmd("playid 999999"), 50, "playid inconnu")
        self.ack(c.cmd("seek 99 1"), 2, "seek hors file")
        self.ack(c.cmd("seekid 999999 1"), 50, "seekid inconnu")
        self.ack(c.cmd("seekcur abc"), 2, "seekcur invalide")
        self.ack(c.cmd("pause 2"), 2, "pause 2")
        self.ok(c.cmd("stop"), "stop")
        self.check(self.wait_status(c, lambda s: s.get("state") == "stop", 3).get("state") == "stop", "arrêt")
        self.check(c.cmd("seekcur 5").ack, "seekcur à l'arrêt → ACK")
        self.ok(c.cmd("next"), "next à l'arrêt")
        self.ok(c.cmd("previous"), "previous à l'arrêt")
        self.check(c.status().get("state") == "stop", "next/previous à l'arrêt ne relancent pas")
        self.ok(c.cmd("seek 0 12"), "seek à l'arrêt")
        st = self.playing(c, 0)
        self.check(st.get("state") == "play" and self.elapsed(st) >= 11, "seek à l'arrêt lance la lecture", st)

        # Suppression du morceau en cours
        c.cmd("play 0")
        self.playing(c, 0)
        self.ok(c.cmd("delete 0"), "delete du morceau en cours")
        st = self.playing(c, 0)
        cur = c.cmd("currentsong").get("file")
        self.check(st.get("state") == "play" and cur == self.paths[1], "le suivant enchaîne", (st.get("state"), cur))
        self.ok(c.cmd("clear"), "clear pendant la lecture")
        self.check(self.wait_status(c, lambda s: s.get("state") == "stop", 3).get("state") == "stop",
                   "clear arrête la lecture")

        # Volume
        self.section("Volume et options")
        self.ok(c.cmd(f"setvol {self.volume + 4}"), "setvol")
        self.check(c.status().get("volume") == str(self.volume + 4), "volume réglé", c.status().get("volume"))
        self.ok(c.cmd("volume -2"), "volume -2")
        self.check(c.status().get("volume") == str(self.volume + 2), "volume relatif")
        self.ok(c.cmd("volume +1"), "volume +1")
        self.check(c.cmd("getvol").get("volume") == str(self.volume + 3), "getvol")
        for bad in ("setvol 101", "setvol -1", "setvol abc", "volume 200"):
            self.ack(c.cmd(bad), 2, bad)
        c.cmd(f"setvol {self.volume}")

        # Options
        for opt, vals in (("repeat", ("1", "0")), ("random", ("1", "0")), ("consume", ("1", "0")),
                          ("single", ("1", "oneshot", "0"))):
            for v in vals:
                self.ok(c.cmd(f"{opt} {v}"), f"{opt} {v}")
                self.check(c.status().get(opt) == v, f"status {opt}: {v}", c.status().get(opt))
            self.ack(c.cmd(f"{opt} 2"), 2, f"{opt} 2")
        self.ok(c.cmd("consume oneshot"), "consume oneshot")
        c.cmd("consume 0")

        # Fins de morceau
        self.section("Fins de morceau (repeat, single, consume, random)")
        self.end_of_song(c)

    def near_end(self, c, song):
        """Lance le morceau song et saute à 2,5 s de sa fin."""
        c.cmd(f"play {song}")
        self.playing(c, song)
        target = self.durations[song] - 2.5
        c.cmd(f"seekcur {target:.1f}")
        self.wait_status(c, lambda s: self.elapsed(s) >= target - 1.5 or s.get("song") != str(song), 4)

    def end_of_song(self, c):
        self.setup_queue(c)
        self.near_end(c, 0)
        st = self.playing(c, 1, 12)
        self.check(st.get("song") == "1" and st.get("state") == "play", "fin de morceau → le suivant", st)

        self.near_end(c, 2)
        st = self.wait_status(c, lambda s: s.get("state") == "stop", 12)
        self.check(st.get("state") == "stop", "fin du dernier morceau (sans repeat) → arrêt", st)

        c.cmd("repeat 1")
        self.near_end(c, 2)
        st = self.playing(c, 0, 12)
        self.check(st.get("song") == "0" and st.get("state") == "play", "repeat : du dernier au premier", st)
        c.cmd("repeat 0")

        c.cmd("single 1")
        st = c.status()
        self.near_end(c, 0)
        st = self.wait_status(c, lambda s: s.get("state") == "stop", 12)
        self.check(st.get("state") == "stop", "single : arrêt à la fin du morceau", st)

        c.cmd("repeat 1")
        self.near_end(c, 1)
        self.check(c.status().get("nextsong") == "1", "single + repeat : nextsong = morceau en cours")
        st = self.wait_status(c, lambda s: s.get("song") == "1" and 0 <= self.elapsed(s) < 2.0, 12)
        self.check(st.get("song") == "1" and st.get("state") == "play" and self.elapsed(st) < 2.0,
                   "single + repeat : le même morceau recommence", st)
        c.cmd("repeat 0")

        c.cmd("single oneshot")
        self.near_end(c, 0)
        st = self.wait_status(c, lambda s: s.get("state") == "stop", 12)
        self.check(st.get("state") == "stop" and st.get("single") == "0", "single oneshot : arrêt puis single 0",
                   st)
        c.cmd("single 0")

        c.cmd("consume 1")
        self.near_end(c, 0)
        st = self.wait_status(c, lambda s: s.get("playlistlength") == "2", 12)
        cur = c.cmd("currentsong").get("file")
        self.check(st.get("playlistlength") == "2" and cur == self.paths[1] and st.get("state") == "play",
                   "consume : le morceau joué quitte la file", (st.get("playlistlength"), cur))
        c.cmd("consume 0")

        self.setup_queue(c)
        c.cmd("random 1")
        c.cmd("play 0")
        self.playing(c, 0)
        c.cmd("next")
        st = self.wait_status(c, lambda s: s.get("song") not in (None, "0") and s.get("state") == "play", 6)
        self.check(st.get("song") in ("1", "2"), "random : next joue un autre morceau", st.get("song"))
        c.cmd("random 0")
        c.cmd("stop")

    # ======================= Clic dans M.A.L.P. =======================

    def t_malp(self, c):
        self.section("Clic sur un morceau dans M.A.L.P.")
        p0, p1, p2 = self.paths
        self.setup_queue(c)
        c.cmd("delete 1:")  # file : [p0], arrêtée

        # « Ajouter » (réglage par défaut de M.A.L.P.) : add seulement, la lecture ne démarre pas.
        self.ok(c.cmd("add " + q(p1)), "Ajouter : add")
        st = c.status()
        self.check(st.get("state") == "stop" and st.get("playlistlength") == "2",
                   "Ajouter : morceau en file, lecture non lancée (comme un vrai serveur MPD)", st.get("state"))

        # « Jouer la piste » sur un morceau déjà en file : playlistfind file, puis play <pos>.
        r = c.cmd("playlistfind file " + q(p1)).songs()
        self.check([s.get("pos") for s in r] == ["1"], "Jouer : playlistfind trouve le morceau", r)
        self.ok(c.cmd("play " + r[0]["pos"] if r else "play 1"), "Jouer : play <position>")
        st = self.playing(c, 1)
        self.check(st.get("state") == "play" and c.cmd("currentsong").get("file") == p1, "Jouer : le morceau joue",
                   st)

        # « Jouer la piste » sur un morceau absent : add, status, play <longueur - 1>.
        self.check(c.cmd("playlistfind file " + q(p2)).songs() == [], "Jouer : playlistfind vide si absent")
        c.cmd("add " + q(p2))
        n = int(c.status()["playlistlength"])
        self.ok(c.cmd(f"play {n - 1}"), "Jouer : play <dernière position>")
        st = self.playing(c, n - 1)
        self.check(st.get("state") == "play" and c.cmd("currentsong").get("file") == p2,
                   "Jouer : le morceau ajouté joue", st)

        # « Jouer après la piste actuelle » : status, puis addid "url"  <courant + 1>.
        cur = int(c.status()["song"])
        r = c.cmd("addid " + q(p0) + f"  {cur + 1}")
        self.check(r.ok and c.cmd(f"playlistinfo {cur + 1}").get("file") == p0, "Jouer ensuite : inséré après",
                   r)
        self.check(c.status().get("nextsong") == str(cur + 1), "Jouer ensuite : c'est le prochain morceau")

        # « Ajouter au début » : addid "url"  0.
        r = c.cmd("addid " + q(p2) + "  0")
        self.check(r.ok and self.queue_files(c)[0] == p2, "Ajouter au début : en tête de file", r)
        st = c.status()
        self.check(st.get("state") == "play" and c.cmd("currentsong").get("file") == p2 and st.get("song") == "3",
                   "la lecture en cours continue (position décalée)", st)

        # Lecture d'un dossier (« Jouer » sur un dossier) : clear, add dossier, play 0.
        c.cmd("clear")
        c.cmd("add " + q(self.folder))
        self.ok(c.cmd("play 0"), "Jouer un dossier : clear, add, play 0")
        self.check(self.playing(c, 0).get("state") == "play", "Jouer un dossier : lecture")
        c.cmd("stop")

    # ======================= Listes de commandes =======================

    def t_command_lists(self, c):
        self.section("Listes de commandes")
        r = c.cmd_list(["status", "currentsong", "ping"])
        self.check(r.ok and r.get("state") is not None, "command_list_begin … end", r)
        r = c.cmd_list(["ping", "status", "ping"], ok_mode=True)
        self.check(r.ok and [k for k, _ in r.pairs].count("list_OK") == 3, "command_list_ok_begin : 3 list_OK")
        r = c.cmd_list([])
        self.ok(r, "liste vide")
        r = c.cmd_list([f"setvol {self.volume + 1}", "xyzzy", f"setvol {self.volume + 2}"])
        self.check(r.ack and r.ack.startswith("ACK [5@1]"), "erreur dans une liste : ACK [5@1]", r.ack)
        self.check(c.status().get("volume") == str(self.volume + 1), "liste : arrêt à la commande fautive",
                   c.status().get("volume"))
        r = c.cmd_list(["ping", "play 999"], ok_mode=True)
        self.check(r.ack and r.ack.startswith("ACK [2@1]") and r.pairs == [("list_OK", "")],
                   "liste ok : list_OK puis ACK [2@1]", (r.pairs, r.ack))
        c.cmd(f"setvol {self.volume}")

    # ======================= idle =======================

    def drain(self, b):
        b.send("idle")
        time.sleep(0.2)
        b.send("noidle")
        b.read()

    def idle_expect(self, b, subsystems, action, expected, msg, unexpected_action=None):
        sub = (" " + " ".join(subsystems)) if subsystems else ""
        self.drain(b)
        b.send("idle" + sub)
        self.used.add("idle")
        if unexpected_action:
            unexpected_action()
            self.check(not b.has_data(0.6), f"{msg} : pas réveillé par un autre sous-système")
        t0 = time.monotonic()
        action()
        if not b.has_data(5):
            self.check(False, msg, "aucune réponse en 5 s")
            b.send("noidle")
            b.read()
            return
        r = b.read()
        got = set(r.values("changed"))
        self.check(r.ok and expected & got, msg, f"reçu {sorted(got)} en {time.monotonic() - t0:.2f} s")

    def t_idle(self, c):
        self.section("idle (changements vus par une autre connexion)")
        b = self.connect()
        self.setup_queue(c)
        self.idle_expect(b, [], lambda: c.cmd(f"setvol {self.volume + 1}"), {"mixer"}, "idle : volume → mixer")
        self.idle_expect(b, ["playlist"], lambda: c.cmd("add " + q(self.paths[0])), {"playlist"},
                         "idle playlist : add → playlist", unexpected_action=lambda: c.cmd("repeat 1"))
        self.idle_expect(b, ["player"], lambda: c.cmd("play 0"), {"player"}, "idle player : play → player")
        self.idle_expect(b, ["player"], lambda: c.cmd("pause 1"), {"player"}, "idle player : pause → player")
        self.idle_expect(b, ["options"], lambda: c.cmd("repeat 0"), {"options"}, "idle options : repeat → options")
        self.idle_expect(b, ["database", "update"], lambda: c.cmd("update"), {"database", "update"},
                         "idle database update : update")
        c.cmd("stop")
        c.cmd(f"setvol {self.volume}")
        self.drain(b)
        b.send("idle")
        t0 = time.monotonic()
        b.send("noidle")
        r = b.read()
        self.check(r.ok and time.monotonic() - t0 < 1.0, "noidle répond tout de suite", r)
        # Au repos, rien ne doit réveiller idle : en particulier, le volume ne bouge pas tout seul.
        vol0 = c.status().get("volume")
        b.send("idle")
        woke = b.has_data(3)
        changed = b.read().values("changed") if woke else []
        vol1 = c.status().get("volume")
        self.check(not woke, "au repos, idle reste en attente 3 s",
                   f"réveillé par {changed}, volume {vol0} → {vol1} sans commande (touche tactile ?)")
        # Pendant idle, seule noidle est permise : toute autre commande ferme la connexion (comme MPD).
        if woke:
            b.send("idle stored_playlist")  # sous-système que rien ne change
            time.sleep(0.2)
        b.send("status")
        self.check(b.closed_by_server(3), "commande pendant idle : connexion fermée")
        b.close()
        # Changements faits par la connexion elle-même : signalés aussi (comme MPD).
        self.drain(c)
        c.cmd(f"setvol {self.volume + 1}")
        c.send("idle mixer")
        self.check(c.has_data(2) and "mixer" in c.read().values("changed"), "idle signale ses propres changements")
        c.cmd(f"setvol {self.volume}")

    # ======================= Plusieurs clients =======================

    def t_concurrency(self, c):
        self.section("Plusieurs clients en même temps")
        results, errors = {}, []

        def heavy(name):
            try:
                for attempt in range(10):
                    try:
                        x = self.connect()
                        break
                    except ConnectionRefusedError:
                        if attempt == 9:
                            raise
                        time.sleep(0.3)
                t0 = time.monotonic()
                r = x.cmd("listallinfo")
                results[name] = (len(r.songs()), time.monotonic() - t0)
                x.close()
            except Exception as e:  # noqa: BLE001
                errors.append(f"{name} : {e}")

        th = [threading.Thread(target=heavy, args=(n,)) for n in ("B", "C")]
        lat = []
        for x in th:
            x.start()
        while any(x.is_alive() for x in th):
            t0 = time.monotonic()
            c.cmd("status")
            lat.append(time.monotonic() - t0)
            time.sleep(0.1)
        for x in th:
            x.join()
        self.check(not errors, "deux listallinfo en parallèle", errors)
        self.check(all(n == self.total for n, _ in results.values()), "chacun reçoit toute la bibliothèque",
                   {k: v[0] for k, v in results.items()})
        worst = max(lat) if lat else 0
        self.check(worst < 1.0, "status reste rapide pendant ce temps", f"pire : {worst:.2f} s")
        print(f"  info    listallinfo ×2 : " + ", ".join(f"{k} {v[1]:.1f} s" for k, v in sorted(results.items()))
              + f" ; status au pire {worst * 1000:.0f} ms sur {len(lat)}")

    # ======================= Bilan =======================

    def coverage(self):
        self.section("Couverture")
        tested = self.used | {"idle", "noidle"}
        missing = [x for x in self.commands if x not in tested]
        self.check(not missing, "toutes les commandes annoncées ont été essayées", missing)
        print(f"  info    {len(self.commands)} commandes annoncées, {len(self.commands) - len(missing)} essayées")

    def report(self):
        print("\n== Commandes les plus lentes ==")
        for d, text in sorted(self.timings, reverse=True)[:8]:
            print(f"  {d * 1000:7.0f} ms  {text}")
        slow = [t for d, t in self.timings if d >= SLOW_S]
        if slow:
            print(f"  {len(slow)} commande(s) de plus de {SLOW_S:.0f} s")
        print(f"\n{self.checks} vérifications, {len(self.failures)} échec(s), {len(self.notes)} remarque(s)")
        for f in self.failures:
            print("  ÉCHEC  ", f)
        for n in self.notes:
            print("  note   ", n)
        return 1 if self.failures else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().split("\n")[0])
    ap.add_argument("host", nargs="?", default="enceinte.local")
    ap.add_argument("--port", type=int, default=6600)
    ap.add_argument("--password", default=os.environ.get("MPD_PASSWORD"))
    ap.add_argument("--folder", help="dossier de test (au moins 3 morceaux de plus de 30 s)")
    ap.add_argument("--volume", type=int, default=5, help="volume pendant le test (défaut : 5)")
    ap.add_argument("--yes", action="store_true", help="lancer même si l'enceinte est en train de jouer")
    ap.add_argument("-v", "--verbose", action="store_true", help="afficher aussi les vérifications réussies")
    args = ap.parse_args()
    t = Tester(args)
    try:
        c = t.connect()
    except ConnectionRefusedError as e:
        sys.exit(f"Connexion refusée : {e} (fermer les applications MPD connectées à l'enceinte)")
    except OSError as e:
        sys.exit(f"Enceinte injoignable sur {args.host}:{args.port} : {e}")
    except PermissionError as e:
        sys.exit(str(e))
    print(f"Enceinte {args.host}:{args.port} — {c.greeting}")
    if c.status().get("state") == "play" and not args.yes:
        sys.exit("L'enceinte est en train de jouer : relancer avec --yes pour l'interrompre le temps du test")
    t.save_state(c)
    t0 = time.monotonic()
    try:
        t.discover(c)
        c.close()
        t.t_connection()
        c = t.connect()
        t.t_misc(c)
        t.t_library(c)
        t.t_queue(c)
        t.t_malp(c)
        t.t_playback(c)
        t.t_command_lists(c)
        t.t_idle(c)
        t.t_concurrency(c)
        t.coverage()
    except (ConnectionError, socket.timeout, OSError) as e:
        t.check(False, "connexion perdue en cours de test", f"{type(e).__name__}: {e}")
    finally:
        try:
            c.close()
        except Exception:  # noqa: BLE001
            pass
        print()
        t.restore_state()
    print(f"Durée : {time.monotonic() - t0:.0f} s")
    sys.exit(t.report())


if __name__ == "__main__":
    main()
