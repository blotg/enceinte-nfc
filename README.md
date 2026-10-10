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
| Carte NFC → lecture d'un dossier | ✅ | MP3, AAC, M4A, FLAC, WAV, OGG/Opus. Sous-dossiers inclus, ordre « naturel » (2 avant 10) ou aléatoire (réglage général ou carte par carte). |
| Lecteur NFC PN532 ou PN5180 | ✅ | Détecté automatiquement au démarrage. Le PN5180 porte plus loin et lit aussi les étiquettes ISO 15693 (ICODE SLIX). |
| Pause au retrait, reprise sous 10 min | ✅ | Durée de conservation de la progression réglable (0 = sans limite), progression conservée ou non après une autre carte, en réglage général ou carte par carte. Une carte inconnue compte comme « une autre carte ». |
| Mode sommeil par carte | ✅ | Pause après N morceaux ou N minutes d'écoute (fondu sonore), compté depuis la pose de la carte. |
| Répétition | ✅ | La playlist recommence au lieu de s'arrêter (berceuses, bruit blanc), en réglage général ou carte par carte. |
| Webradio | ✅ | Une carte lance une radio en direct (flux MP3, AAC ou Ogg, HTTP ou HTTPS, listes .m3u/.pls), avec le titre diffusé. Pas de flux HLS (.m3u8 en segments). |
| Podcasts | ✅ | Une carte suit un flux RSS : les nouveaux épisodes sont téléchargés chaque nuit sur la carte SD, tous gardés ou seulement les N plus récents, joués dans l'ordre avec la reprise habituelle. |
| Journal consultable | ✅ | *Réglages → Système → Journal* : derniers messages, raison du dernier redémarrage, résumé d'un plantage, mémoire libre ; à télécharger. |
| Association dossier ↔ carte par l'interface web | ✅ | Mode association : la carte posée est capturée sans lancer la musique. Dossiers nommés librement (accents, espaces). |
| Dépôt de fichiers par l'interface web | ✅ | Fichiers ou dossiers entiers, glisser-déposer. Débit limité par la carte SD en SPI. |
| Mises à jour automatiques | ✅ | Depuis les releases GitHub (ou votre serveur), au démarrage (+1 min) et toutes les N heures. Installation seulement quand l'enceinte est inactive. Retour automatique à l'ancienne version si la nouvelle ne démarre pas. |
| Protocole MPD | ✅ partiel | Large sous-ensemble (cf. plus bas). MPD ne connaît qu'un **mot de passe**, pas d'identifiant. |
| Point d'accès + portail captif | ✅ | Sortie d'usine ou Wi-Fi injoignable : réseau `Enceinte-XXXX`, la page de configuration s'ouvre seule. L'enceinte continue de chercher le Wi-Fi de la maison et y revient dès que possible (cf. plus bas). |
| Volume sur l'enceinte | ✅ | Deux touches tactiles cachées sous le bois (pièces de monnaie), ou deux boutons poussoirs. Maintien avant réaction contre les gestes involontaires des enfants. Volume entre 0 et le volume maximum, conservé après un redémarrage. |
| Normalisation et compression | ✅ | Trois intensités chacune, en réglage général et carte par carte. Niveau égalisé entre playlists et morceaux, écarts réduits dans un morceau. |
| Adresse IP fixe | ✅ | Adresse, masque, passerelle, DNS. Sans connexion administrateur à la nouvelle adresse sous 5 min, retour à l'ancienne configuration. |
| Réglages et cartes sur la carte SD | ✅ | Une carte SD clonée se comporte dans une autre enceinte exactement comme dans la première. Export et import des réglages en JSON. |
| Mot de passe administrateur en sortie d'usine | ✅ | Stocké haché (PBKDF2-SHA256). |
| Adresse `enceinte.local` | ✅ | mDNS ; nom modifiable. |
| Page d'administration | ✅ | Wi-Fi, adresse IP, mots de passe admin et MPD, fichiers (envoi, déplacement, renommage), cartes, volume maximum, son, HTTPS, mises à jour, sauvegarde, redémarrage. |
| *En plus* | | Bips au volume réglé (carte inconnue, dossier vide), réinitialisation usine (bouton BOOT 10 s), installation manuelle d'un `.bin`, volume maximum. |

**Limites assumées**

