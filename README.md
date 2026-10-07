# Enceinte NFC (ESP32-S3)

On pose une carte NFC sur l'enceinte : elle joue le dossier de la carte SD associé à
cette carte. On retire la carte : pause. On la repose dans les 10 minutes (sans avoir
posé d'autre carte entre-temps, et si la playlist n'était pas finie) : la lecture
reprend où elle s'était arrêtée. Sinon, elle recommence au début. Le délai et la règle
« autre carte » se règlent globalement et carte par carte.

Firmware ESP-IDF 5.5, successeur du prototype Arduino (`archive/arduino/`).

## Fonctionnalités et faisabilité

| Fonction demandée | État | Remarques |
|---|---|---|
| Carte NFC → lecture d'un dossier | ✅ | MP3, AAC, M4A, FLAC, WAV, OGG/Opus. Sous-dossiers inclus, ordre « naturel » (2 avant 10). |
| Pause au retrait, reprise sous 10 min | ✅ | Délai réglable (0 = toujours) et reprise possible même après une autre carte, en réglage général ou carte par carte. Une carte inconnue compte comme « une autre carte ». |
| Association dossier ↔ carte par l'interface web | ✅ | Mode association : la carte posée est capturée sans lancer la musique. Dossiers nommés librement (accents, espaces). |
| Dépôt de fichiers par l'interface web | ✅ | Fichiers ou dossiers entiers, glisser-déposer. Débit limité par la carte SD en SPI. |
| Mises à jour automatiques | ✅ | Depuis les releases GitHub (ou votre serveur), au démarrage (+1 min) et toutes les N heures. Installation seulement quand l'enceinte est inactive. Retour automatique à l'ancienne version si la nouvelle ne démarre pas. |
| Protocole MPD | ✅ partiel | Large sous-ensemble (cf. plus bas). MPD ne connaît qu'un **mot de passe**, pas d'identifiant. |
| Point d'accès + portail captif | ✅ | Sortie d'usine ou Wi-Fi injoignable : réseau `Enceinte-XXXX`, la page de configuration s'ouvre seule. |
| Mot de passe administrateur en sortie d'usine | ✅ | Stocké haché (PBKDF2-SHA256). |
| Adresse `enceinte.local` | ✅ | mDNS ; nom modifiable. |
| Page d'administration | ✅ | Wi-Fi, mots de passe admin et MPD, fichiers (envoi, déplacement, renommage), cartes, volume maximum, HTTPS, mises à jour, redémarrage. |
| *En plus* | | Bips (carte inconnue, dossier vide), réinitialisation usine (bouton BOOT 10 s), installation manuelle d'un `.bin`, volume maximum. |

**Limites assumées**

