# Protocole Murmur

Ce document est le contrat entre le client et le service. Il décrit d'abord ce qui
est **déjà diffusé et figé** (version 0.7), puis le protocole **cible** des serveurs
(non implémenté, voir [architecture-serveurs.md](architecture-serveurs.md)).

## 1. Formats existants, figés

Les noms visibles ont changé en Murmur, mais ces chaînes sont dans des codes et des
paquets déjà échangés. Les modifier casserait les clients 0.7 installés. Elles ne
changeront qu'avec une nouvelle version majeure du protocole.

| Élément | Valeur | Rôle |
| --- | --- | --- |
| Invitation locale | `murmur1\|hôte\|port\|clé hex` | connexion IP manuelle (clé 32 octets) |
| Code ami | texte de 72 caractères | secret de relation de 32 octets |
| Code ami avec service | `direct2\|code\|origine\|accès` | secret + adresse du service + secret d'accès |
| Préfixe équivalent accepté | `murmur2\|` | lu comme `direct2\|` ; **pas encore émis** |
| Route opaque | HKDF-SHA256, domaine `direct/rendezvous/route/v1` | identifie une relation sans la révéler |
| Chiffrement des signaux | AES-256-GCM, domaine `direct/friend/signal/v1` | protège les descriptions de connexion |
| Clé média | HKDF-SHA256, domaine `murmur/dtls/voice/v1` | PSK DTLS de la voix |
| Manifeste de mise à jour | `DIRECT-UPDATE-1`, ECDSA P-256 | mises à jour signées |
| Fichiers de release | `Direct-win-x64.manifest\|.sig\|.exe` depuis `Jxlio/Direct` | clients 0.7 qui se mettent à jour |

Passage à `murmur2|` : le préfixe est lu depuis cette version ; il pourra être émis
une fois que tous les amis ont installé une version qui le lit. Changer
`INVITE_PREFIX` dans `src/rendezvous.c` suffit alors.

### Trames du canal fiable

En-tête de 12 octets, puis charge utile. Types : 1 texte (16 384 octets maximum),
2 contrôle (4 096), 3 audio (404). Le texte commence par un octet de salon (0 à 2).
Les contrôles vocaux valent 0 (arrêt), 1 (actif) ou 3 (actif, micro coupé).
Le transport est DTLS 1.2 avec ECDHE-PSK et ChaCha20-Poly1305.

### Service de rencontre (version 0.7)

| Élément | Valeur |
| --- | --- |
| `GET /health` | `Direct rendezvous v1; TURN disabled` (texte inchangé, déjà déployé) |
| `GET /v1/connect` | WebSocket, `Authorization: Bearer <accès hex 64>` |
| Abonnement | message texte `JOIN\|<route 64 hex><rôle 0 ou 1>\|…`, 16 routes maximum |
| Présence | 33 octets : route + type (0 en ligne, 2 hors ligne) |
| Enveloppe | route (32) + type 1 + contenu chiffré, 99 à 6 097 octets |
| Limites | 32 clients, débit 2 messages par seconde (rafale de 16) |
| Isolation | un seul groupe `private-group-v1`, un client par route et par rôle |

Le service ne reçoit ni texte, ni audio, ni secret de relation.

## 2. Protocole des serveurs, version 2

Implémenté dans [`server/`](../server/README.md) (service Cloudflare isolé) et vérifié par
une suite de contrat de 13 tests, exécutée à la fois sur le cœur du protocole et sur un
vrai runtime Cloudflare local (`wrangler dev`). Le client Windows s'y connecte (section 4) et un test de bout en bout
pilote la vraie application.

### Principes

1. **Le service ne lit jamais le contenu.** Il vérifie les signatures, les tailles et les
   permissions, ordonne les enveloppes et les stocke opaques. Le champ `body` est chiffré par
   les clients (section 5) : le service n'en voit que des octets.
2. **Identité = clé ECDSA P-256.** Elle remplace le « Ed25519 » envisagé d'abord : la
   bibliothèque cryptographique du client (mbedTLS) ne propose pas Ed25519, alors que
   P-256 est déjà utilisé pour les mises à jour et fonctionne dans WebCrypto côté service.
   L'empreinte est le SHA-256 de la clé publique (65 octets, point non compressé).
   Les signatures sont au format brut `r||s` (64 octets) sur SHA-256.
