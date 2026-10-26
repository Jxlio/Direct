# Service Murmur (protocole v2)

Service isolé : serveurs (guilds), salons, invitations et enveloppes signées.
Il ne lit jamais le contenu des messages. Le contrat est dans
[../docs/protocole.md](../docs/protocole.md) ; ce dossier ne partage aucun code avec le client.

| Fichier | Rôle |
| --- | --- |
| `src/protocol.mjs` | constantes, limites, vérification des signatures (WebCrypto) |
| `src/guild.mjs` | logique du protocole pour un serveur, sans socket ni hébergeur |
| `src/index.mjs` | hébergement Cloudflare : Worker + un Durable Object par serveur |
| `test/contract.mjs` | suite de contrat, valable pour toute implémentation |
| `test/harness.mjs` | clients de test, en mémoire ou par WebSocket |

## Tester

```sh
cd server
npm install        # une fois ; installe wrangler et ws
npm test           # contrat sur le cœur du protocole, en mémoire
```

Contre un vrai runtime Cloudflare local (workerd, Durable Objects, stockage SQLite) :

```sh
npm run dev                                   # terminal 1, écoute sur 127.0.0.1:8788
MURMUR_SERVICE_URL=http://127.0.0.1:8788 npm test   # terminal 2
```

Sous PowerShell : `$env:MURMUR_SERVICE_URL='http://127.0.0.1:8788'; npm test`.
Un test (invitation expirée) est ignoré en mode distant, car il a besoin d'une horloge contrôlée.

## Déployer

```sh
npm run deploy
```

Nécessite un compte Cloudflare et `wrangler login`. Pour les appels derrière un pare-feu strict, créer une clé TURN dans le tableau de bord Cloudflare puis :

```sh
npx wrangler secret put TURN_KEY_ID
npx wrangler secret put TURN_KEY_API_TOKEN
```

Ou, en une fois et avec vérification sur le service en ligne : `.\scripts\setup_turn.ps1` (la connexion de `wrangler` ne peut pas créer la clé elle-même : elle n'a pas la permission Realtime ; la clé se crée dans le tableau de bord, Realtime → TURN). `node test/turn_live.mjs <adresse>` dit seulement si un relais est distribué.

Sans ces secrets, `turn.get` répond `not_found` et les appels n'utilisent que la connexion directe. Le service se déploie sous le nom `murmur-service` (les tests de contrat y passent : `MURMUR_SERVICE_URL=… npm test`). Le service d'amis
(`../service/`) est un autre Worker, avec une autre adresse, et ne répond pas à `/v2/...`. Avant une vraie ouverture :
mesurer le nombre de requêtes d'un serveur actif (quotas gratuits) et prévoir la modération.

## Limites connues

- Les corps de messages sont chiffrés par le client (voir `docs/protocole.md`, section 5) ; le service n'en voit que des octets, plus les métadonnées.
- Rôles : propriétaire, modérateur, membre. Transfert de propriété et réintégration d'un exclu possibles (voir `docs/protocole.md`).
- Voix de groupe en maillage (8 personnes au plus), pas de SFU. Le relais TURN demande deux secrets (ci-dessous).
- Le service d'amis existant (`../service/`) reste séparé et inchangé.

## Test de bout en bout avec l'application

`scripts/e2e_servers.cjs` pilote la vraie application Windows (création d'un serveur, invitation,
pseudos (choix, renommage, homonymes), remise de la clé de groupe, messages chiffrés dans les deux sens, message falsifié, exclusion d'un membre avec changement de clé, nouveau salon, coupure et reconnexion,
faux propriétaire, clé arrivée après l'adhésion, refus de signer ou d'accorder une clé sur des données arbitraires) avec d'autres membres simulés. Il faut le service local (`npm run dev`), puis l'application
lancée avec `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--remote-debugging-port=9223`, puis
`NODE_PATH=service/node_modules node scripts/e2e_servers.cjs http://127.0.0.1:8788`.
Après un redémarrage de l'application, `... http://127.0.0.1:8788 --after-restart <serveur>` vérifie que la liste
et l'historique ont survécu (l'identifiant du serveur est affiché en fin de première exécution).
