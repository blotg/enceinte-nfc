#!/usr/bin/env python3
"""
Test d'intégration : le vrai lecteur et le vrai serveur MPD (compilés pour PC,
mpd_host) pilotés par un vrai client MPD (python-mpd2).

Usage : mpd_integration.py <binaire mpd_host> <dossier SD> <dossier fixtures>
"""
import os
import shutil
import socket
import subprocess
import sys
import threading
import time

import mpd

PORT = 16600
FAILURES = []


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)
        print("ÉCHEC :", msg)


def wait_for(pred, timeout=5.0, step=0.02):
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return True
        time.sleep(step)
    return False


def build_sd(sd, fix):
    shutil.rmtree(sd, ignore_errors=True)
    os.makedirs(f"{sd}/Comptines")
    os.makedirs(f"{sd}/Rock/Album A")
    os.makedirs(f"{sd}/Long")
    os.makedirs(f"{sd}/Bad")
    shutil.copy(f"{fix}/cbr.mp3", f"{sd}/Comptines/01 - Une souris verte.mp3")
    shutil.copy(f"{fix}/vbr.mp3", f"{sd}/Comptines/02 - Frère Jacques.mp3")
    shutil.copy(f"{fix}/art.mp3", f"{sd}/Comptines/10 - Dernière.mp3")
    shutil.copy(f"{fix}/cover.png", f"{sd}/Comptines/cover.png")
    shutil.copy(f"{fix}/cbr.mp3", f"{sd}/Comptines/._01 - Une souris verte.mp3")  # métadonnées macOS
    shutil.copy(f"{fix}/tone.flac", f"{sd}/Rock/Album A/track1.flac")
    shutil.copy(f"{fix}/tone.wav", f"{sd}/Rock/Album A/track2.wav")
    with open(f"{sd}/Long/long.mp3", "wb") as f:
        f.write(bytes(12 * 1024 * 1024))  # ~70 s de « PCM » pour le décodeur simulé
    with open(f"{sd}/Bad/01 corrupt.mp3", "wb") as f:
        for _ in range(40):
            f.write(b"CORRUPT" + bytes(4096 - 7))
    shutil.copy(f"{fix}/cbr.mp3", f"{sd}/Bad/02 after.mp3")
    with open(f"{sd}/Comptines/notes.txt", "w") as f:
        f.write("pas de l'audio")


class Server:
    def __init__(self, binary, password=None):
        env = dict(os.environ, SHIM_SPEEDUP="20", ASAN_OPTIONS="detect_leaks=0")
        if password:
            env["MPD_PASSWORD"] = password
        self.proc = subprocess.Popen([binary], stdout=subprocess.PIPE, text=True, env=env)
        self.events = []
        line = self.proc.stdout.readline()
        assert line.strip() == "READY", line
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.proc.stdout:
            self.events.append(line.strip())

    def stop(self):
        self.proc.terminate()
        self.proc.wait(5)
        return self.proc.returncode


def client():
    c = mpd.MPDClient()
    c.timeout = 10
    c.connect("127.0.0.1", PORT)
    return c