3. **Un serveur (guild) = un Durable Object**, nommé par un identifiant aléatoire de 32 caractères hexadécimaux choisi par son créateur.
4. **Borné.** Toutes les tailles et tous les débits ont une limite écrite ci-dessous.

### Connexion

```
GET /v2/guild/<id 32 hex>/connect?protocol=2      (Upgrade: websocket)
```

Le service envoie `hello` avec un nonce aléatoire. Le client répond par `auth` avec sa
clé publique et la signature de `murmur/v2/auth|<id du serveur>|<nonce>`. Le nonce est
propre à la connexion et le serveur est inclus dans la signature : une signature ne vaut
ni pour une autre connexion, ni pour un autre serveur. Il n'y a aucun secret de groupe.

### Trames (JSON texte, 24 000 caractères maximum)

| Type | Sens | Contenu |
| --- | --- | --- |
| `hello` | service → client | `protocol`, `nonce`, `limits` |
| `auth` | client → service | `pub` (130 hex), `sig` (128 hex) |
| `ready` | service → client | `exists`, `member`, `you`, puis si membre : `role`, `name`, `owner`, `channels`, `last` |
| `guild.create` | client → service | `name`, `enc`, `encsig`, `name` (pseudo), `namesig` ; l'auteur devient propriétaire, salon `general` créé |
| `invite.create` | propriétaire | `max_uses` (1 à 100), `ttl_s` (60 s à 7 jours) → `invite` avec `code` (32 hex) |
| `guild.join` | client → service | `invite`, `enc`, `encsig`, `name`, `namesig` ; le nouveau membre reçoit `ready`, les membres reçoivent `member` (avec ses clés) |
| `members.list` | membre | → `members` : `epoch`, `items` (pour chaque membre `fp`, `pub`, `enc`, `encsig`, `name`, `namesig`, `role`, `keyed` pour la période courante) et `former` (membres exclus : `fp`, `pub`, `name`, `namesig`) |
| `profile.set` | membre | `name`, `namesig` → `ack` ; les membres reçoivent `profile` |
| `member.remove` | propriétaire | `fp`, `epoch` (la période suivante exactement) → `ack` ; les membres reçoivent `removed`, puis le service ferme les connexions de l'exclu |
| `keys.put` | propriétaire | `to` (empreinte), `epoch`, `blob` (clé de groupe enveloppée, base64, 700 caractères maximum) → `ack` ; les membres reçoivent `keys` (`to`, `epoch`, sans le blob). Une période pas encore ouverte (courante + 1) ne peut être préparée que pour le propriétaire lui-même |
| `keys.get` | membre | → `key` avec `items` : tous les blobs qui lui sont destinés (`epoch`, `blob`, `from`), les 32 dernières périodes |
| `channel.create` | propriétaire | `name`, `type` (`text` ou `voice`) → `channel` (aussi diffusé) |
| `envelope.send` | membre | `channel`, `id` (8 à 40 caractères), `body` (base64, 16 Ko brut), `sig` |
| `ack` | service → client | `seq`, `id` |
| `envelope` | service → membres | `channel`, `seq`, `id`, `from` (empreinte), `ts`, `body`, `sig` |
| `history.fetch` | membre | `channel`, `limit` (≤ 50), `before` ou `after` (numéros de séquence) → `history` avec `items`, `more` |
| `ping` | les deux | → `pong` |
| `error` | service → client | `code`, `ref`, parfois `retry_after` |

Chaque requête peut porter un `ref` que la réponse reprend. Codes d'erreur : `bad_request`,
`unauthorized`, `forbidden`, `not_found`, `conflict`, `too_large`, `rate_limited`.

La signature d'une enveloppe porte sur `murmur/v2/envelope|<serveur>|<salon>|<id>|<body>` :
tout membre peut la revérifier avec la clé publique de l'auteur, le service ne peut donc
pas fabriquer de message au nom de quelqu'un. Le numéro `seq` est attribué par le service
et croît de 1 à chaque enveloppe du serveur.

