# Direct 0.6.0 — Messages et appels

La version 0.6 ajoute le panneau **Versions**, l'historique local et GitHub,
la verification et le telechargement en arriere-plan de mises a jour signees,
puis leur installation a la fermeture. Le depot cible est `Jxlio/Direct`.
Le workflow et les assets sont prepares ; aucune release n'est encore publiee.
Voir [UPDATES.md](UPDATES.md) pour la publication et les limites de cette premiere
generation de mise a jour de l'executable Windows x64.

Direct est le nouveau nom affiche de Murmur. L'application Windows se lance avec
`Direct.exe`. Les formats reseau, la cible CMake `murmur` et le stockage existant
`%LOCALAPPDATA%/Murmur` sont conserves pour garder les contacts et la compatibilite.
Le nom est un choix d'interface, sans affirmation de disponibilite commerciale.

Les pictogrammes d'interface sont les vrais SVG du pack Lucide, convertis en
ressources ICO multi-resolution et charges en cache. Ils remplacent les dessins
manuels et les glyphes systeme. Les SVG, la revision et les licences sont conserves
dans `src/windows/assets/lucide/`. Les ressources compilees sont deja fournies :
aucune dependance supplementaire pour construire ou utiliser l'application.
Pour les regenerer : `fetch_lucide.py`, puis `build_lucide.cjs` (resvg-js 2.6.2
dans out/icon-tools), puis `build_lucide_ico.py` (Pillow), tous dans assets/.

L'interface claire utilise des accents bleus, des icones vectorielles et des
panneaux translucides sur un fond dessine, avec contours fins. Ces panneaux
n'appliquent pas de flou au bureau derriere la fenetre. Le backdrop et les coins
natifs sont demandes a Windows ; les anciennes versions peuvent les ignorer.
Pas de navigateur, d'animation permanente ni de capture d'ecran pour cet effet.
Le rendu principal, le panneau d'amis et la connexion Internet ont ete inspectes
sous Windows. Les autres DPI et un echange reel entre logements restent a valider.

Les actions principales s'appellent **Ajouter ou connecter un ami**, **Demarrer
l'appel**, **Connexion Internet** et **Appliquer la reponse**. Les discussions
s'appellent **Messages**, **Organisation** et **Jeux**.

Projet de communication privee entre amis : texte, vocal et partage d'ecran,
avec un coeur en C17 pour Windows, Linux et macOS.

## Etat reel

La version 0.5 ajoute « Mes amis » sous Windows : un code prive par relation,
16 contacts persistants proteges avec DPAPI et une recherche automatique sur le
reseau local. Chaque ami clique « Retrouver sur ce reseau » ; les invitations ICE
de session sont alors echangees automatiquement sous chiffrement authentifie.
Aucun serveur a lancer pour ce mode local. Le service de mise en relation
Internet reste a implementer et a heberger : ce parcours ne fonctionne pas encore
automatiquement entre deux logements. Voir [FRIENDS.md](FRIENDS.md).

