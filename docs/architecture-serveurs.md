# Plan d'architecture : serveurs, salons et appels de groupe

Statut : proposition, rien n'est implémenté. Ce document remplace la question
« comment passer de 1 contre 1 à un clone de Discord ? » par des décisions
à valider une par une.

## 1. Pourquoi l'architecture actuelle ne suffit pas

| Limite actuelle | Cause | Conséquence |
| --- | --- | --- |
| Une seule conversation | `transport` est un singleton statique dans `link.c` | pas de salons ni de groupes |
| Pas d'historique | messages en mémoire, 128 maximum | rien après un redémarrage |
| Pas de message hors ligne | P2P pur, il faut les deux en ligne | incompatible avec un salon |
| Identité = nom local | un secret partagé par ami, aucune identité | pas de rôles, bans ni révocation |
| Appels à deux | une association DTLS par appel | un maillage P2P ne tient pas au-delà de 4 à 5 |

Le P2P 1 contre 1 reste excellent pour les messages privés (léger, sans serveur,
chiffré de bout en bout). Il est conservé tel quel comme mode « Message direct ».

## 2. Décision structurante : qui héberge un serveur ?

| Option | Principe | Pour | Contre |
| --- | --- | --- | --- |
| A. Cloudflare | un Durable Object par serveur (guild), D1 ou stockage SQLite du DO pour l'historique, R2 pour les fichiers | gratuit au départ, aucune machine à gérer, la mise en relation existe déjà | quotas, dépendance à un fournisseur, difficile à auditer |
| B. Auto-hébergé | un binaire `direct-server` (C ou Go), SQLite, WebSocket | contrôle total, pas de quota | l'ami qui héberge doit avoir une machine allumée et un port ouvert |
| C. P2P répliqué | chaque membre garde l'historique, synchronisation entre pairs | aucun serveur | complexe, messages perdus si personne n'est en ligne |

**Recommandation : définir un seul protocole serveur et deux hébergements (A puis B).**
Le client ne sait pas quel hébergement il utilise : il se connecte à une URL
WebSocket. Le Worker actuel (`service/worker.mjs`) devient le premier prototype
de l'option A. C est écartée : trop de cas limites pour un gain faible.

## 3. Modèle de données

```
User      id (clé publique Ed25519), nom affiché, avatar
Server    id, nom, propriétaire, créé le
Member    server_id, user_id, rôles, surnom, rejoint le
Role      id, server_id, nom, couleur, permissions (bitmask), position
Channel   id, server_id, type (texte | vocal), nom, catégorie, position
Message   id (ULID, trié dans le temps), channel_id, auteur, contenu, répond_à, édité le, supprimé le
Invite    code, server_id, créé par, expire le, utilisations maximales
Attachment id, message_id, nom, type MIME, taille, clé de chiffrement, emplacement
```

Choix importants :
- **Identité = paire de clés Ed25519 générée au premier lancement**, protégée par
  DPAPI. Fini les noms-étiquettes : chaque message et chaque invitation est signé.
- **Identifiants ULID** : triables par date, créables hors ligne, sans collision.
- **Permissions en bitmask** (envoyer, gérer les salons, bannir, parler, etc.),
  calculées côté serveur. Le client ne décide jamais d'une permission.

## 4. Protocole client-serveur

- Un WebSocket sur TLS par serveur rejoint, avec reprise (`since=<id du dernier événement>`).
- Messages JSON ou CBOR ; événements : `message.create|edit|delete`, `member.join|leave`,
  `presence`, `typing`, `voice.state`.
- Pagination de l'historique par curseur (`before=<ULID>&limit=50`).
- Limitation de débit par utilisateur et par salon, côté serveur.
- Le serveur ordonne les messages : un seul Durable Object (ou un seul processus)
  par serveur garantit l'ordre sans consensus.

Côté client :
- Cache local **SQLite** (déjà portable en C) pour l'historique et la recherche.
- Le pont C vers JavaScript envoie des **événements différentiels** (ajout d'un
  message, changement de présence) au lieu de l'état complet en JSON.