- L'interface web est en **HTTP** par défaut. **HTTPS** s'active dans *Réglages → Accès sécurisé* :
  l'enceinte crée alors son propre certificat (auto-signé, ECDSA P-256, valable 20 ans pour
  `nom.local`). Le navigateur affiche un avertissement la première fois, qu'il faut accepter ;
  les accès HTTP depuis le réseau local sont ensuite redirigés vers HTTPS. Le Wi-Fi de
  configuration (point d'accès) reste en HTTP, comme l'exigent les téléphones.
- **Reprise en cours de morceau et recherche de position : MP3, FLAC, Opus, Vorbis, WAV.**
  Pas en AAC ni M4A : le morceau y reprend à son début. Le décodeur d'Espressif ne lit
  qu'en continu ; le firmware lui renvoie l'en-tête du fichier puis saute à la position voulue.
- **M4A** : seulement les fichiers « optimisés pour le streaming » (`moov` avant `mdat`).
- **Carte SD en FAT32** : exFAT n'est pas pris en charge par ESP-IDF. Reformater les cartes de plus
  de 32 Go en FAT32.
- **MPD** : pas de base de données. Les recherches par tag (`list`, `find`, `search`) parcourent
  la carte et sont lentes sur une grosse bibliothèque. Les listes de lecture sont les dossiers
  de premier niveau (lecture seule).

## Matériel

| Élément | Broches ESP32-S3 (modifiables dans `idf.py menuconfig` → *Enceinte*) | Alimentation |
|---|---|---|
| PN532 en mode **HSU** (interrupteurs sur 0/0) | TX ESP **GPIO20** → RX PN532, RX ESP **GPIO19** ← TX PN532 | **3V3** (logique compatible ESP32 garantie) + 10 à 100 µF |
| Carte SD (SPI) | CS 5, MOSI 16, MISO 15, SCK 12 | 3V3 ; **5V** si le module a un régulateur (AMS1117) |
| MAX98357A (I2S) | BCLK 3, LRC 1, DIN 9 | **5V** (puissance, ménage le régulateur 3,3 V) + 220 à 470 µF |
| Réinitialisation usine | bouton BOOT (GPIO0) | |

Les GPIO de l'ESP32-S3 ne tolèrent pas le 5 V. Les entrées I2S du MAX98357A acceptent le
3,3 V même s'il est alimenté en 5 V.

### Quel port USB-C ?

Branchez **le port marqué « UART » (ou « COM »)**, pour le développement comme pour
l'alimentation définitive :

- les broches 19/20 de l'autre port (USB natif) servent au PN532 : ce port n'aurait que
  l'alimentation, sans données ;
- la console série (journaux, plantages, chargeur de démarrage) est configurée sur ce port ;
- le flash et le redémarrage automatique y fonctionnent même si le firmware est planté.

Alimentation : bloc USB **5 V / 2 A**. L'ampli MAX98357A tire des pointes d'environ 1 A, en plus
du PN532 et du Wi-Fi. Si l'ampli est alimenté par la broche 5 V de la carte, vérifiez au
multimètre qu'elle reçoit bien le 5 V de ce port. Sur la carte du prototype (N16R8, deux
ports USB-C), elle était à 0 V : sur ces cartes (type YD-ESP32-S3), la broche 5V n'est
qu'une entrée tant que le pont de soudure **« IN-OUT »**, près des connecteurs USB, n'est
pas fermé. Une fois ce pont fermé, ne jamais alimenter la broche 5V depuis une autre source
pendant que l'USB est branché.

**Wi-Fi** : la puissance d'émission est limitée à 15 dBm (réglable dans menuconfig). Sur la
carte du prototype, rien n'était reçu à 20 dBm (maximum de l'ESP32-S3), alors que 17 et 15 dBm
fonctionnaient. Causes possibles : alimentation 3,3 V qui faiblit pendant les pointes
d'émission, ou antenne de la carte mal adaptée. Un condensateur de 470 µF entre 3V3 et GND,
près du module, permet de trancher : s'il rend les 20 dBm utilisables, c'était l'alimentation.

Module conseillé : ESP32-S3 avec **PSRAM** (N8R8, N16R8…), configuré par défaut en PSRAM
octale. Pour un module N8R2 (PSRAM quad), choisir `SPIRAM_MODE_QUAD` dans menuconfig. Sans
PSRAM, le firmware fonctionne avec des tampons réduits. Flash de 8 Mo minimum.

## Compilation et flash

ESP-IDF **v5.5.5** (installé dans `~/esp/esp-idf-v5.5.5`) :

```bash
. ~/esp/esp-idf-v5.5.5/export.sh
idf.py set-target esp32s3        # une seule fois
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor     # port « UART » de la carte (ou /dev/ttyACM0)
```

Les versions des composants (`esp_audio_codec`, `mdns`) sont figées dans `dependencies.lock`.
La version du firmware est dans `version.txt`.

## Première mise en route

1. Se connecter au Wi-Fi **`Enceinte-XXXX`** depuis un téléphone : la page de configuration
   s'ouvre seule (sinon : `http://192.168.4.1`).
2. Choisir le mot de passe administrateur, le nom de l'enceinte et le Wi-Fi de la maison.
3. L'enceinte se connecte. Elle est ensuite accessible sur **`http://enceinte.local`** (ou par
   son adresse IP, affichée à la fin de l'assistant).

Si le Wi-Fi configuré reste injoignable (2 min en fonctionnement, 30 s au démarrage), le
point d'accès se rouvre en secours. Les nouvelles tentatives de connexion continuent, mais
sont suspendues tant qu'un appareil est connecté au point d'accès : chaque tentative ferait
changer celui-ci de canal.