def test_library(c):
    root = c.lsinfo("")
    dirs = [e["directory"] for e in root if "directory" in e]
    check(dirs == ["Bad", "Comptines", "Long", "Rock"], f"lsinfo racine : {dirs}")

    songs = [e for e in c.lsinfo("Comptines") if "file" in e]
    names = [s["file"] for s in songs]
    check(names == ["Comptines/01 - Une souris verte.mp3", "Comptines/02 - Frère Jacques.mp3",
                    "Comptines/10 - Dernière.mp3"], f"ordre naturel / fichiers cachés : {names}")
    check(songs[0].get("title") == "Été indien", f"tag titre : {songs[0]}")
    check(songs[0].get("artist") == "日本の歌手", "tag artiste UTF-16")
    check(abs(float(songs[0].get("duration", 0)) - 5.0) < 0.2, f"durée : {songs[0].get('duration')}")

    all_files = [e["file"] for e in c.listall("") if "file" in e]
    check("Rock/Album A/track1.flac" in all_files and len(all_files) == 8, f"listall : {all_files}")
    info = c.listallinfo("Rock")
    check(any(e.get("title") == "Titre FLAC" for e in info), "listallinfo FLAC")

    check(c.list("album") == [{"album": "Album test"}] or c.list("album") == ["Album test"],
          f"list album : {c.list('album')}")
    found = c.search("title", "ÉTÉ")
    check(len(found) == 4, f"search insensible à la casse : {len(found)}")  # 3 Comptines + Bad/02
    found = c.find("(artist == '日本の歌手')")
    check(len(found) >= 3, f"find expression : {len(found)}")
    found = c.find("(base 'Comptines')")
    check(len(found) == 3, f"find base : {len(found)}")
    cnt = c.count("album", "Album test")
    check(int(cnt["songs"]) >= 3, f"count : {cnt}")

    pls = [p["playlist"] for p in c.listplaylists()]
    check(pls == ["Bad", "Comptines", "Long", "Rock"], f"listplaylists : {pls}")
    check(len(c.listplaylist("Rock")) == 2, "listplaylist")

    art = c.albumart("Comptines/01 - Une souris verte.mp3")
    with open(os.path.join(SD, "Comptines/cover.png"), "rb") as f:
        check(art.get("binary") == f.read(), "albumart")
    try:
        c.albumart("Rock/Album A/track1.flac")
        check(False, "albumart sans pochette devrait échouer")
    except mpd.CommandError:
        pass
    try:
        c.lsinfo("../etc")
        check(False, "traversée de répertoire acceptée")
    except mpd.CommandError:
        pass
    try:
        c.add("Comptines/notes.txt")
        check(False, "fichier non audio ajouté")
    except mpd.CommandError:
        pass


def test_playback(c, srv):
    c.clear()
    c.add("Comptines")
    pl = c.playlistinfo()
    check(len(pl) == 3 and pl[0]["pos"] == "0", f"playlistinfo : {len(pl)}")
    st = c.status()
    check(st["state"] == "stop" and st["playlistlength"] == "3", f"status initial : {st}")

    srv.events.clear()
    c.play()
    st = c.status()
    check(st["state"] == "play" and st["song"] == "0", f"play : {st}")
    cur = c.currentsong()
    check(cur.get("file") == "Comptines/01 - Une souris verte.mp3" and cur.get("pos") == "0", f"currentsong : {cur}")

    # idle : changement de morceau à la fin du premier
    changed = c.idle("player")
    check("player" in changed, f"idle player : {changed}")
    check(wait_for(lambda: srv.events.count("EVENT queue_end") == 1, 10), "fin de file signalée")
    st = c.status()
    check(st["state"] == "stop", f"état après la fin : {st}")
    check("song" in st, "le morceau courant reste connu après la fin")

    # pause / reprise
    c.clear()
    c.add("Long")
    c.play(0)
    time.sleep(0.3)
    c.pause(1)
    e1 = float(c.status()["elapsed"])
    time.sleep(0.4)
    e2 = float(c.status()["elapsed"])
    check(c.status()["state"] == "pause" and abs(e2 - e1) < 0.01, f"pause : {e1} -> {e2}")
    c.pause(0)
    time.sleep(0.4)
    e3 = float(c.status()["elapsed"])
    check(c.status()["state"] == "play" and e3 > e2 + 2, f"reprise : {e2} -> {e3}")
    c.pause()  # bascule
    check(c.status()["state"] == "pause", "pause sans argument = bascule")
    c.pause()
    check(c.status()["state"] == "play", "bascule retour")

    # recherche dans un fichier non MP3 réel : refusée proprement
    try:
        c.seekcur(10)
        check(False, "seek accepté sur un fichier sans durée connue")
    except mpd.CommandError as e:
        check("seekable" in str(e), f"message seek : {e}")

    # recherche dans un vrai MP3 (table Xing)
    c.clear()
    c.add("Comptines/02 - Frère Jacques.mp3")
    c.play(0)
    c.pause(1)
    c.seekcur(2.5)
    st = c.status()
    check(st["state"] == "pause", f"seek en pause conserve la pause : {st['state']}")
    check(abs(float(st["elapsed"]) - 2.5) < 0.3, f"seekcur : {st['elapsed']}")
    c.stop()

    # volume
    c.setvol(55)
    check(c.status()["volume"] == "55", "setvol")
    _, out = raw_session([b"getvol\n"], 0.5)
    check(out == b"volume: 55\nOK\n", f"getvol : {out}")
    c.volume(-5)
    check(c.status()["volume"] == "50", "volume relatif")
    try:
        c.setvol(150)
        check(False, "volume 150 accepté")
    except mpd.CommandError:
        pass

    # options
    c.repeat(1)
    c.random(1)
    c.single("oneshot")
    c.consume(1)
    st = c.status()
    check(st["repeat"] == "1" and st["random"] == "1" and st["single"] == "oneshot" and st["consume"] == "1",
          f"options : {st}")
    c.repeat(0)
    c.random(0)
    c.single(0)
    c.consume(0)


