#!/bin/bash
# Tests sur PC (sans le matériel).
#  1. tests unitaires des modules purs (+ fichiers audio réels générés par ffmpeg/lame,
#     réglages en JSON si la bibliothèque cJSON est installée : paquet libcjson-dev) ;
#  2. contrôleur de cartes avec le vrai lecteur (I2S et décodeur simulés) ;
#  3. intégration MPD : vrai lecteur + vrai serveur pilotés par python-mpd2 ;
#  4. copie des réglages et des associations sur la carte SD (carte clonée, modifiée, absente).
# Tout est compilé avec AddressSanitizer et UndefinedBehaviorSanitizer.
#
# Fichiers audio d'exemple générés avec ffmpeg (libopus, libvorbis), lame et flac.
# Usage : test/host/run_tests.sh [dossier_de_travail]
#   PYTHON=/chemin/vers/python (avec python-mpd2) pour l'étape 3.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
MAIN="$HERE/../../main"
WORK="${1:-$(mktemp -d)}"
FIX="$WORK/fixtures"
PY="${PYTHON:-python3}"
mkdir -p "$FIX"

CFLAGS=(-std=gnu17 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-missing-field-initializers
        -fsanitize=address,undefined -fno-omit-frame-pointer -pthread)

gen_fixtures() {
    local tags=(-metadata "title=Été indien" -metadata "artist=日本の歌手" -metadata "album=Album test"
                -metadata "track=3/12" -metadata "date=2024" -metadata "genre=Chanson")
    local q=(-hide_banner -loglevel error -y)
    ffmpeg "${q[@]}" -f lavfi -i "sine=frequency=440:duration=5:sample_rate=44100" -ac 2 "$FIX/tone.wav"
    ffmpeg "${q[@]}" -i "$FIX/tone.wav" -c:a libmp3lame -b:a 128k -write_xing 0 -id3v2_version 3 "${tags[@]}" "$FIX/cbr.mp3"
    ffmpeg "${q[@]}" -i "$FIX/tone.wav" -c:a libmp3lame -q:a 4 -id3v2_version 4 "${tags[@]}" "$FIX/vbr.mp3"
    ffmpeg "${q[@]}" -f lavfi -i "testsrc=size=600x600:rate=1" -frames:v 1 "$FIX/cover.png"
    ffmpeg "${q[@]}" -i "$FIX/cbr.mp3" -i "$FIX/cover.png" -map 0:a -map 1:v -c copy -id3v2_version 3 \
        -metadata:s:v "title=Cover" -metadata:s:v "comment=Cover (front)" "$FIX/art.mp3"
    lame --quiet --cbr -b 96 --id3v1-only --tt "Vieux titre" --ta "Vieil artiste" --tl "Vieil album" \
        --ty 1999 --tn 7 "$FIX/tone.wav" "$FIX/v1.mp3"
    ffmpeg "${q[@]}" -i "$FIX/tone.wav" -c:a flac -metadata "title=Titre FLAC" -metadata "artist=Artiste FLAC" \
        -metadata "track=5" "$FIX/tone.flac"
    ffmpeg "${q[@]}" -f lavfi -i "sine=frequency=330:duration=30:sample_rate=44100" -ac 2 "$FIX/long.wav"
    ffmpeg "${q[@]}" -i "$FIX/long.wav" -c:a libopus -b:a 96k -metadata "title=Chouette hulotte" \
        -metadata "artist=Oiseaux de France" "$FIX/long.opus"
    ffmpeg "${q[@]}" -i "$FIX/long.wav" -c:a libvorbis -q:a 3 -metadata "title=Merle noir" "$FIX/long.ogg"
    # Opus dont les métadonnées commencent par une « pochette » de 400 Ko
    python3 -c "import base64, os; print(';FFMETADATA1'); print('METADATA_BLOCK_PICTURE=' + base64.b64encode(os.urandom(300000)).decode()); print('title=Grosse pochette')" > "$FIX/bigtags.txt"
    ffmpeg "${q[@]}" -i "$FIX/long.wav" -i "$FIX/bigtags.txt" -map_metadata 1 -c:a libopus -b:a 64k "$FIX/bigtags.opus"
    flac --silent --force -S 2s -T "TITLE=Rouge-gorge" -o "$FIX/long_seektable.flac" "$FIX/long.wav"
    flac --silent --force -S- -o "$FIX/long_noseektable.flac" "$FIX/long.wav"
    python3 -c "import random, sys; random.seed(1234); sys.stdout.buffer.write(bytes(random.getrandbits(8) for _ in range(20000)))" > "$FIX/garbage.mp3"
    printf 'ID3\x03\x00\x00\x7f\x7f\x7f\x7f' > "$FIX/truncated.mp3"
}