### Limites

| Élément | Maximum |
| --- | --- |
| Trame | 24 000 caractères (au-delà : `too_large` et fermeture) |
| Enveloppe | 16 384 octets bruts |
| Requêtes | 5 par seconde par connexion, rafale de 20 |
| Historique par page | 50 |
| Membres par serveur | 100 (en attendant de mesurer le coût du chiffrement de groupe) |
| Salons par serveur | 200 |
| Connexions simultanées par serveur | 200 |

### Trames ajoutées (messagerie, rôles, pièces jointes, voix)

| Type | Sens | Contenu |
| --- | --- | --- |
| `typing` | membre | `channel` → diffusé aux autres (coût 0,5, jamais stocké) |
| `blob.put` / `blob.get` | membre | morceau d'un fichier chiffré (`id`, `index`, `data` base64, 64 000 caractères, 24 morceaux, 64 Mo) ; seule trame autorisée au-delà de 24 000 caractères (jusqu'à 70 000) |
| `role.set` | propriétaire | `fp`, `role` (`member` ou `moderator`), `sig` sur `murmur/v2/role|…` ; les modérateurs peuvent exclure des membres simples |
| `owner.transfer` | propriétaire | `to`, `sig` sur `murmur/v2/owner|…` (le destinataire doit avoir la clé de groupe) ; la chaîne de propriétaires reste vérifiable |
| `member.unban` | propriétaire | `fp` : l'exclu peut de nouveau rejoindre avec une invitation |
| `voice.join` / `voice.leave` / `voice.signal` | membre | salons vocaux (maillage WebRTC, 8 personnes) : `signal` transporte offre et réponse chiffrées et signées (`murmur/v2/voice|…`, 16 000 caractères), relayées à une seule personne |
| `turn.get` | membre | → identifiants TURN temporaires (API TURN de Cloudflare ; nécessite les secrets `TURN_KEY_ID` et `TURN_KEY_API_TOKEN`) |

`ready` contient aussi `presence` : qui est déjà dans chaque salon vocal. Les contenus (texte, réponse, mentions, fichiers,
modification, suppression, réaction) sont dans la charge utile **chiffrée** de l'enveloppe ; le service ne voit que des
octets. Les règles (modifier = auteur seul ; supprimer = auteur ou équipe ; réaction = tout le monde ; une modification
ne vise qu'un `seq` antérieur) sont appliquées par les clients : un message « supprimé » reste chiffré sur le service.

### Limites de confiance connues (audit du 26 octobre 2026)

Le chiffrement protège le contenu contre le service tant que celui-ci se comporte comme annoncé. Un service **malveillant**
garde des pouvoirs que le protocole actuel ne retire pas :

- **Faux membre.** Le service fournit la liste des membres ; un membre fabriqué (identité signée par lui-même, sans invitation
  réelle) recevrait la clé de groupe de la part d'un propriétaire ou modérateur connecté. Piste : invitations portant une clé
  publique signée par l'équipe, ou approbation de chaque nouvelle empreinte avant remise de clé.
- **Numéros de séquence.** Les modifications, suppressions et réactions visent le `seq` attribué par le service, qui peut les
  renuméroter ou rejouer une enveloppe valide. Piste : viser l'`id` signé et refuser un couple (auteur, id) déjà vu.
- **Clés de groupe envoyées par un modérateur.** C'est voulu (un modérateur qui exclut quelqu'un renouvelle la clé), mais un
  modérateur peut donc proposer une clé que lui seul maîtrise à un nouveau venu. Piste : clé de groupe signée par le propriétaire.
- **Ressources.** Le quota de 64 Mo de pièces jointes n'est jamais libéré, les messages n'ont pas de plafond ni de durée de
  conservation, créer un serveur ne coûte rien, et des connexions non authentifiées occupent des places. Piste : durées de
  vie, plafonds par membre, délai d'authentification.
- **Liaison de la signature de connexion à l'adresse du service** : non faite (la signature couvre l'identifiant du serveur et
  un défi, pas l'adresse).

### Ce qui reste à faire