- La carte SD contient le **mot de passe du Wi-Fi en clair** (`/.enceinte.json`) : c'est ce qui
  permet à une copie de fonctionner telle quelle dans une autre enceinte. Les mots de passe
  administrateur et MPD n'y figurent que sous forme d'empreintes.
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
| *ou* PN5180 (SPI) | SCK 18, MOSI 17, MISO 8, NSS 10, BUSY 11, RST 13 | **5V** et **3V3** (cf. plus bas) + 100 µF |
| Carte SD (SPI) | CS 5, MOSI 16, MISO 15, SCK 12 | 3V3 ; **5V** si le module a un régulateur (AMS1117) |
| MAX98357A (I2S) | BCLK 3, LRC 1, DIN 9 | **5V** (puissance, ménage le régulateur 3,3 V) + 220 à 470 µF |
| Réinitialisation usine | bouton BOOT (GPIO0) | |
| Volume + et − | **GPIO7** (+) et **GPIO6** (−) : touches tactiles, ou boutons poussoirs vers **GND** | aucune |

Les GPIO de l'ESP32-S3 ne tolèrent pas le 5 V. Les entrées I2S du MAX98357A acceptent le
3,3 V même s'il est alimenté en 5 V.

### Lecteur NFC : PN532 ou PN5180

Au démarrage, l'enceinte cherche un **PN5180**, puis un **PN532** : un seul des deux suffit et
le même firmware convient aux deux. Le lecteur détecté s'affiche au survol de la puce « NFC »,
en haut de l'interface web.

Le PN5180 émet plus fort et tolère mieux une carte mal placée. Il lit les cartes ISO 14443A
(MIFARE, NTAG) comme le PN532 : les cartes déjà associées gardent le même numéro (UID). Il lit
aussi les étiquettes **ISO 15693** (NXP ICODE SLIX, SLIX2), dont la portée est nettement plus
grande, y compris à travers le bois.