def test_queue(c):
    c.clear()
    c.add("Comptines")
    c.add("Rock")
    ids = [s["id"] for s in c.playlistinfo()]
    check(len(ids) == 5, "5 morceaux")
    c.move(0, 4)
    order = [s["id"] for s in c.playlistinfo()]
    check(order == ids[1:] + ids[:1], f"move : {order}")
    c.swap(0, 1)
    order2 = [s["id"] for s in c.playlistinfo()]
    check(order2[0] == order[1] and order2[1] == order[0], "swap")
    c.swapid(order2[0], order2[4])
    order3 = [s["id"] for s in c.playlistinfo()]
    check(order3[0] == order2[4] and order3[4] == order2[0], "swapid")
    new_id = c.addid("Comptines/10 - Dernière.mp3", 1)
    check(c.playlistinfo()[1]["id"] == new_id, "addid avec position")
    c.deleteid(new_id)
    c.delete((0, 2))
    check(len(c.playlistinfo()) == 3, "delete plage")
    check(len(c.playlistinfo((1, 3))) == 2, "playlistinfo plage")
    check(len(c.playlistsearch("title", "été")) >= 1, "playlistsearch")

    # suppression du morceau en cours : la lecture continue sur le suivant
    c.clear()
    c.add("Long")
    c.add("Comptines")
    c.play(0)
    playing = c.currentsong()["id"]
    c.deleteid(playing)
    check(wait_for(lambda: c.status().get("songid") not in (None, playing), 3), "lecture continue après suppression")
    check(c.status()["state"] == "play", "toujours en lecture")
    c.clear()
    check(wait_for(lambda: c.status()["state"] == "stop", 3), "clear arrête la lecture")

    # charge une « liste de lecture » (dossier)
    c.load("Rock")
    check(len(c.playlistinfo()) == 2, "load")
    try:
        c.save("x")
        check(False, "save accepté")
    except mpd.CommandError:
        pass

    # plchanges et version de la file
    v = int(c.status()["playlist"])
    c.add("Comptines/01 - Une souris verte.mp3")
    check(int(c.status()["playlist"]) > v, "version de file incrémentée")
    check(len(c.plchanges(v)) == 3, "plchanges")

    # morceau illisible : il est sauté, le suivant est joué
    c.clear()
    c.add("Bad")
    c.play(0)
    check(wait_for(lambda: c.status().get("song") == "1", 5), "fichier corrompu sauté")

    # fichier supprimé entre l'ajout et la lecture
    c.clear()
    tmp = os.path.join(SD, "Comptines", "temp.mp3")
    shutil.copy(os.path.join(SD, "Comptines", "01 - Une souris verte.mp3"), tmp)
    c.add("Comptines/temp.mp3")
    c.add("Comptines/01 - Une souris verte.mp3")
    os.remove(tmp)
    c.play(0)
    check(wait_for(lambda: c.status().get("song") == "1", 5), "fichier disparu sauté")
    st = c.status()
    check("temp.mp3" in st.get("error", ""), f"erreur signalée : {st.get('error')}")
    c.clearerror()
    check("error" not in c.status(), "clearerror")
    c.stop()


def test_radio(c):
    """Webradio : adresse ajoutée par une application et jouée en direct (flux simulé)."""
    c.clear()
    url = "http://test/Long/long.mp3"
    c.add(url)
    sid = c.addid(url, 0)
    songs = c.playlistinfo()
    check(len(songs) == 2 and songs[0]["file"] == url and songs[0]["id"] == sid, f"radio ajoutée : {songs}")
    c.play(0)
    check(wait_for(lambda: c.currentsong().get("title") == "Titre simulé", 5), f"titre de la radio : {c.currentsong()}")
    st = c.status()
    check(st["state"] == "play" and float(st.get("duration", 0)) == 0, f"en direct : {st}")
    try:
        c.seekcur(10)
        check(False, "recherche acceptée dans un direct")
    except mpd.CommandError:
        pass
    c.clear()
    try:
        c.add("ftp://x/y")
        check(False, "schéma d'adresse inconnu accepté")
    except mpd.CommandError:
        pass