HAVE_MEDIA=0
if command -v ffmpeg > /dev/null && command -v lame > /dev/null && command -v flac > /dev/null; then
    gen_fixtures
    HAVE_MEDIA=1
else
    echo "ffmpeg/lame/flac absents : tests utilisant des fichiers audio ignorés"
fi

echo "== 1. Tests unitaires"
JSON=()
if [ -f /usr/include/cjson/cJSON.h ]; then
    JSON=(-DHAVE_CJSON -I/usr/include/cjson "$MAIN/config_json.c" -lcjson)
fi
gcc "${CFLAGS[@]}" -DDNS_HOST_TEST -I"$MAIN" -I"$HERE" -I"$HERE/stubs" \
    "$HERE"/test_*.c "$MAIN/util.c" "$MAIN/pn532_frame.c" "$MAIN/session.c" "$MAIN/dns_server.c" \
    "$MAIN/mpd_proto.c" "$MAIN/media_info.c" "$MAIN/dsp.c" "$MAIN/touch_keys.c" "${JSON[@]}" -lm -o "$WORK/tests"
if [ $HAVE_MEDIA = 1 ]; then "$WORK/tests" "$FIX"; else "$WORK/tests"; fi

echo "== 2. Contrôleur de cartes (vrai lecteur)"
rm -rf "$WORK/sd_ctrl"
gcc "${CFLAGS[@]}" -DMUSIC_ROOT="\"$WORK/sd_ctrl\"" -DCONFIG_ENC_RESUME_TIMEOUT_S=2 \
    -I"$HERE/stubs" -I"$MAIN" -I"$HERE" "$HERE/controller_test.c" "$HERE/shims.c" "$HERE/mocks.c" \
    "$MAIN/controller.c" "$MAIN/session.c" "$MAIN/player.c" "$MAIN/dsp.c" "$MAIN/media_info.c" "$MAIN/util.c" \
    "$MAIN/changes.c" -lm -o "$WORK/controller_test"
if [ $HAVE_MEDIA = 1 ]; then
    ASAN_OPTIONS=detect_leaks=0 "$WORK/controller_test" "$FIX"
else
    ASAN_OPTIONS=detect_leaks=0 "$WORK/controller_test"
fi

echo "== 3. Intégration MPD"
if [ $HAVE_MEDIA = 1 ] && "$PY" -c "import mpd" 2> /dev/null; then
    gcc "${CFLAGS[@]}" -DMUSIC_ROOT="\"$WORK/sd\"" -I"$HERE/stubs" -I"$MAIN" -I"$HERE" \
        "$HERE/mpd_host_main.c" "$HERE/shims.c" "$HERE/mocks.c" "$MAIN/player.c" "$MAIN/dsp.c" "$MAIN/mpd_server.c" \
        "$MAIN/mpd_proto.c" "$MAIN/media_info.c" "$MAIN/util.c" "$MAIN/changes.c" -lm -o "$WORK/mpd_host"
    "$PY" "$HERE/mpd_integration.py" "$WORK/mpd_host" "$WORK/sd" "$FIX"
else
    echo "python-mpd2 ou fichiers audio absents : étape ignorée (pip install python-mpd2)"
fi
echo "== 4. Réglages et associations sur la carte SD"
if [ ${#JSON[@]} -gt 0 ]; then
    gcc "${CFLAGS[@]}" -DMUSIC_ROOT="\"$WORK/sd_backup\"" -I"$HERE/stubs" -I"$MAIN" -I"$HERE" -I/usr/include/cjson \
        "$HERE/backup_test.c" "$HERE/nvs_mem.c" "$HERE/shims.c" "$MAIN/backup.c" "$MAIN/cards.c" \
        "$MAIN/config_json.c" "$MAIN/util.c" -lcjson -lm -o "$WORK/backup_test"
    ASAN_OPTIONS=detect_leaks=0 "$WORK/backup_test"
else
    echo "bibliothèque cJSON absente : étape ignorée (paquet libcjson-dev)"
fi
echo "== Tous les tests sont passés"