- Liste de messages **virtualisée** dans le DOM (seuls les ~50 visibles existent).

## 5. Chiffrement

| Niveau | Effet | Coût |
| --- | --- | --- |
| Transport seul (TLS) | le serveur lit tout | le plus simple, comme Discord |
| Chiffrement de bout en bout avec MLS (RFC 9420) | le serveur ne lit rien | recherche côté serveur impossible, gestion des appareils, bibliothèque à intégrer |

**Recommandation : démarrer avec TLS seul pour les salons, garder le bout en bout
pour les messages directs (déjà en place).** Le MLS peut s'ajouter plus tard
sans changer le modèle de données, car le contenu est déjà opaque pour le routage.
Il faut l'annoncer clairement dans l'interface pour ne pas induire en erreur.

## 6. Voix et vidéo en groupe

Un maillage P2P envoie N-1 flux par participant : inutilisable à partir de 5 personnes.
Il faut un **SFU** (Selective Forwarding Unit) qui reçoit un flux par participant
et le redistribue.

| Option | Remarque |
| --- | --- |
| Cloudflare Realtime SFU | s'intègre avec l'option A ; vérifier les quotas gratuits actuels avant de s'engager |
| LiveKit (open source) | SFU complet auto-hébergeable, SDK C++ disponible, très utilisé |
| SFU maison | à éviter : beaucoup de travail (congestion, simulcast, bande passante) |

**Recommandation : LiveKit en auto-hébergement pour l'option B, Cloudflare SFU pour l'option A,
derrière une même interface `voice_join(channel)` côté client.** L'appel 1 contre 1
continue d'utiliser la pile ICE/DTLS actuelle.