- Vocal de groupe par SFU (le maillage actuel se limite à 8 personnes).
- Secret après compromission (ratchet) : non fait, il s'opposerait à l'historique relisible par les nouveaux appareils.
- Le service n'a pas été déployé ; l'API TURN n'a pas été essayée contre le vrai service Cloudflare.

## 3. Identité côté client

`include/murmur/identity.h` : génération, signature, vérification et empreinte (tests
`identity_sign_verify`). `src/windows/identity_store.c` conserve la clé secrète dans
`%LOCALAPPDATA%\Murmur\identity.dat`, protégée par DPAPI et liée au compte Windows
(tests `windows_protected_identity`). Un fichier endommagé n'est jamais remplacé
silencieusement. L'empreinte abrégée (16 caractères) est affichée dans Paramètres.
Limite : l'identité n'est pas transférable d'un ordinateur ou d'un compte Windows à un autre.

## 4. Client Windows et service

Le client ne donne à la page web aucun accès réseau : la page garde sa politique de sécurité
d'origine unique, et l'application relaie les trames.

| Action page → application | Effet |
| --- | --- |
| `service-open:<emplacement>\|<origine>\|<serveur>` | ouvre le WebSocket (4 emplacements) ; `https://` partout, `http://` seulement pour `127.0.0.1` et `localhost` |
| `service-send:<emplacement>\|<trame>` | envoie une trame texte (24 000 octets maximum, file de 16) |
| `service-close:<emplacement>` | ferme |
| `identity-sign:<id>\|<texte>` | signe `<texte>` avec l'identité, **uniquement** s'il commence par `murmur/v2/auth\|`, `murmur/v2/envelope\|`, `murmur/v2/enc\|` ou `murmur/v2/name\|` ; sinon la réponse est `null` |
| `identity-ecdh:<id>\|<clé publique>` | accord de clé P-256 entre la clé de chiffrement de l'utilisateur et celle d'un pair ; `null` si le point n'est pas valide |

Réponses application → page : `{type:"service", slot, event:"open"/"message"/"close", code, data}`,
`{type:"signature", id, signature}` et `{type:"ecdh", id, secret}`. La page conserve la liste de ses serveurs (adresse, identifiant,
nom) dans son stockage local ; elle se reconnecte seule avec attente progressive, rattrape l'historique
manquant après une coupure ou une mise en veille, et le recharge au redémarrage.

