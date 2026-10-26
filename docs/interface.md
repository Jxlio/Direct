# Interface Windows

Le cœur réseau, le chiffrement, les messages et l’audio restent natifs. La fenêtre
principale est dessinée par WebView2 à partir des fichiers de `src/windows/ui` :
`index.html` pour la structure, `style.css` pour l’apparence,
`app.js` pour les interactions. Ces fichiers sont intégrés à l’exécutable afin
que les mises à jour emportent aussi l’interface. Aucun framework JavaScript ni ressource distante
n’est chargé. Le pont `web_ui.cpp` transmet les états de l’application et les
actions autorisées ; le contenu des messages est inséré avec `textContent`.

Les messages, amis, appels, réglages audio, connexion manuelle et mises à jour
sont accessibles dans une seule fenêtre. Le partage d’écran utilise
la capture et WebRTC avec H.264 quand il est disponible. Le flux s’affiche dans l’onglet
Appels et peut passer en plein écran. La résolution et la cadence affichées
proviennent des statistiques du flux reçu ; la cible est 1080p à 60 images/s,
avec adaptation à la source, à la machine et au réseau.
Le bouton `+` de la conversation envoie des JPEG, PNG et GIF animés dans la
connexion chiffrée. La taille est limitée à 2 Mo par fichier ; les images sont
validées avant affichage et gardées seulement en mémoire pendant la session.
Le cache conserve jusqu’à huit images, soit 16 Mo au maximum. Les deux personnes
doivent utiliser la nouvelle version pour échanger ces fichiers. Les liens
HTTP(S) sont cliquables avec une carte d’aperçu ; une vidéo YouTube ne charge
son lecteur intégré qu’après un clic.
La barre supérieure de l’interface web permet de déplacer, réduire, agrandir et
fermer la fenêtre ; ses bords restent redimensionnables. Si le composant WebView2
est absent, Murmur l’indique et se ferme : il n’existe plus d’interface native de secours.

WebView2 donne accès à un vrai moteur HTML/CSS/JS, mais son coût mémoire est
important. Mesure du 25 octobre 2026 (voir [performance.md](performance.md)) :
environ **164 Mo de mémoire privée** et **359 Mo de mémoire résidente** au repos
pour Murmur et ses six processus WebView2 réunis. Ces valeurs varient selon la
machine. Une seule vue est créée, les ressources sont locales et l’interface ne
comporte aucune boucle de rendu inactive.

## Serveurs

La page **Serveurs** crée ou rejoint un serveur (adresse du service Murmur et nom, ou invitation
`murmur4|…`). Un serveur affiche ses salons, l'historique par pages, les non-lus et, pour son
propriétaire, la création de salons et d'invitations. Les messages d'un serveur sont chiffrés de bout en bout
(clé distribuée par le propriétaire) ; le service ne voit que du texte chiffré. Le détail du protocole est dans
[protocole.md](protocole.md) ; l'hébergement du service dans [../server/README.md](../server/README.md).

Sur la page **Serveurs**, la carte **Votre nom** fixe le nom que les autres membres voient (le même sur tous vos serveurs, modifiable
à tout moment). Dans un serveur, la liste des membres montre chacun avec son nom ; le propriétaire (★) peut exclure un membre
(confirmation demandée) : la clé de groupe change pour la suite et l'exclu est déconnecté.

## Apparence et thèmes