Premiere messagerie directe entre deux pairs : interface Windows native en
C/Win32, invitations secretes de session, transport chiffre via Mbed TLS 3.6.7 et
trois salons texte. Historique uniquement en memoire (128 messages maximum,
2048 unites UTF-16 par message dans l'interface). Le premier vocal a deux est
integre : Opus mono, canal temps reel distinct du texte fiable, activation des deux
cotes, bouton muet et liberation des peripheriques en quittant.
La version 0.3 ajoute un reordonnancement borne a delai fixe, la compensation
des pertes courtes et la reprise apres silence, testes sur reseau simule.
La version 0.4 ajoute ICE/STUN/TURN via libjuice, un echange invitation/reponse
a copier et un transport DTLS sur le chemin choisi. KCP rend le texte fiable ;
l'audio le contourne. L'interface propose STUN Cloudflare, modifiable, et un relais
TURN facultatif. Aucun relais Internet n'est deploye. Un executable murmur-relay
permet de preparer un hebergement futur. Le partage d'ecran reste a faire.
L'ancien mode IP manuelle, TCP/TLS + UDP/DTLS, reste accessible comme solution de test.
Les tests d'echange chiffre et de refus d'une mauvaise invitation passent sur
loopback sous Windows. Le parcours complet entre deux interfaces et deux machines
reste a valider. Le codec et le trajet vocal chiffre entre processus sont testes
avec du son synthetique, sans ouvrir le micro. Une conversation avec de vrais
peripheriques reste a valider. Ce prototype n'a pas ete audite.
Voir [ICE.md](ICE.md) pour le nouveau parcours, [RELAY.md](RELAY.md) pour le relais,
[NETWORK.md](NETWORK.md) pour le mode manuel et [VOICE.md](VOICE.md) pour l'audio.
Les seize tests Windows passent, y compris connexion automatique entre processus,
contacts proteges et signalisation
chiffree, ICE direct et TURN local, textes de
8192 octets, Opus chiffre, invitation alteree, worker entre processus et annulation.
Les reseaux NAT Internet et le parcours complet entre deux interfaces restent
a verifier ; les tests n'utilisent aucun serveur public ni peripherique audio.
Linux et macOS disposent seulement de la demonstration console du coeur C ;
leurs interfaces restent a implementer et leurs compilations a verifier.

## Compiler

Installer CMake >= 3.21, Git, Python 3 (generation Mbed TLS) et un compilateur C17
(MSVC, Clang ou GCC). CMake recupere les versions epinglees de Mbed TLS, Opus,
miniaudio, libjuice et KCP si elles ne sont pas presentes dans vendor/. Les executions suivantes
sont locales. Garder le dossier licenses/ avec l'executable lors du partage.

```sh
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Executable Windows avec Visual Studio : `build/Debug/Direct.exe`.
Sur les generateurs a configuration unique : `build/Direct`.
Pour une version optimisee : remplacer Debug par Release dans les commandes.
Sous Windows, lancer `build/Release/Direct.exe` ouvre la fenetre native.
L'interface utilise les polices du systeme, suit le changement de DPI et ne
redessine que sur evenement ; elle ne possede ni minuterie de rendu ni navigateur.
La presentation Windows utilise un theme clair avec accents bleus, une navigation
laterale unique, des controles arrondis dessines en GDI et une barre de titre sombre lorsque le systeme
la prend en charge. Aucun asset distant ou moteur web n'est charge.
Les performances reelles de l'interface restent a mesurer.

L'ecran principal et le panneau d'amis partagent la meme palette. Les etats
inactifs, la selection des salons et le focus clavier restent visibles. Les
boutons principaux reagissent au survol ; le dessin principal est mis en tampon
pour limiter le scintillement lors du redimensionnement, sans boucle de rendu.
Le rendu principal a ete inspecte dans l'application Windows. La verification
visuelle complete du panneau d'amis, des messages recus et des changements de
moniteur/DPI reste a terminer.

## Direction technique

- Transport P2P avec ICE, STUN et relais TURN de secours implemente. L'echange
  des descriptions est automatise sur le reseau local ; il reste manuel entre
  logements. Aucun service de rendez-vous Internet deploye.
- Transport chiffre et authentifie standard, bibliotheque maintenue, identites
  verifiees par invitation. Choix de bibliotheque et protocole de groupe a valider
  pour la suite du projet. La premiere implementation utilise TLS avec une
  invitation PSK par session ; pas de cryptographie maison. Il n'y a pas encore
  d'identite permanente ni d'authentification individuelle au sein d'un groupe.
- Mode ICE : texte et controles fiables sur KCP, puis DTLS, puis ICE/UDP ; audio
  direct dans DTLS sans KCP, avec sequence et Opus 20 ms. Mode IP manuelle :
  texte TCP/TLS et audio UDP/DTLS distinct. Le transport video reste a faire.
- Audio Opus, capture et lecture natives ; tampons bornes, traitement audio sans
  allocation dans notre callback, dissimulation des pertes courtes. L'adaptation
  du debit a la congestion et l'annulation d'echo restent a implementer.
- Partage d'ecran avec capture et encodage materiel propres a chaque plateforme.
  Windows : Windows Graphics Capture ; Linux : portail ScreenCast/PipeWire pour
  Wayland ; macOS : ScreenCaptureKit. Validation des versions et dependances a faire.
- Interface native legere, sans Electron, Chromium, WebView ni runtime JavaScript
  embarque. Le choix de la couche graphique devra respecter ces contraintes.

## Contraintes produit : leger et soigne

La sobriete en ressources est une exigence du projet, au meme titre que la
qualite visuelle. Une interface sombre lisible, une hierarchie claire des salons,
des etats micro/connexion visibles et des raccourcis clavier sont prevus.
Les effets visuels devront rester discrets et desactivables.

- Coeur applicatif en C17 ; adaptateurs natifs minimaux si une API systeme exige
  un autre langage, notamment pour la capture et l'integration macOS.
- Interface pilotee par les evenements : redessiner les zones modifiees, sans
  boucle de rendu permanente lorsque la fenetre est immobile ou masquee.
- Aucun service au demarrage par defaut, aucune telemetrie, publicite, boutique
  ou fonction sociale annexe. Les mises a jour devront rester explicites.
- Capture d'ecran et encodeur video actifs seulement pendant un partage ; arret
  et liberation des ressources a sa fin. Frequence et resolution configurables.
- Historique affiche par portions, files d'attente et caches bornes. Pas de
  chargement de toutes les conversations en memoire.
- Dependances ajoutees seulement pour un besoin identifie ; evaluer leur poids,
  leur maintenance, leur licence et leur surface d'attaque avant integration.

Mesurer sur une machine de reference le demarrage a froid, la taille installee,
la memoire residente, le CPU et les reveils au repos, en vocal et en partage.
Verifier aussi la consommation fenetre reduite et apres la fin d'un appel.
Les budgets chiffres seront fixes apres un premier prototype representatif ;
aucune performance n'est encore mesuree pour ces fonctions.

## Etapes

1. Choisir et integrer une pile P2P maintenue avec authentification des pairs,
   invitations et connexion entre deux machines sur des reseaux differents.
2. Texte prive avec gestion des reconnexions, limites et tests de paquets hostiles.
3. Vocal a plusieurs : mute, choix du micro, controle de congestion et mesures.
4. Partage d'ecran avec permissions explicites et indicateur permanent de capture.
5. Interface, paquets d'installation et validation sur les trois systemes.

## Securite et performances

Le C ne garantit pas la securite memoire. Les objectifs incluent analyse statique,
sanitizers, fuzzing des parseurs et revue independante avant diffusion. Les tests
actuels ne constituent pas un audit. Ne jamais journaliser de cles ni de contenu.

Une connexion directe revele l'IP aux pairs. Un mode relais uniquement pourra
masquer l'IP aux autres participants, au prix de bande passante sur le relais.
Un pair autorise peut enregistrer les conversations ; le chiffrement ne l'empeche pas.
Les messages hors ligne necessiteront un pair en ligne ou un stockage chiffre relais.

Le maillage complet sera destine aux petits groupes ; son debit montant augmente
avec le nombre de pairs. Les objectifs CPU/RAM/latence devront etre mesures sur
des machines et reseaux de reference, sans promettre de chiffres avant les mesures.

References : [ICE](https://www.rfc-editor.org/info/rfc8445/),
[Opus](https://opus-codec.org/docs/).
