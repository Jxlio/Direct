# Service de mise en relation Murmur

Petit service privé Cloudflare Workers Free. Il transmet uniquement des
informations de connexion chiffrées entre les clients natifs. Aucun texte de
conversation, audio ou partage d’écran ne transite par ce service.
Le client reste en C ; aucun runtime JavaScript n’est ajouté à Murmur.exe.

## Déployer une fois pour le groupe

Prérequis : un compte Cloudflare Free et Node.js pour les outils de publication.
Aucune offre TURN, Workers Paid, Realtime ou autre option payante n’est nécessaire.

```sh
npm ci --ignore-scripts
npm test
npx wrangler login --scopes account:read user:read workers:write workers_scripts:write workers_kv:write
npx wrangler deploy
```

Wrangler fournit une adresse HTTPS `*.workers.dev`. Le service refuse les
connexions tant que le secret `GROUP_ACCESS` n’est pas configuré. Générer un
secret aléatoire de 32 octets en hexadécimal dans un fichier privé hors du dépôt,
puis le transmettre à `wrangler secret put GROUP_ACCESS` par l’entrée standard.
Ne pas le passer dans les arguments, le versionner ou l’afficher dans les journaux.

Sur le PC de l’organisateur, exécuter `Install-LocalProfile.ps1` avec l’origine
HTTPS et le chemin du fichier privé. Il conserve le profil avec Windows DPAPI.
Les invitations créées ensuite dans l’application incluent cette configuration.
Les amis l’importent avec leur code, sans compte Cloudflare.

## Fonctionnement

Une connexion WSS par application transporte les inscriptions de 16 relations
maximum. Les routes sont des identifiants opaques de 256 bits et les rôles sont
complémentaires. Le service limite le groupe à 32 connexions, les messages à
6 097 octets et le rythme par connexion. Un doublon du même profil remplace
l’ancien client ; celui-ci suspend ses tentatives après un refus de politique.

La présence et les inscriptions utilisent les pièces jointes des WebSockets
hibernables. Aucun historique n’est enregistré dans une table ou un journal
applicatif. La journalisation Workers est désactivée dans la configuration.
Les contrôles de ping/pong ne réveillent pas le Durable Object.

Workers et Durable Objects restent soumis aux quotas du compte. Sur la formule
Free, un dépassement coupe les opérations concernées. Ne pas activer Workers Paid
pour conserver ce comportement. Le fournisseur peut modifier ses offres.

Le secret du groupe limite l’accès au service, sans remplacer l’authentification
et le chiffrement de chaque relation côté client. Un membre ayant ce secret peut
saturer les quotas gratuits. Le serveur voit des métadonnées de présence et les IP
HTTPS. Ce service privé n’est pas présenté comme une infrastructure publique durcie.

[Limites Workers](https://developers.cloudflare.com/workers/platform/pricing/) ·
[Limites Durable Objects](https://developers.cloudflare.com/durable-objects/platform/pricing/)