**Paramètres → Apparence** règle trois choses, enregistrées sur l'appareil et appliquées avant le premier affichage :
un **thème** (Auto, Nuit, Jour, Minuit, Noir pur, Crépuscule, Contraste élevé, Marathon : choisir Marathon règle aussi l'accent sur son jaune-vert), une **couleur d'accentuation** (huit
teintes dont le vert par défaut, ou n'importe quelle couleur) et la **taille du texte** (100 %, 112 %, 125 %).
Le texte posé sur l'accent est blanc ou presque noir selon ce qui se lit le mieux. Un test automatique
(`tests/ui/theme.test.cjs`) vérifie le contraste de chaque niveau de texte sur chaque surface de chaque thème, et
l'étiquette de chaque couleur d'accent.

Toutes les couleurs de `style.css` sont des variables (`--bg-main`, `--bg-card`, `--text`, `--line`, `--accent`…) ; un
thème ne fait que leur donner d'autres valeurs. Pour aller plus loin, le fichier **`%LOCALAPPDATA%\Murmur\user.css`**
(bouton « Ouvrir le dossier », qui le crée avec un exemple) s'applique par-dessus : on peut y redéfinir ces variables
ou n'importe quel style, puis cliquer « Recharger user.css ». L'accent choisi dans l'interface est posé par la page :
pour le remplacer depuis `user.css`, ajouter `!important`. Le fichier est limité à 256 Ko, ne peut charger aucune
ressource externe (la politique de sécurité de la page l'interdit) et reste sur l'appareil.

## Qui parle

Pendant un appel, un anneau de la couleur d'accentuation entoure l'avatar de la personne qui parle (vous ou votre ami),
le voyant d'appel de la barre latérale s'allume et la page Appels l'indique en toutes lettres. La mesure vient du son
réellement envoyé (micro, après réglage du volume ; zéro si le micro est coupé) et du son réellement reçu, sur une échelle
de 0 à 100 ; au-dessus de 12, la personne compte comme parlant. L'application ne prévient la page que quand un niveau
bouge sensiblement : un appel silencieux ne réveille rien.

## Messagerie

Dans un serveur : réponse à un message, modification et suppression (par l'auteur ; suppression aussi par l'équipe),
réactions emoji, sélecteur d'emoji, mentions `@nom`, indicateur de saisie, pièces jointes (images et fichiers, chiffrés
par morceaux avant l'envoi), recherche dans le salon (sur les messages déjà déchiffrés par l'appareil). Quand la fenêtre
est réduite, une nouveauté (message, mention) fait clignoter l'icône de la barre des tâches et affiche une notification.

## Salons vocaux

Un salon de type « vocal » affiche un panneau : rejoindre, micro, son, quitter, volume par personne, anneau de la
personne qui parle et statistiques (chemin direct ou relais, latence, pertes, débit). L'audio passe en direct entre les
participants (maillage, 8 personnes au plus) ; la signalisation transite chiffrée et signée par le service. Annulation
d'écho, réduction de bruit et gain automatique viennent du moteur du navigateur et se règlent dans Paramètres.

## Seuils personnels

Deux réglages locaux à chaque personne (Paramètres → Voix) : le **seuil du micro** (au-dessous, rien n'est envoyé) et la
**sensibilité de « qui parle »** (niveau à partir duquel un anneau s'allume, 12 par défaut). Ils ne sont partagés avec
personne.

## Paramètres du serveur

Un clic sur l'en-tête du serveur (colonne de gauche) ouvre ses paramètres, un panneau par sujet : **Aperçu** (nom,
rôle, empreinte du propriétaire, adresse du service), **Salons** (liste, création pour l'équipe), **Membres** (rôle,
exclusion, transfert de propriété, réintégration, selon vos droits), **Invitations** (durée et nombre d'utilisations)
et **Avancé** (retirer le serveur de la liste). Échap ou un clic à côté ferme le panneau. Renommer un serveur ou un
salon n'est pas encore possible : le service ne propose pas ce geste.

## Sons et animations

**Paramètres → Sons de l'interface** : un interrupteur et un volume, propres à l'appareil. Les sons (envoi, réception,
mention, entrée ou sortie d'un appel, micro ou son coupé, ouverture des paramètres, erreur) sont synthétisés par la page
avec Web Audio ; il n'y a aucun fichier. Un message reçu dans le salon que vous regardez ne fait qu'un léger tic. Les
messages qui arrivent glissent en place, les messages modifiés clignotent une fois ; tout est coupé si Windows demande
moins d'animations.

## Accueil, couleurs et logos

Au premier lancement (aucun nom ni cercle), un accueil en trois étapes propose : un mot de bienvenue, votre nom avec le
choix Nuit/Jour/Auto et deux couleurs, puis la création d'un premier cercle ou l'ajout par invitation. « Passer » le ferme ;
**Paramètres → Apparence → Revoir l'accueil** le rouvre. La **couleur principale** teinte les boutons et la sélection ;
la **couleur secondaire** teinte vos messages, les pastilles, les voyants et l'anneau de parole. Le logo (Pétales, Ondes,
Anneaux, Cercles) se choisit au même endroit. Les images de l’accueil et des couvertures (quinze illustrations) viennent du jeu d’images du projet
et sont embarquées dans l'application.
