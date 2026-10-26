# Murmur — historique des versions

## 0.8.0 — 26 octobre 2026

- **Accueil** au premier lancement (portrait du moodboard, trois étapes : bienvenue, nom/jour-nuit/couleurs, premier cercle ou invitation), passable et rejouable depuis Paramètres → Apparence.
- **Deux couleurs d'accentuation** : principale (boutons, sélection) et secondaire (vos messages, pastilles, voix, voyants), toutes deux au choix ; **quatre logos** au choix (Pétales, Ondes, Anneaux, Cercles) ; nouvelles icônes (Cercles, Appels en ondes, Direct en deux cercles, Paramètres hexagonal) ; images du moodboard sur les couvertures des cercles.
- Fin des lueurs (halos, ombres colorées, taches lumineuses) : l'interface est à plat.
- **Audit de sécurité** : un message de type `constructor` désactivait la limite de débit du service (corrigé et testé, service redéployé) ; un message « membre » reçu ne peut plus faire passer quelqu'un pour propriétaire ; un changement de rôle sans signature valide est ignoré ; une clé de membre inutilisable n'empêche plus les autres de recevoir la nouvelle clé ; les adresses de service des invitations qui visent un réseau local sont refusées ; les serveurs ICE reçus sont filtrés (`stun:`, `turn:`, `turns:`).
- **Nouvelle identité visuelle** d'après le moodboard : Encre et Sable, corail en accent par défaut, logo en deux pétales, titres en Bahnschrift, boutons et champ de saisie arrondis, messages envoyés en couleur d'accent pleine. **Nuit** (défaut), **Jour** et **Auto** (suit le réglage jour/nuit de Windows). Les accents Corail, Lime, Violet et Graphite de la palette rejoignent les autres, et n'importe quelle couleur reste possible.
- Les serveurs deviennent des **cercles** dans toute l'interface, avec une page d'accueil en cartes à couverture (six motifs, choisis d'après le nom).
- Thème **Marathon** (inspiré du jeu, aucun de ses éléments n'est utilisé) : noir, jaune-vert acide, angles tranchés, quadrillage et lignes de balayage fixes ; il propose sa couleur d'accentuation en s'activant.
- **Sons de l'interface** fabriqués par l'application (message envoyé et reçu, mention, entrée et sortie d'un appel, micro coupé, panneaux) : rien à télécharger, activables et réglables par personne dans Paramètres.
- **Animations de messages** : un message qui arrive glisse en place, un message modifié clignote une fois, une réaction qui s'ajoute rebondit ; une page d'historique ne s'anime pas.
- `scripts/setup_turn.ps1` pour saisir les secrets du relais sans les afficher, et `server/test/turn_live.mjs` pour vérifier que le service en ligne distribue bien un relais.
- **Paramètres du serveur** (clic sur l'en-tête du serveur) : Aperçu, Salons, Membres (rôles, exclusion, propriété, réintégration), Invitations (durée et nombre d'utilisations au choix), Avancé. La colonne de gauche ne garde qu'une liste de membres calme, avec leur rôle.
- Apparence : lueur d'accent, dégradés, transitions de pages, anneau de parole qui pulse, boutons à reflet ; uniquement des animations de transformation et d'opacité (peu coûteuses), désactivées si le système demande moins de mouvement.
- Correction : à la création d'un serveur, le nom du fondateur remplaçait le nom du serveur. Le champ `title` porte désormais le nom du serveur (`name` reste celui du fondateur).
- Service `murmur-service` déployé sur Cloudflare Workers.
- Salons vocaux de groupe (maillage WebRTC jusqu'à 8), signalisation chiffrée et signée, relais TURN à la demande, statistiques d'appel.
- Messagerie : réponses, modification, suppression, réactions, emoji, mentions, indicateur de saisie, pièces jointes chiffrées, recherche, notifications fenêtre réduite.
- Rôles : modérateur, transfert de propriété, réintégration d'un exclu.
- Seuils personnels : seuil du micro et sensibilité de « qui parle », propres à chaque personne.
- Pas encore : SFU, secret après compromission, déploiement du service, mesures sur un vrai réseau, SpeexDSP natif (optionnel, non testé).
- Nouveau nom : **Murmur** (anciennement Direct) dans la fenêtre, l'exécutable `Murmur.exe`, les messages et la documentation.
- Les codes amis `direct2|` restent émis pour rester lisibles par les versions 0.7 ; le préfixe `murmur2|` est déjà accepté.
- Les mises à jour continuent d'utiliser les fichiers `Direct-win-x64.*` du dépôt `Jxlio/Direct`.
- Le mode natif (`--native-ui`) et ses panneaux Win32 sont supprimés : l’interface est uniquement web (WebView2). Sans WebView2, Murmur l’indique puis se ferme.
- Script `scripts/measure_performance.ps1` et document de performance.
- Identité de l'utilisateur : clé ECDSA P-256 protégée par Windows, empreinte affichée dans Paramètres.
- Messages envoyés à la page par différences et liste fenêtrée : 10 000 messages restent fluides, au plus 200 nœuds dans la page.
- Correction : la zone de conversation ne défilait pas quand elle contenait beaucoup de messages (le champ de saisie sortait de la fenêtre).
- Serveurs (bêta) : créer ou rejoindre un serveur, salons, historique, invitations `murmur4|…`, non-lus, reconnexion automatique.
- **Qui parle** : un anneau de couleur autour de l'avatar de la personne qui parle pendant un appel, mesuré sur le son réel (micro et réception).
- **Couleur d'accentuation** (vert par défaut) et **sept thèmes** : Sombre, Minuit, Noir pur, Crépuscule, Clair, Contraste élevé, Marathon ; taille du texte réglable ; fichier `user.css` pour tout personnaliser.
- Chacun choisit son nom sur les serveurs (signé, modifiable, homonymes distingués par un suffixe).
- Le propriétaire peut exclure un membre : nouvelle clé de groupe pour les messages suivants, l'exclu est déconnecté et ne peut plus revenir avec la même identité, ses anciens messages restent vérifiables.
- Les messages d'un serveur sont chiffrés de bout en bout (AES-256-GCM, clé de groupe remise par le propriétaire, messages signés). L'invitation contient l'empreinte du propriétaire pour déjouer un faux propriétaire. Pas de secret après compromission ni d'exclusion de membre pour l'instant.
- Nouveau service isolé `server/` (Cloudflare) et son contrat de protocole, vérifiés de bout en bout avec la vraie application.
- Mémoire : options du moteur WebView2 mesurées une à une, profil déplacé dans `%LOCALAPPDATA%\Murmur\webview2`, page suspendue quand la fenêtre est réduite hors appel (environ −10 % au repos).
- Plans : architecture des serveurs et contrat de protocole.


## 0.7.0 — 21 octobre 2026

- Mise en relation automatique via un service privé Cloudflare Workers Free.
- Nouveau code ami : un seul échange transmet aussi la configuration du service.
- Reconnexion du service et nouvelles tentatives P2P sans échanger de codes de session.
- Descriptions de connexion chiffrées, challenge de session et routes opaques.
- Présence des amis et nom du correspondant dans la conversation.
- Aucun relais TURN activé : les réseaux bloquant le P2P peuvent rester incompatibles.

## 0.6.1 — 21 octobre 2026

- Navigation séparée entre amis et conversations, avec avatar et présence du correspondant.
- Bulles distinctes pour les messages envoyés et reçus, noms d’auteur et heures.
- Paramètres audio : choix du micro et de la sortie, volumes indépendants enregistrés localement.
- Tests du micro avec indicateur de niveau et son de test sur la sortie sélectionnée.
- Appel entrant, réponse, attente et état des micros visibles ; raccrochage des deux côtés.
- Test solo accessible depuis les paramètres : deux clients connectés automatiquement, messages chiffrés et audio simulé sans modifier les contacts personnels.

## 0.6.0 — 21 octobre 2026

- Panneau Versions et mises à jour dans l'application Windows.
- Vérification au lancement depuis les releases de Jxlio/Direct.
- Téléchargement d'un exécutable authentifié par signature ECDSA P-256.
- Installation différée à la fermeture, avec conservation de l'ancien exécutable.
- Historique local hors ligne et notes des dix dernières releases GitHub.

## 0.5.1 — 21 octobre 2026

- Nouveau nom : Direct — Messages et appels.
- Interface claire, panneaux translucides et pictogrammes Lucide.
- Connexion Internet et diagnostics réseau plus explicites.

## 0.5.0 — 20 octobre 2026

- Contacts persistants, protégés par le compte Windows.
- Ajout d'ami par code privé et découverte chiffrée sur le réseau local.

## 0.4.0 — 20 octobre 2026

- ICE/STUN/TURN et premier relais natif pour les essais.
- Messages fiables et vocal sur une connexion DTLS.

## 0.3.0 — 20 octobre 2026

- Réordonnancement audio et compensation des pertes courtes.

Ces versions sont des prototypes. La connexion entre deux box, les périphériques
audio réels et les interfaces Linux/macOS restent à valider ou à compléter.