def test_protocol(c):
    c.command_list_ok_begin()
    c.status()
    c.currentsong()
    c.ping()
    res = c.command_list_end()
    check(len(res) == 3, f"command_list_ok : {len(res)}")
    try:
        c.command_list_ok_begin()
        c.status()
        c.play(999)
        c.ping()
        c.command_list_end()
        check(False, "erreur dans une liste non signalée")
    except mpd.CommandError as e:
        check("@1]" in str(e) or "Bad song index" in str(e), f"index d'erreur : {e}")
    check("status" in c.commands() and "idle" in c.commands(), "commands")
    check(c.tagtypes()[:2] == ["Artist", "AlbumArtist"], f"tagtypes {c.tagtypes()[:2]}")
    c.tagtypes("clear")
    c.tagtypes("enable", "Title")
    s = [e for e in c.lsinfo("Comptines") if "file" in e][0]
    check("title" in s and "artist" not in s, f"tagtypes filtrés : {s}")
    c.tagtypes("all")
    outs = c.outputs()
    check(len(outs) == 1 and outs[0]["outputenabled"] == "1", "outputs")
    check(c.update() == "1" or c.update() == {"updating_db": "1"}, "update")


def raw_session(lines, read_timeout=2.0):
    s = socket.create_connection(("127.0.0.1", PORT), timeout=read_timeout)
    data = s.recv(100)
    for line in lines:
        s.sendall(line)
    out = b""
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            out += chunk
    except (socket.timeout, ConnectionResetError):
        pass
    s.close()
    return data, out


def test_robustness():
    greet, out = raw_session([b"frobnicate\n"])
    check(greet.startswith(b"OK MPD 0.23"), f"accueil : {greet}")
    check(out.startswith(b"ACK [5@0] {} unknown command"), f"commande inconnue : {out[:60]}")
    _, out = raw_session([b"status \"unterminated\n"])
    check(out.startswith(b"ACK [5@0]"), "guillemet non fermé")
    _, out = raw_session([b"A" * 5000 + b"\n", b"ping\n"])
    check(b"OK" not in out, "ligne trop longue : connexion fermée")
    _, out = raw_session([b"play 1 2 3\n"])
    check(b"wrong number of arguments" in out, "nombre d'arguments")
    # noidle immédiat
    _, out = raw_session([b"idle\n", b"noidle\n"], 1.0)
    check(out.endswith(b"OK\n"), f"idle/noidle : {out}")
    # nombre maximal de clients
    conns = [socket.create_connection(("127.0.0.1", PORT)) for _ in range(3)]
    for s in conns:
        s.recv(100)
    extra = socket.create_connection(("127.0.0.1", PORT))
    data = extra.recv(100)
    check(b"too many clients" in data, f"4e client refusé : {data}")
    extra.close()
    for s in conns:
        s.close()
    time.sleep(0.3)
    c = client()  # les connexions libérées sont réutilisables
    c.ping()
    c.close()


def test_password(binary):
    srv = Server(binary, password="secret")
    try:
        c = client()
        try:
            c.status()
            check(False, "status sans mot de passe accepté")
        except mpd.CommandError as e:
            check("permission" in str(e), f"refus : {e}")
        c.ping()  # autorisé sans mot de passe
        try:
            c.password("faux")
            check(False, "mauvais mot de passe accepté")
        except mpd.CommandError as e:
            check("incorrect password" in str(e), f"mauvais mot de passe : {e}")
        c.password("secret")
        c.status()
        c.close()
    finally:
        srv.stop()


if __name__ == "__main__":
    BINARY, SD, FIX = sys.argv[1], sys.argv[2], sys.argv[3]
    build_sd(SD, FIX)
    server = Server(BINARY)
    try:
        cl = client()
        test_library(cl)
        test_playback(cl, server)
        test_queue(cl)
        test_radio(cl)
        test_protocol(cl)
        cl.close()
        test_robustness()
        alive = server.proc.poll() is None
        check(alive, "le serveur a planté")
    finally:
        code = server.stop()
    test_password(BINARY)
    if FAILURES:
        print(f"{len(FAILURES)} échec(s) d'intégration")
        sys.exit(1)
    print("intégration MPD : OK")