Invitation à partager : `murmur4|<origine>|<serveur>|<code>|<empreinte du propriétaire>` (32 caractères hexadécimaux pour le serveur et le code, 64 pour l'empreinte).

## 5. Chiffrement de bout en bout

Le chiffrement est fait par la page (`e2ee.js`, primitives WebCrypto standard : ECDH P-256, HKDF-SHA-256,
AES-256-GCM). Le service ne reçoit jamais de clé de groupe ni de texte clair.

**Clés.** Chaque utilisateur a une clé de chiffrement P-256 dérivée de son identité
(HMAC-SHA-256 de son secret avec l'étiquette `murmur/v2/enc|`) : elle se retrouve avec l'identité
seule et ne sert jamais à signer. Sa partie publique est publiée avec une signature de l'identité
sur `murmur/v2/enc|<serveur>|<clé>` ; le service la vérifie et les autres membres aussi.

**Clé de groupe.** Le propriétaire tire 32 octets au hasard à la création du serveur (période 1).
Pour chaque membre, il calcule un accord ECDH entre sa clé de chiffrement et celle du membre, en tire
une clé d'enveloppe (HKDF, étiquette `murmur/v2/keywrap|<serveur>|<période>|<clé expéditeur>|<clé destinataire>`)
et chiffre la clé de groupe en AES-GCM. Le blob est confié au service (`keys.put`) ; il reste donc
récupérable après une réinstallation ou un redémarrage, et le propriétaire n'a pas besoin d'être en ligne
quand un membre se reconnecte. Un membre n'accepte une clé que du propriétaire désigné par son invitation.

**Messages.** Sous-clé par salon (HKDF de la clé de groupe, étiquette `murmur/v2/msg|<serveur>|<salon>`),
AES-256-GCM, nonce aléatoire de 96 bits. Corps : version (1 octet), période (4), nonce (12), texte chiffré
et étiquette. Les données authentifiées sont `murmur/v2/body|<serveur>|<salon>|<id>|<période>` : un corps
ne peut pas être rejoué dans un autre salon, sous un autre identifiant ou comme appartenant à une autre période.
Le contenu est `{"v":1,"text":"…"}`. La signature d'enveloppe (section 2) porte sur le corps chiffré.

**Vérifications côté client.** Pour chaque message : la signature est contrôlée avec la clé publique du
membre (dont l'empreinte est le hachage), puis le déchiffrement. Un échec s'affiche comme une note
(« signature invalide », « illisible ou modifié », « en attente de la clé »), jamais comme un texte ou une absence silencieuse.

**Confiance de départ.** L'invitation contient l'empreinte du propriétaire. Si le service annonce un autre
propriétaire, le client refuse de se connecter : un service malveillant ne peut pas se faire passer pour lui.
L'invitation doit donc être transmise par un canal sûr. L'empreinte abrégée du propriétaire reste affichée
dans le serveur pour qu'on puisse la comparer de vive voix.

### Ce que ce chiffrement ne fait pas

- **Pas de secret après compromission d'une clé.** Une même clé de groupe protège tous les messages
  d'une période : si elle fuit, toute la période est lisible. MLS ou un cliquet par message le corrigeraient ; ce n'est pas fait.
- **Une exclusion protège l'avenir, pas le passé.** Après `member.remove`, la période suivante a une nouvelle clé que
  l'exclu ne reçoit jamais ; mais il garde les messages et les clés des périodes précédentes qu'il a déjà reçus, et un
  membre qui a copié la clé de groupe avant son exclusion pourrait la donner à quelqu'un d'autre pour les anciennes périodes.
- **Un nouveau membre lit tout l'historique.** Il reçoit les clés de toutes les périodes (c'est voulu, comme pour rejoindre un salon existant).
- **Le propriétaire est un point central.** Seul lui remet la clé aux nouveaux membres : tant qu'il n'est
  pas venu, ils attendent. S'il perd son identité, le serveur ne peut plus accepter de membres lisibles.
- **Le service voit les métadonnées** : qui est membre, quand et dans quel salon on écrit, la taille des messages, les adresses IP.
  Il peut aussi retarder, supprimer ou refuser des messages, mais pas les lire ni les modifier sans que la signature ou le déchiffrement échoue.
- **Aucun audit indépendant.** La construction n'utilise que des primitives standard, mais l'assemblage est propre à Murmur.

### Exclusion d'un membre

Le propriétaire (1) tire une nouvelle clé et la prépare pour lui-même à la période suivante, (2) envoie
`member.remove` avec cette période : le service supprime le membre et ses clés, inscrit son identité sur la liste
des exclus (elle ne peut plus rejoindre, même avec une nouvelle invitation), ouvre la période et ferme ses
connexions, puis (3) enveloppe la nouvelle clé pour chaque membre restant. Si le propriétaire est interrompu entre (2)
et (3), sa clé préparée lui reste : à la reconnexion, il remet la clé aux membres qui ne l'ont pas encore.
Les membres restants gardent les anciennes clés et lisent donc tout l'historique ; les nouveaux messages utilisent la
nouvelle période, que l'exclu ne peut pas lire.

L'identité et le pseudo signé d'un exclu restent disponibles (`former`) : la clé doit hacher vers l'empreinte inscrite
sur ses anciens messages, le service ne peut donc pas en substituer une autre, et ces messages restent vérifiables.

### Pseudos

Chacun choisit son nom (1 à 32 caractères, sans caractère de contrôle ni de changement de sens d'écriture). Il est
signé par l'identité pour un serveur donné (`murmur/v2/name|<serveur>|<nom>`), donc le service ne peut renommer personne
ni réutiliser un nom d'un serveur à l'autre. **Le pseudo n'est pas chiffré** : le service et les membres le voient.
Deux membres peuvent choisir le même nom : le client ajoute alors un suffixe de leur empreinte (`alice #6c71`).