| Module PN5180 | ESP32-S3 |
|---|---|
| 5V | 5V (alimente l'émetteur : c'est lui qui donne la portée) |
| 3V3 | 3V3 (logique, compatible avec l'ESP32) |
| GND | GND |
| SCK, MOSI, MISO | GPIO18, GPIO17, GPIO8 |
| NSS, BUSY, RST | GPIO10, GPIO11, GPIO13 |
| IRQ, GPIO, AUX, REQ | non reliées |

Fils courts (moins de 15 cm), condensateur de 100 µF entre 5V et GND près du module. Antenne à
plat contre le bois, à distance des pièces métalliques et de l'aimant du haut-parleur. Les
broches se changent dans menuconfig (SCK à -1 : pas de PN5180). Le pilote a été validé avec un
PN5180 simulé (tests sur PC), pas encore avec un vrai module : en cas de souci, les journaux
série (`idf.py monitor`, étiquettes `nfc` et `pn5180`) indiquent ce qui coince.

### Volume sur l'enceinte : touches tactiles sous le bois

Par défaut, GPIO7 (+) et GPIO6 (−) sont des **touches tactiles** (canaux tactiles 7 et 6 de
l'ESP32-S3) : rien ne dépasse du panneau.

- **Électrodes** : une pièce de 50 centimes (Ø 24,25 mm) ou un disque de cuivre ou de laiton
  de 20 à 25 mm, soit à peu près la taille d'un bout de doigt.
- **Logement** : par l'arrière du panneau, un lamage à fond plat qui laisse **1,5 à 2,5 mm** de
  bois devant la pièce. Une mèche Forstner de 25 mm avec butée de profondeur (perceuse à
  colonne) donne un fond plus régulier qu'un ciseau ; finir au ciseau si besoin. Plus le bois
  restant est fin, plus la touche est sensible ; sous 1,5 mm il risque de marquer ou de fendre.
- **Collage** : la pièce doit être plaquée contre le bois, sans lame d'air (colle époxy ou
  néoprène sur toute la surface).
- **Fil** : souder le fil sur la pièce (poncer la soudure, flux) ou le serrer sous un adhésif
  cuivre. Fil court (moins de 20 cm), éloigné des fils du haut-parleur et des mains : le fil
  lui-même est sensible. Une résistance de 470 Ω à 1 kΩ en série, près de l'ESP32, protège
  l'entrée des décharges électrostatiques.
- **Écartement** : au moins 4 cm entre les deux pièces. Un repère discret (point gravé,
  pyrogravure, petite incrustation) aide les adultes à les trouver.

*Réglages → Commandes de volume sur l'enceinte* : jauges en direct de chaque touche, seuil de
déclenchement (2 % par défaut) et temps de maintien (0,8 s par défaut). Doigt posé, la jauge
doit dépasser franchement le trait du seuil ; sans doigt, rester près de zéro. Contre les
gestes involontaires des enfants : un effleurement ne fait rien, il faut maintenir le doigt ;
les deux touches à la fois (main à plat) sont ignorées ; un appui de plus de 20 s (objet posé)
aussi. La mesure de repos suit lentement l'humidité du bois.

On peut aussi y choisir des **boutons poussoirs** (contact à la fermeture, entre la broche et
GND, sans résistance). Broches, cran (5 par défaut) et valeurs par défaut se règlent dans
menuconfig ; -1 désactive une commande. En mode tactile, seules les GPIO 1 à 14 conviennent ;
éviter les broches de démarrage (0, 3, 45, 46), de la PSRAM octale (33 à 37) et de l'USB (19, 20).

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
point d'accès se rouvre en secours, et l'enceinte continue de chercher le Wi-Fi de la maison
toutes les 30 s (une analyse ne coupe pas le point d'accès). Quand il réapparaît :

- si aucun appareil n'est connecté au point d'accès et qu'aucun envoi de fichier n'est en
  cours, l'enceinte s'y reconnecte aussitôt ;
- sinon, l'interface propose de basculer (« Basculer maintenant » ou « Plus tard ») : la
  connexion ferait changer le point d'accès de canal et couperait l'appareil. Sans réponse,
  la bascule se fait d'elle-même dès que le dernier appareil a quitté le point d'accès et
  que les envois sont terminés.

**Mot de passe oublié** : maintenir le bouton BOOT **10 secondes** (un bip à 3 s, trois bips à
10 s, au volume réglé). Réglages et associations sont effacés, dans l'enceinte comme sur la
carte SD (fichiers `.enceinte.json` et `.cartes.json`), la musique est conservée. Une carte SD
absente à ce moment rapporterait ses réglages à son retour.

## Utilisation

- **Cartes** (onglet *Cartes*) : « Associer une carte », poser la carte, choisir le dossier.
  Compatibilité avec l'ancien firmware : une carte non associée joue le dossier nommé d'après
  son identifiant (ex. `04ab53a96f2681`), s'il existe à la racine de la carte SD.
  Pour un sous-dossier : onglet *Musique*, ouvrir le dossier, « Associer une carte ».
  Une carte peut aussi jouer une **webradio** ou un **podcast** (« Que joue cette carte ? »).
  Le bouton ☰ d'une carte ouvre ses réglages : contenu, ordre de lecture, fin de playlist
  (s'arrêter ou recommencer), conservation de la progression, son, mode sommeil. L'onglet
  *Musique* montre aussi les cartes associées au dossier ouvert (et le nombre de cartes de
  chaque sous-dossier).
- **Bips** : deux bips = carte inconnue ; trois bips graves = dossier vide ou carte SD absente.
  Ils sont joués au volume réglé (volume 0 : aucun bip), à un niveau proche de celui de la musique.
- **Volume** : curseur de l'onglet *Lecture*, touches ou boutons de l'enceinte, ou application MPD. Il va
  de 0 au **volume maximum** (*Réglages → Enceinte*), affiché au bout du curseur, que rien ne
  peut dépasser ; baisser le maximum baisse aussi le volume s'il était au-dessus. Le volume
  est enregistré en flash une seconde après le dernier changement et retrouvé au démarrage.
  Hors lecture, chaque cran donné sur l'enceinte fait un bip court au nouveau volume.
- **Musique** (onglet *Musique*) : envoyer des fichiers ou des dossiers entiers, créer,
  renommer, supprimer, écouter un dossier sans carte, et **déplacer** des fichiers ou des
  dossiers (bouton ↦, ou cases à cocher pour en déplacer ou supprimer plusieurs à la fois).
  Les associations de cartes et les positions de reprise suivent les dossiers déplacés.
- **MPD** : adresse IP de l'enceinte, port 6600 (affichés dans *Réglages → Accès MPD*), dans
  M.A.L.P. (Android), mpc, Cantata, ncmpcpp, Rigelian (iOS)… Beaucoup d'applications, dont
  M.A.L.P., ne savent pas résoudre `enceinte.local` : mieux vaut réserver l'adresse IP de
  l'enceinte dans la box. Mot de passe facultatif dans *Réglages*.
  Pris en charge : lecture, pause, morceau suivant ou précédent, recherche de position,
  volume, aléatoire, répétition, single, consume. Aussi la file d'attente (`add`, `delete`,
  `move`, `swap`, `playlistinfo`, `plchanges`…), le parcours de la carte (`lsinfo`,
  `listall[info]`, `listfiles`), `list`/`find`/`search`/`count` (filtres classiques et
  expressions), `idle`/`noidle`, les listes de commandes et `albumart` (`cover.jpg`/`.png`
  dans le dossier).

### Son : normalisation et compression

*Réglages → Son*, et carte par carte (*Cartes → ☰*, « Réglage général » par défaut). Valeur
d'usine : **moyenne** pour les deux (menuconfig : `ENC_DEFAULT_NORMALIZE`, `ENC_DEFAULT_COMPRESS`).
Le traitement se fait avant le volume ; un limiteur empêche toute saturation.

| Réglage | Effet | Légère | Moyenne | Forte |
|---|---|---|---|---|
| **Normalisation** | Égalise le niveau d'une playlist et d'un morceau à l'autre. Le niveau est mesuré en continu (silences et passages calmes ignorés) : un morceau plus fort est baissé en moins d'une seconde, un plus calme remonté en quelques secondes. Chaque nouvelle playlist repart d'un gain neutre : jamais plus fort qu'à l'origine. | écart corrigé de moitié, +6 dB au plus | 75 %, +9 dB | en totalité, +12 dB |
| **Compression** | Réduit les écarts de volume dans un morceau : passages calmes remontés, passages forts atténués, sans remonter le souffle des silences. Pour les histoires et livres audio. | 2:1 | 3:1 | 5:1 |

Sur un signal alternant passages forts et calmes (30 dB d'écart), il reste environ 22, 15 et
7 dB d'écart selon l'intensité de la compression. Pour une lecture lancée depuis l'interface
web ou une application MPD, ce sont les réglages généraux qui s'appliquent.

### Mode sommeil

Carte par carte (*Cartes → ☰ → Mode sommeil*) : la lecture se met en pause après un nombre
de morceaux (1 à 999) ou une durée d'écoute (1 à 720 minutes).

- Le décompte part de la pose de la carte. Les pauses (carte retirée, bouton pause) ne
  comptent pas.
- **En morceaux** : le morceau en cours à la pose compte pour un ; la pause tombe juste avant
  le morceau suivant.
- **En durée** : le son baisse doucement pendant les 15 dernières secondes, puis la lecture
  se met en pause.
- Carte laissée sur l'enceinte : rien ne repart. Retirer et reposer la carte reprend là où
  la lecture s'était arrêtée (selon les règles de reprise de la carte), avec un nouveau
  décompte.
- L'onglet *Lecture* affiche ce qui reste avant la pause. Modifier le réglage pendant
  l'écoute relance le décompte. Une lecture lancée depuis l'interface web ou une application
  MPD n'est pas concernée.

### Webradio

Une carte associée à une webradio la joue en direct : *Cartes → Associer une carte*, « Une
webradio », un nom et l'adresse du flux (ou *Musique → + Webradio*, puis « Associer une
carte » dans le dossier créé).

- L'adresse est celle d'un flux MP3, AAC ou Ogg, souvent donnée sur le site de la radio
  (elle finit souvent par `.mp3`, `.aac`, `.m3u` ou `.pls`). Les listes `.m3u`/`.pls` et les
  redirections sont suivies ; HTTP et HTTPS. Les flux HLS (`.m3u8` découpés en segments),
  qu'utilisent certaines applications, ne sont pas pris en charge : prendre l'adresse
  « Icecast » ou « Shoutcast » de la radio.
- Elle est rangée dans `Webradios/<nom>/webradio.m3u` sur la carte SD. N'importe quel
  dossier contenant une liste `.m3u` ou `.pls` d'adresses `http(s)://` se joue d'ailleurs de
  la même façon.
- L'onglet *Lecture* et les applications MPD affichent le titre diffusé par la radio. Une
  application MPD peut aussi ajouter directement une adresse de flux (`add http://…`).
- Retirer la carte coupe la connexion ; la reposer reprend **en direct** (pas de retard
  accumulé). Coupure du réseau : reconnexion automatique (3 essais), sinon message
  « radio injoignable ». La position n'est pas enregistrée (rien à reprendre) et le mode
  sommeil en minutes s'applique comme pour un dossier.

### Podcasts

*Cartes → Associer une carte*, « Un podcast » : l'adresse du flux RSS (« flux RSS » ou
« RSS feed » sur le site du podcast), un nom facultatif (sans nom : le titre du podcast),
et le nombre d'épisodes à garder (10 par défaut, **0 pour tous**). Ou *Musique → + Podcast*.

- L'enceinte crée `Podcasts/<nom>` et y télécharge les épisodes, du plus récent au plus ancien, nommés
  `AAAA-MM-JJ HHhMM - Titre.mp3` : la carte les joue **du plus ancien au plus récent**, avec la
  reprise habituelle (idéal pour une série d'histoires).
- Les nouveaux épisodes arrivent **chaque nuit** entre 2 h et 5 h, enceinte inactive ; si
  elle est éteinte la nuit, au plus tard 48 h après la dernière vérification. Le dossier du
  podcast (onglet *Musique*) montre l'état et permet de vérifier tout de suite.
- Avec N épisodes gardés, seuls les N plus récents restent sur la carte SD (sauf l'épisode
  en cours d'écoute) ; si N augmente, les anciens reviennent. Avec 0, tout est gardé (les
  2000 plus récents du flux au plus). Un épisode supprimé à la main n'est jamais
  retéléchargé. 300 Mo restent toujours libres.
- Un téléchargement automatique s'interrompt dès que l'écoute commence ; il reprend plus tard.
- L'abonnement est le fichier `.podcast.json` du dossier, la liste des épisodes connus
  `.podcast-episodes.txt` : ils suivent le dossier quand on le déplace et voyagent avec une
  carte SD clonée. « Se désabonner » garde les épisodes.
- Une mise à jour du firmware attend la fin des téléchargements.

### Journal

*Réglages → Système → Journal de l'enceinte* : les derniers messages de l'enceinte (48 Ko,
les mêmes que sur le port série), avec l'heure, la raison du dernier redémarrage (coupure,
chute de tension, plantage, chien de garde…) et, après un plantage, la tâche en cause et la
pile d'appels. La mémoire libre y est affichée (mémoire interne, plus grand bloc, minimum
depuis le démarrage, PSRAM). « Télécharger » en fait un fichier texte à joindre à un
signalement.