**Mot de passe oublié** : maintenir le bouton BOOT **10 secondes** (un bip à 3 s, trois bips à
10 s). Réglages et associations sont effacés, la musique est conservée.

## Utilisation

- **Cartes** (onglet *Cartes*) : « Associer une carte », poser la carte, choisir le dossier.
  Compatibilité avec l'ancien firmware : une carte non associée joue le dossier nommé d'après
  son identifiant (ex. `04ab53a96f2681`), s'il existe à la racine de la carte SD.
  Pour un sous-dossier : onglet *Musique*, ouvrir le dossier, « Associer une carte ».
- **Bips** : deux bips = carte inconnue ; trois bips graves = dossier vide ou carte SD absente.
- **Musique** (onglet *Musique*) : envoyer des fichiers ou des dossiers entiers, créer,
  renommer, supprimer, écouter un dossier sans carte, et **déplacer** des fichiers ou des
  dossiers (bouton ↦, ou cases à cocher pour en déplacer ou supprimer plusieurs à la fois).
  Les associations de cartes et les positions de reprise suivent les dossiers déplacés.
- **MPD** : serveur `enceinte.local`, port 6600, dans M.A.L.P. (Android), mpc, Cantata,
  ncmpcpp, Rigelian (iOS)… Mot de passe facultatif dans *Réglages*.
  Pris en charge : lecture, pause, morceau suivant ou précédent, recherche de position,
  volume, aléatoire, répétition, single, consume. Aussi la file d'attente (`add`, `delete`,
  `move`, `swap`, `playlistinfo`, `plchanges`…), le parcours de la carte (`lsinfo`,
  `listall[info]`, `listfiles`), `list`/`find`/`search`/`count` (filtres classiques et
  expressions), `idle`/`noidle`, les listes de commandes et `albumart` (`cover.jpg`/`.png`
  dans le dossier).

### Reprise d'une carte

Retirer une carte met en pause et mémorise sa position (morceau et instant, pour les
16 dernières cartes). Reposée, la carte reprend si sa playlist n'était pas finie et si :

- le **délai** n'est pas dépassé : 10 min par défaut, 0 = toujours ;
- aucune **autre carte** n'a été posée entre-temps, sauf si la règle « reprendre même si
  une autre carte a été posée » est active : le dossier est alors rechargé et la lecture
  repart au morceau et à l'instant mémorisés.

Ces deux réglages se trouvent dans *Réglages → Reprise de la lecture*. Chaque carte peut
les remplacer (*Cartes → ✎*), par exemple « toujours reprendre » pour un livre audio.
Une commande MPD ou web qui modifie la file d'attente compte comme une autre carte.
Les positions survivent aux coupures de courant et aux mises à jour : idéal pour un long
podcast ou un livre audio. Elles sont tenues à jour chaque seconde en mémoire vive, et
enregistrées en mémoire permanente :

- pendant la lecture d'un même morceau, **la position seule** (clé de 32 octets), toutes
  les 10 s si elle a avancé : une coupure fait perdre au plus une dizaine de secondes ;
- le point complet (carte, dossier, morceau, ~600 octets) au changement de morceau, et tout
  de suite au retrait de la carte, en pause, au changement de carte ou en fin de playlist ;
- jamais plus d'une écriture toutes les 10 s pour une même carte, même si on la pose et la
  retire frénétiquement.

En lecture continue, cela représente environ 280 Ko écrits par jour dans une partition de
256 Ko à répartition d'usure : environ un effacement par secteur et par jour, pour une
endurance de 100 000 cycles, soit bien plus que la durée de vie de l'enceinte. Après un
redémarrage, le délai de reprise est calculé avec l'heure réelle (NTP).

Une mise à jour automatique attend que l'enceinte soit inactive : pas de carte posée, pas
d'envoi de fichiers (ni dans les 5 dernières minutes), et lecture arrêtée ou en pause
depuis plus de 2 heures.

## Mises à jour du firmware

### Publier une version (GitHub)

Les enceintes se mettent à jour depuis les **releases GitHub** de ce dépôt
(`https://github.com/blotg/enceinte-nfc`, source par défaut) :

```bash
# 1. incrémenter version.txt (ex. 1.2.0), committer
# 2. créer et pousser le tag correspondant :
git tag v1.2.0 && git push origin v1.2.0
```

