# Vocal a deux (0.4)

Les reglages audio et les limites de test ci-dessous restent valables. Depuis
la version 0.4, le parcours ICE transporte le texte fiable et l'audio dans la
meme association DTLS ; seul le texte passe par KCP. Il utilise un seul chemin
ICE choisi automatiquement, direct ou relaye. Le port UDP +1 et les details
TLS/TCP de cette page s'appliquent uniquement au mode IP manuelle. Voir connectivity.md.

## Utilisation

Connecter les deux applications via Connexion privee, puis cliquer chacun sur
Rejoindre le vocal. Tant qu'une seule personne le demande, aucun peripherique
audio n'est ouvert. Apres authentification DTLS, le micro et la sortie par defaut
sont ouverts et le statut passe a Micro actif. Muet suspend l'encodage et l'envoi
du micro ; l'ecoute reste active et le peripherique reste ouvert. Retablir reactive
l'envoi. Quitter le vocal, la sortie du proche ou la deconnexion libere les
peripheriques via le worker, sans attendre une action de l'interface.

Les autorisations micro de l'OS restent necessaires. Le prototype n'a pas
d'annulation d'echo : utiliser un casque pour les premiers essais. Le choix des peripheriques et les volumes existent depuis 0.6.1 (voir la fin de cette
page) ; il n'y a toujours pas de suppression de bruit ni de controle automatique du gain.
Le partage d'ecran video est decrit dans interface.md.

## Transport

Le texte utilise TCP/TLS. Les demandes vocales (arret=0, actif=1, actif/muet=3)
passent dans des controles authentifies sur ce canal. Une demande distante ne
peut pas activer seule le micro local. La reception de son est egalement subordonnee
a l'activation locale et distante.

Le vocal ouvre un canal UDP distinct, sur le port TCP de l'invitation + 1. Avec
le port par defaut, il faut TCP 48152 et UDP 48153 accessibles chez l'hote.
Pas de STUN, TURN ou relais automatique pour l'instant. Pas de vocal sur une
invitation TCP 65535. Les adresses IPv6 ne sont pas encore supportees.

DTLS 1.2 est gere par Mbed TLS avec ECDHE-PSK et ChaCha20-Poly1305. La PSK media
est derivee du secret d'invitation via HKDF-SHA256 et le domaine
murmur/dtls/voice/v1, distinct du canal texte. Les cookies DTLS et les verifications
anti-rejeu de Mbed TLS restent actifs. MTU de transport 1200 octets.
L'authentification UDP doit aboutir sous environ 15 secondes. Un echec du vocal
ferme ses ressources et laisse le texte utilisable ; il faut redemander le vocal
pour reessayer. Les erreurs micro et reseau ne sont pas encore distinguees dans l'UI.

## Audio et ressources

Opus 1.6.1, mono, 48 kHz, 960 echantillons par trame (20 ms), cible codec
24 kbit/s, complexite 5 et DTX. Ces chiffres sont des reglages, pas une mesure
du debit IP ou de la latence totale. Les extensions ML d'Opus sont desactivees.
Le callback miniaudio transfere uniquement le PCM dans des ring buffers SPSC ;
notre callback n'alloue pas de memoire, ne prend pas de verrou et n'encode pas.
Encodage, decodage et DTLS sont traites dans le worker reseau.

Chaque file PCM contient au plus 3840 echantillons mono (80 ms). Le consommateur
capture saute les vieux echantillons quand il prend du retard. La lecture commence
apres 40 ms de prechargement et revient au silence si les donnees manquent.
Le producteur lecture abandonne les nouveaux echantillons si la file est pleine.
Il n'y a pas d'accumulation illimitee de PCM ; ces limites ne bornent pas la
latence des pilotes et du reseau.

Les paquets portent un numero de sequence de 32 bits et au plus 400 octets Opus.
Seules les trames de 20 ms sont acceptees. Une file de reordonnancement de huit
paquets maximum (environ 3,4 Ko de stockage) attend 40 ms avant le premier decodage,
puis produit une trame toutes les 20 ms via une horloge monotone du worker.
Le callback de lecture conserve son prechargement PCM ; la fenetre reseau s'ajoute
donc a la latence locale, ce n'est pas une promesse de latence totale de 40 ms.
Ce delai est fixe : il n'y a pas encore d'adaptation automatique a la gigue.

