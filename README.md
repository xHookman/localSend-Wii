# LocalSend pour Wii

Homebrew Wii qui reçoit des fichiers envoyés avec l'application **LocalSend**
(protocole v2, **HTTP et HTTPS**) et les enregistre sur la carte SD.

## Compilation (macOS, devkitPro)

```sh
sudo dkp-pacman -S wii-dev     # une seule fois
make clean && make             # produit localsend-wii.dol
```

Première installation : copier `localsend-wii.dol` vers `SD:/apps/localsend/boot.dol`,
ainsi que `sd/apps/localsend/meta.xml` et `icon.png`.

## Mettre à jour sans retirer la carte SD

LocalSend pour Wii doit être lancé sur la Wii, puis sur le Mac :

```sh
make envoyer WII=192.168.1.42        # compile et envoie le .dol
# ou : ./envoyer.sh 192.168.1.42 [fichier]
```

La Wii propose **« Installer la vX et redémarrer »** (A). L'ancienne version est
gardée dans `boot.dol.bak`. Cela marche aussi en envoyant le `.dol` avec l'app LocalSend.

## Utilisation

- **HTTPS actif par défaut** : rien à changer dans LocalSend sur le téléphone.
  La Wii crée sa clé et son certificat au premier lancement
  (`sd:/localsend/tls_key.bin`, `tls_cert.der`). Le HTTP en clair reste accepté.
- Après réception d'un type connu, un panneau propose (croix + A, B = laisser) :
  - `.wad` : **Installer avec YAWM ModMii Edition** (rangé dans `sd:/wad`,
    puis yawmME est lancé ; il ouvre automatiquement le dossier `wad`),
  - ROMs : ranger dans le dossier de l'émulateur, ou en choisir un autre
    (SD ou clé USB, copie vérifiée par CRC32), dossier mémorisé par extension,
  - ouvrir WiiXplorer.
- **1** : réglages (port, rangement automatique, HTTPS, clé USB, WiiXplorer).
- Configuration : `sd:/localsend/config.ini`.

## Tiers

- [BearSSL](https://bearssl.org) (MIT) dans `source/bearssl/`.
- `source/app_booter.h` : chargeur de USB Loader GX (GPLv3), pour lancer
  WiiXplorer, YAWM ModMii Edition et la nouvelle version après mise à jour.
- Police DejaVu Sans (licence libre) rasterisée dans `source/font_data.h`.