La GitHub Action `firmware` lance les tests, compile avec ESP-IDF 5.5.5, vérifie que le tag
correspond à `version.txt` et publie la release avec :

- `enceinte.bin` : le firmware téléchargé par les enceintes ;
- `enceinte-complet.bin` : image complète (chargeur, partitions, firmware) à flasher à
  l'adresse 0 sur une carte neuve (`esptool.py --chip esp32s3 write_flash 0x0 enceinte-complet.bin`).

Un tag avec suffixe (`v1.2.0-rc1`) est publié en **pré-version** : les enceintes l'ignorent.
On peut l'installer à la main sur une enceinte de test (*Réglages → Installer un fichier .bin*).

L'enceinte vérifie 1 minute après le démarrage, puis toutes les 24 h (réglable). Elle lit la
version dans l'en-tête du binaire de la dernière release stable, sans le télécharger en
entier. Si elle est plus récente, elle attend la fin de la lecture, l'installe et redémarre.
Si le nouveau firmware plante avant 30 s de fonctionnement, l'enceinte revient seule à
l'ancienne version. Une tâche bloquée plus de 10 s fait aussi redémarrer l'enceinte (chien de
garde).

### Autres sources

Dans *Réglages → Mises à jour*, la source peut être un autre dépôt GitHub **public**, ou un
manifeste sur votre propre serveur :

```bash
tools/publish_ota.sh /srv/www/enceinte        # copie enceinte-X.Y.Z.bin + manifest.json
```

```json
{ "version": "1.0.1", "url": "enceinte-1.0.1.bin" }
```

**HTTPS est recommandé** pour un manifeste. HTTP reste accepté pour un serveur local, mais
n'importe qui sur le réseau pourrait alors substituer un firmware.

On peut aussi installer un `.bin` à la main depuis la même page. Le fichier est vérifié :
il doit s'agir d'un firmware de ce projet.

## Organisation du code (`main/`)

| Module | Rôle |
|---|---|
| `main.c` | Démarrage, bouton de réinitialisation, validation du firmware après 30 s |
| `pn532*.c`, `nfc.c` | Pilote PN532 (trames vérifiées, nombre d'essais borné, réinitialisation automatique), détection pose/retrait avec anti-rebond |
| `player.c` | File d'attente, lecture SD anticipée (réservoir de 2 à 16 s), décodage, I2S, volume |
| `controller.c`, `session.c` | Règles carte ↔ lecture (reprise, délai de 10 min) |
| `cards.c`, `settings.c` | Associations et réglages en NVS (résistants aux coupures de courant) |
| `storage.c`, `media_info.c` | Carte SD, tags ID3/Vorbis, durée, positions de recherche MP3 |
| `wifi_mgr.c`, `dns_server.c` | Wi-Fi, point d'accès de secours, portail captif, mDNS, NTP |
| `web_server.c`, `web/` | API et interface web (sessions, protection CSRF) |
| `mpd_server.c`, `mpd_proto.c` | Serveur MPD |
| `ota.c` | Mises à jour automatiques et manuelles |

## Tests (sur PC, sans le matériel)

```bash
test/host/run_tests.sh       # nécessite gcc, ffmpeg, lame ; python-mpd2 pour l'intégration MPD
```

1. Tests unitaires des modules purs, avec de vrais fichiers audio générés par ffmpeg/lame :
   ID3v2.3/2.4/v1, pochette intégrée, FLAC, WAV, fichiers corrompus. Couvre aussi les trames
   PN532, la règle de reprise, le DNS captif, les chemins (traversée de répertoire) et les
   filtres MPD.
2. Le contrôleur de cartes avec le **vrai** lecteur ; FreeRTOS, l'I2S et le décodeur sont
   simulés. Scénarios : reprise, autre carte, carte inconnue, délai dépassé, playlist
   terminée, mode association.
3. Le **vrai** serveur MPD et le **vrai** lecteur, pilotés par un vrai client MPD (python-mpd2).

Le tout est compilé avec AddressSanitizer et UndefinedBehaviorSanitizer.

L'interface web peut être développée sans l'enceinte : `python3 test/web/mock_api.py`, puis
http://127.0.0.1:8080 (`--setup` pour l'assistant ; mot de passe de connexion : `secret`).