### Adresse IP fixe

*Réglages → Adresse IP* : automatique (DHCP) ou fixe (adresse, masque, passerelle, DNS
facultatif, sinon la passerelle). Les champs sont pré-remplis avec la configuration du moment.
Une nouvelle configuration est d'abord **essayée sans être enregistrée** : il faut se connecter
à l'interface **à la nouvelle adresse dans les 5 minutes** (le mot de passe est sans doute
redemandé). Sans connexion administrateur à cette adresse dans ce délai, ou après une coupure
de courant, l'enceinte revient d'elle-même à l'ancienne configuration. Un avertissement le
rappelle avant l'application, puis un compte à rebours s'affiche.

### Sauvegarde, carte SD clonée

Réglages et associations sont recopiés sur la carte SD à chaque modification :

- `/.enceinte.json` (racine) : réglages généraux, y compris le Wi-Fi et l'adresse IP, et les
  mots de passe administrateur et MPD sous forme d'empreintes (PBKDF2, jamais en clair) ;
- `<dossier>/.cartes.json` : les cartes associées à ce dossier et leurs réglages propres. Le
  fichier suit le dossier quand on le déplace, depuis l'interface ou sur un ordinateur.

Au démarrage, et quand une carte SD est remise en place, l'enceinte charge ces fichiers : la
carte SD **fait foi**. Une copie de la carte SD placée dans une autre enceinte (même sortie
d'usine) s'y comporte exactement comme dans la première : même Wi-Fi, mêmes mots de passe,
mêmes cartes. Sans `/.enceinte.json` (carte neuve, ou mise à jour depuis la version 1.3), les
réglages et associations de l'enceinte y sont recopiés. Une modification faite pendant
l'absence de la carte SD lui est recopiée à son retour. Un fichier abîmé est ignoré et réécrit.
Supprimer un dossier depuis l'interface supprime aussi ses associations.

Le volume courant et les positions de reprise restent dans la flash interne (écrits bien trop
souvent pour une carte SD). Si deux enceintes clonées fonctionnent en même temps, changez le
nom et l'éventuelle adresse IP fixe de l'une d'elles.

*Réglages → Sauvegarde des réglages* : **exporter** un fichier JSON (réglages et associations,
mots de passe compris ou non) et l'**importer** dans la même enceinte ou une autre. L'import
remplace les associations ; celles dont le dossier n'existe pas sur la carte SD sont ignorées.
Une adresse IP fixe importée est essayée comme ci-dessus.

### Reprise d'une carte

Retirer une carte met en pause et mémorise sa position (morceau et instant, pour les
16 dernières cartes). Reposée, la carte reprend si sa playlist n'était pas finie et si :

- la progression est encore **conservée** : 10 min par défaut après le retrait, 0 = sans
  limite ;
- aucune **autre carte** n'a été posée entre-temps, sauf si la règle « conserver la
  progression même si une autre carte est posée » est active : le dossier est alors
  rechargé et la lecture repart au morceau et à l'instant mémorisés.

Ces réglages, et l'ordre de lecture (dans l'ordre ou aléatoire), se trouvent dans
*Réglages → Lecture des cartes*. Chaque carte peut les remplacer (*Cartes → ☰*), par
exemple « sans limite » pour un livre audio, « aléatoire » pour des comptines.