Les doublons sont ignores ; pendant la lecture, les paquets arrives apres leur
echeance sont ecartes. Opus masque au plus trois trames consecutives manquantes,
puis le moteur se remet en attente. Apres un silence, il se recale sur le prochain
paquet reel, sans confondre les trames PLC locales avec des numeros de sequence
emis par le proche. Une grande discontinuite de sequence vide la file et recale
la lecture. Apres un retard d'ordonnancement de plus de 100 ms par rapport a
l'echeance, seul le paquet en attente le plus recent est conserve.
Chaque passage du worker decode au plus deux trames. Le passage de sequence
0xffffffff a 0 est gere sans redemarrer la session.
Il manque le controle de congestion et les mesures de qualite sur reseaux reels.
Un paquet en attente d'ecriture DTLS peut faire abandonner un nouveau paquet
audio ; le texte ne partage pas sa file avec l'audio.

## Verification

Six suites de tests Windows passent : format borne, TLS/DTLS avec mauvaise cle,
worker texte, codec sans peripherique, worker vocal entre processus et simulation
de reseau a horloge virtuelle. Le codec
est teste avec un signal synthetique, une trame perdue, les doublons, l'ordre,
le mute et le debordement capture. Le test vocal entre processus couvre
l'activation des deux cotes, le trajet Opus/DTLS dans les deux sens, le mute et
la sortie ; il attend un decodage effectif, pas seulement la reception d'un paquet.
La variante du worker utilisee par ce test n'ouvre aucun peripherique.
La simulation couvre des retards variables de 0 a 35 ms avec inversion de paquets,
deux pertes consecutives, un paquet tardif, un doublon, le debordement du compteur,
la reprise apres silence, une rafale et un arret temporaire du worker. Elle verifie
les comptes de trames decodees/compensees et les limites de file, sans delai reel
ni acces aux peripheriques.
Ports de test supplementaires : UDP 49185, TCP 49186 et UDP 49187.

La capture WASAPI reelle, une conversation entre machines, l'echo, les changements
de peripherique, la consommation CPU/RAM en appel et la latence restent a valider.
Linux et macOS ne sont pas encore compiles ni testes et n'ont pas d'interface
vocale. Les tests ne constituent pas un audit de securite.


## Réglages audio Windows

Depuis 0.6.1, **Paramètres** permet de choisir le micro et la sortie audio,
avec deux volumes indépendants de 0 à 100 %. Les volumes s’appliquent aussi
pendant un appel ; changer de périphérique demande de raccrocher.
Les identifiants de périphériques et les volumes sont conservés dans
`%LOCALAPPDATA%/Murmur/audio-v1.dat` à la fermeture du panneau.
Un périphérique enregistré mais absent doit être remplacé dans les paramètres.

**Tester le micro** ouvre uniquement l’entrée et affiche son niveau, sans
l’enregistrer ni le diffuser. **Écouter un son** ouvre uniquement la sortie
sélectionnée et joue un signal local. Chaque test s’arrête après cinq secondes,
à la fermeture du panneau ou lors d’un appel entrant. Le test solo conserve un
audio entièrement simulé ; ses tests de périphériques sont désactivés.

Les appels ont un état entrant explicite et demandent une réponse locale avant
d’ouvrir les périphériques. L’état muet du correspondant est affiché et un
raccrochage termine la participation des deux personnes.

## Qui parle (0.8)

`murmur_voice_levels` donne, de 0 à 100, la force de ce que le micro envoie et de ce que l'autre côté envoie
(échelle racine carrée du pic, qui suit mieux l'oreille : la parole normale tombe entre 30 et 70, une pièce calme
sous 10). Un côté vaut 0 s'il est muet ou si aucun son n'est arrivé depuis 150 ms. Le codec traite un son pur et
stable comme du bruit de confort (DTX) : les tests utilisent donc du bruit fort, que Opus prend pour de la parole.

## Seuil du micro et traitement du son (0.8)

La configuration audio contient maintenant `echo`, `noise`, `agc` et `gate`. Le seuil (`gate`, 0 à 100, même échelle que
les niveaux) coupe l'envoi quand le micro reste en dessous. Pour le vocal à deux natif, l'annulation d'écho, la réduction
de bruit et le gain automatique reposent sur SpeexDSP, **désactivé par défaut** (`-DMURMUR_ENABLE_SPEEXDSP=ON`, qui télécharge
la bibliothèque) ; ce code n'a pas encore été compilé ni testé. Les salons vocaux de la page utilisent, eux, le traitement du navigateur.