Le moteur audio de `voice.c` (Opus, jitter buffer, miniaudio) reste utilisé
pour la capture et la lecture. Deux chantiers s'ajoutent : annulation d'écho et
suppression de bruit (WebRTC Audio Processing ou RNNoise), et un indicateur
« qui parle » (détection d'activité vocale).

## 7. Interface

- **Rail des serveurs** à gauche, **liste des salons** par catégorie, **liste des membres** à droite.
- Salon vocal : panneau d'appel **intégré à la fenêtre**, le chat reste utilisable
  pendant un partage d'écran.
- Tout en variables CSS pour que les thèmes (point promis par le README) fonctionnent.

## 8. Feuille de route

État au 25 octobre 2026.

| Étape | Contenu | État |
| --- | --- | --- |
| 0 | Commits, renommage en Murmur, docs, mesure de performance, protocole écrit | fait, sauf les commits (à ta demande) ; mémoire web réduite d'environ 10 % (voir [performance.md](performance.md)) |
| 1 | Identité (ECDSA P-256 au lieu d'Ed25519), stockage protégé, signatures | fait, testé (`identity_sign_verify`, `windows_protected_identity`) |
| 2 | Service minimal : serveur, salons, invitations, enveloppes signées, historique | fait côté service, 13 tests de contrat y compris sur un runtime Cloudflare local ; **client non connecté** |
| 3 | Pont différentiel et liste de messages fenêtrée | fait, testé (`windows_message_diff`, 10 000 messages affichés en 9 ms avec ≤ 200 nœuds) |
| 4 | Connexion du client au service, salons dans l'interface, invitations, pseudos, exclusion de membres | fait, testé de bout en bout (`scripts/e2e_servers.cjs`, 40 vérifications) ; autres rôles à faire |
| 5 | Chiffrement de bout en bout du champ `body` | fait avec une clé de groupe distribuée par le propriétaire (plus simple que MLS : pas de secret après compromission ni d'exclusion, voir [protocole.md](protocole.md#5-chiffrement-de-bout-en-bout)) ; MLS reste possible plus tard |
| 6 | Salon vocal via SFU, annulation d'écho et de bruit | à faire |
| 7 | Fichiers, réponses, réactions, non-lus | à faire |
| 8 | Déploiement réel (compte Cloudflare, quotas mesurés) | à faire, nécessite ton compte |

## 9. Risques

- **Quotas gratuits** : un serveur actif de 30 personnes peut dépasser les limites
  de l'option A. Prévoir une mesure du nombre de requêtes dès l'étape 2.
- **Modération** : héberger du contenu d'autrui engage la responsabilité de
  l'hébergeur (signalement, suppression). À traiter avant toute ouverture publique.
- **Sécurité** : aucun audit indépendant aujourd'hui ; un serveur exposé augmente
  nettement la surface d'attaque (spam, injection, déni de service).
- **Charge de travail** : c'est un projet de plusieurs mois. Mieux vaut valider
  l'étape 2 avec quelques amis avant d'investir dans la voix de groupe.

## 10. Décisions prises (25 octobre 2026)

| Sujet | Décision | Conséquence |
| --- | --- | --- |
| Hébergement | Cloudflare (option A) | un Durable Object par serveur ; le protocole reste indépendant pour permettre l'option B plus tard |
| Chiffrement | bout en bout dès le début (MLS) | le service ne voit que du texte chiffré : pas de recherche ni de modération côté serveur |
| Nom | **Murmur** (fait pour tout ce que l'utilisateur voit, voir CHANGELOG) | les formats déjà diffusés restent figés, voir [protocole.md](protocole.md) |
| Interface | **web (WebView2) uniquement** ; le mode natif est supprimé | voir section 11 |
| Priorité | **performance** | voir section 11 |
| Service | **isolé** du client | voir section 12 |

## 11. La performance comme critère de conception

L'interface web est la seule interface. La performance se joue donc sur la façon
de l'utiliser. Mesure du 25 octobre 2026 : 359 Mo résidents au repos
(voir [performance.md](performance.md)).

- **Budget chiffré à tenir**, mesuré à chaque étape : RAM web au repos < 250 Mo,
  CPU au repos ~ 0 %, ouverture d'un salon de 10 000 messages < 100 ms,
  appel de groupe < 5 % d'un cœur.
- **Le coût vient de WebView2** (7 processus). Leviers : arguments du moteur,
  un seul `WebView` créé une fois, aucune boucle d'animation inactive.
- **Le pont C vers JavaScript** passe des états complets à des événements
  différentiels, et la liste de messages devient virtualisée (seuls ~50 nœuds DOM).
- **MLS a un coût** : chaque ajout ou retrait de membre fait avancer l'arbre de clés.
  Plafonner la taille d'un salon chiffré (100 membres au départ) et mesurer le
  coût d'un ajout avant de monter ce plafond.
- **Voix chiffrée de bout en bout** : un SFU ne peut pas déchiffrer l'audio ; il faut
  SFrame (chiffrement des trames média) avec une clé dérivée du groupe MLS. Le SFU
  de Cloudflare et LiveKit le permettent, mais cela empêche tout traitement
  serveur (transcodage, enregistrement).

## 12. Un vrai service isolé

L'actuel `service/` ne fait que la mise en relation. Il devient un produit à part :

- **Dossier ou dépôt séparé** (`murmur-service`), sa propre version, ses propres tests,
  son propre déploiement. Le client ne partage aucun code avec lui, seulement un
  **contrat de protocole** écrit dans `docs/protocole.md` (messages, erreurs, limites).
- **Aucun secret côté client** hormis la clé de l'utilisateur ; le secret de groupe
  actuel (`GROUP_ACCESS`) disparaît au profit d'invitations signées.
- **Le service ne fait confiance à rien** : il vérifie les signatures, applique les
  limites de débit et les permissions, et ne stocke que du chiffré.
- **Tests de contrat** exécutables contre n'importe quelle implémentation du service
  (Cloudflare aujourd'hui, binaire auto-hébergé demain).
- **Observabilité minimale** : compteurs de requêtes et d'erreurs sans adresses IP
  ni contenu, pour surveiller les quotas.