En **ordre aléatoire**, l'ordre est tiré quand la playlist commence (un nouvel ordre à
chaque fois qu'elle recommence) et mémorisé avec la position : une reprise retrouve la même
suite, sans rejouer ni sauter de morceau, même après une coupure de courant.
Une commande MPD ou web qui modifie la file d'attente compte comme une autre carte.
Les positions survivent aux coupures de courant et aux mises à jour : idéal pour un long
podcast ou un livre audio. Elles sont tenues à jour chaque seconde en mémoire vive, et
enregistrées en mémoire permanente :

- pendant la lecture d'un même morceau, **la position seule** (une entrée de 32 octets),
  toutes les 2 s si elle a avancé : une coupure fait perdre au plus 2 secondes ;
- le point complet (carte, dossier, morceau, ~600 octets) au changement de morceau, et tout
  de suite au retrait de la carte, au changement de carte ou en fin de playlist ;
- jamais plus d'un point complet toutes les 10 s pour une même carte, même si on la pose et
  la retire frénétiquement.

Ces données sont dans la flash interne de l'ESP32 (partition « cfg » de 256 Ko), pas sur la
carte SD. Le stockage NVS d'ESP-IDF écrit en journal : chaque nouvelle valeur va dans la case
libre suivante, l'ancienne est seulement marquée périmée, et une page de 4 Ko n'est effacée
qu'une fois tout le ruban parcouru. En lecture continue 24 h/24, cela fait environ
5 effacements par page et par jour, pour une endurance de 100 000 cycles : une cinquantaine
d'années, et des siècles à raison de quelques heures d'écoute par jour. Après un
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

