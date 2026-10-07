#!/bin/bash
# Publie le firmware compilé pour les mises à jour automatiques des enceintes.
#
# Usage : tools/publish_ota.sh <dossier_publié_par_le_serveur_web> [dossier_build]
#
# Copie build/enceinte.bin sous le nom enceinte-<version>.bin et (ré)écrit
# manifest.json. La version est celle de version.txt, vérifiée dans le binaire.
# Dans la page d'administration de l'enceinte, indiquer l'adresse du manifeste,
# par exemple https://mon-serveur.fr/enceinte/manifest.json
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${1:?usage : $0 <dossier_du_serveur> [dossier_build]}"
BUILD="${2:-$ROOT/build}"
BIN="$BUILD/enceinte.bin"
VERSION="$(tr -d ' \n' < "$ROOT/version.txt")"

[ -f "$BIN" ] || { echo "binaire introuvable : $BIN (lancer idf.py build)"; exit 1; }

# Version et nom de projet inscrits dans le binaire (esp_app_desc_t, décalage 0x20)
read -r BIN_PROJECT BIN_VERSION < <(python3 - "$BIN" <<'PY'
import struct, sys
data = open(sys.argv[1], "rb").read(0x20 + 256)
magic, = struct.unpack_from("<I", data, 0x20)
if magic != 0xABCD5432:
    sys.exit("descripteur d'application introuvable")
version = data[0x30:0x50].split(b"\0")[0].decode()
project = data[0x50:0x70].split(b"\0")[0].decode()
print(project, version)
PY
)
if [ "$BIN_PROJECT" != "enceinte" ] || [ "$BIN_VERSION" != "$VERSION" ]; then
    echo "binaire incohérent : projet '$BIN_PROJECT' version '$BIN_VERSION' (version.txt : $VERSION)"
    echo "recompiler après avoir modifié version.txt"
    exit 1
fi

mkdir -p "$DEST"
NAME="enceinte-$VERSION.bin"
cp "$BIN" "$DEST/$NAME.tmp"
mv "$DEST/$NAME.tmp" "$DEST/$NAME"
SHA="$(sha256sum "$DEST/$NAME" | cut -d' ' -f1)"
cat > "$DEST/manifest.json.tmp" <<JSON
{
  "version": "$VERSION",
  "url": "$NAME",
  "sha256": "$SHA",
  "date": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
JSON
mv "$DEST/manifest.json.tmp" "$DEST/manifest.json"  # remplacement atomique : jamais de manifeste à moitié écrit
echo "publié : $DEST/$NAME (version $VERSION)"