L'enceinte vérifie 1 minute après le démarrage, puis toutes les 24 h (réglable).
Source d'usine : `https://github.com/blotg/enceinte-nfc` (bouton « Valeur d'usine » dans
*Réglages → Mises à jour*). Elle lit la
version dans l'en-tête du binaire de la dernière release stable, sans le télécharger en
entier. Si elle est plus récente, elle attend la fin de la lecture, l'installe et redémarre.
Si la vérification ou le téléchargement échoue (réseau, GitHub, mémoire), elle réessaie
15 min plus tard, puis 30 min, 1 h… sans dépasser l'intervalle réglé.
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

Après une mise à jour, une page de l'interface restée ouverte se recharge d'elle-même pour
afficher la nouvelle interface : chaque firmware a un identifiant (empreinte de son binaire),
inscrit dans la page et dans les adresses de `app.js` et `style.css`. Le navigateur revalide
ces fichiers à chaque chargement (réponse 304 sans contenu tant que le firmware est le même).

## Organisation du code (`main/`)

| Module | Rôle |
|---|---|
| `main.c` | Démarrage, bouton de réinitialisation, validation du firmware après 30 s |
| `pn532*.c`, `pn5180*.c`, `nfc.c` | Pilotes PN532 (trames vérifiées, nombre d'essais borné) et PN5180 (ISO 14443A et 15693), détection automatique du lecteur, détection pose/retrait avec anti-rebond, réinitialisation automatique |
| `player.c` | File d'attente, lecture SD ou réseau anticipée (réservoir de 2 à 16 s), décodage, I2S, volume |
| `radio.c`, `stream.c`, `net_http.c` | Webradio : listes .m3u/.pls, titres ICY, flux HTTP(S) avec redirections |
| `rss.c`, `podcast.c` | Podcasts : flux RSS lu au fil de l'eau, abonnements, téléchargements de la nuit |
| `log_ring.c`, `log_buffer.c` | Journal en mémoire pour l'interface web, raison du redémarrage, résumé d'un plantage |
| `dsp.c` | Normalisation, compression, limiteur |
| `buttons.c`, `touch_keys.c` | Volume sur l'enceinte : touches tactiles ou boutons poussoirs |
| `controller.c`, `session.c` | Règles carte ↔ lecture (reprise, délai de 10 min) |
| `cards.c`, `settings.c` | Associations et réglages en NVS (résistants aux coupures de courant) |
| `backup.c`, `config_json.c` | Copie des réglages et associations sur la carte SD, export et import JSON |
| `storage.c`, `media_info.c` | Carte SD, tags ID3/Vorbis, durée, positions de recherche MP3 |
| `wifi_mgr.c`, `dns_server.c` | Wi-Fi, adresse IP fixe à l'essai, point d'accès de secours, portail captif, mDNS, NTP |
| `web_server.c`, `web/` | API et interface web (sessions, protection CSRF) |
| `mpd_server.c`, `mpd_proto.c` | Serveur MPD |
| `ota.c` | Mises à jour automatiques et manuelles |

## Tests (sur PC, sans le matériel)

```bash
test/host/run_tests.sh       # nécessite gcc, ffmpeg, lame, flac ; python-mpd2 (MPD), libcjson-dev ou IDF_PATH (JSON)
```

1. Tests unitaires des modules purs, avec de vrais fichiers audio générés par ffmpeg/lame :
   ID3v2.3/2.4/v1, pochette intégrée, FLAC, WAV, fichiers corrompus. Couvre aussi les trames
   PN532, le PN5180 simulé (anticollision, UID de 4, 7 et 10 octets, ISO 15693), la règle de reprise, le DNS captif, les chemins (traversée de répertoire), les
   filtres MPD, la normalisation et la compression (signaux de synthèse), les touches
   tactiles (effleurement, maintien, main à plat, dérive due à l'humidité, objet posé), les
   adresses IP et les réglages en JSON (valeurs invalides refusées).
2. Le contrôleur de cartes avec le **vrai** lecteur ; FreeRTOS, l'I2S et le décodeur sont
   simulés. Scénarios : reprise, autre carte, carte inconnue, délai dépassé, playlist
   terminée, mode association, ordre aléatoire retrouvé à la reprise, mode sommeil (en
   morceaux, en durée avec fondu, nouveau décompte quand la carte est reposée).
   Aussi la répétition et la webradio (flux simulé : direct, titre, coupure du réseau et
   reconnexion, reprise en direct, radio injoignable).
3. Le **vrai** serveur MPD et le **vrai** lecteur, pilotés par un vrai client MPD (python-mpd2).
4. La copie sur carte SD, avec deux enceintes simulées : première copie, carte clonée dans une
   enceinte sortie d'usine, dossiers déplacés et fichiers ajoutés sur un ordinateur, fichier
   abîmé, modifications pendant l'absence de la carte, export et import, réinitialisation.
5. Les podcasts avec un « Internet » simulé : abonnement, renommage d'après le titre du flux,
   épisodes gardés (N ou tous, N augmenté puis réduit), épisode en cours d'écoute épargné,
   épisode supprimé à la main, épisode ou flux injoignable, désabonnement, reprise d'un
   abonnement de la version 1.8. Le flux RSS lui-même (CDATA, entités, dates) est testé
   découpé de toutes les façons possibles.

Le tout est compilé avec AddressSanitizer et UndefinedBehaviorSanitizer.

L'interface web peut être développée sans l'enceinte : `python3 test/web/mock_api.py`, puis
http://127.0.0.1:8080 (`--setup` pour l'assistant, `--ap` pour le Wi-Fi de secours ; mot de
passe de connexion : `secret`).
