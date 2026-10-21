# Amis et connexion automatique

## Ajouter un ami une fois

Avec Direct 0.7, **Amis → Créer un code ami** produit une invitation privée.
Le créateur choisit un nom local pour le correspondant et transmet le code.
L’autre personne choisit un nom et utilise **Ajouter avec ce code**.
Les noms ne sont pas des identités certifiées.

Un code `direct2|…` contient le secret de la relation et la configuration du
service privé de mise en relation. Une fois enregistré, Direct ouvre une seule
connexion WebSocket HTTPS pour les contacts du groupe et négocie automatiquement
une session ICE/DTLS quand un ami est disponible. Il n’y a pas de serveur à lancer
sur les PC ni de code d’offre/réponse à échanger à chaque session.

Le service doit avoir été configuré une fois par l’organisateur du groupe.
Les autres amis n’ont pas besoin de compte Cloudflare. Une installation sans
service configuré peut importer un code Direct récent pour recevoir cette
configuration. Un seul groupe de mise en relation est pris en charge par profil.
Un code provenant d’un autre groupe ne remplace pas la configuration existante.

## Connexion et reconnexion

- Maximum 16 contacts enregistrés ; une seule conversation P2P simultanée.
- L’application tente de rejoindre un ami disponible. Les autres sessions attendent.
- Une perte du service déclenche une reconnexion avec attente progressive.
- Une tentative de session dispose de 35 secondes. Après échec, une nouvelle
  tentative P2P est espacée d’au moins une minute.
- **Annuler la connexion** suspend la reconnexion P2P jusqu’à son rétablissement
  depuis **Reconnexion automatique** ou au prochain lancement.
- Garder une seule instance normale de Direct ouverte par profil. Les fenêtres
  du test solo sont isolées et n’utilisent pas le service.
- Le vocal demande toujours une action locale et la réponse de l’autre personne.
- Le nom et les messages affichés correspondent à la session courante. En changeant
  d’ami, la conversation précédente en mémoire est effacée ; il n’y a pas encore
  d’archive locale des conversations.

## Sécurité et stockage

Le code contient deux secrets : celui de la relation, différent pour chaque ami,
et celui qui autorise l’accès au service du groupe. Le code est destiné à une
seule personne de confiance et ne doit jamais apparaître dans une issue publique.
Une personne possédant le secret de relation peut se faire passer pour cet ami.

Le secret de relation n’est jamais envoyé au serveur. Une route opaque en est
dérivée par HKDF-SHA256. Les descriptions de connexion sont protégées par
AES-256-GCM avec un domaine distinct du protocole LAN. Une réponse est liée au
challenge aléatoire courant ; le challenge d’offre est consommé à son acceptation.
Le transport des conversations utilise ensuite DTLS comme auparavant.

Le service observe les adresses IP des connexions WebSocket, leur présence,
les routes et les tailles des enveloppes, mais ne reçoit ni texte de conversation,
ni audio, ni secret de relation en clair. Il peut interrompre ou perturber la
mise en relation. Le protocole n’a pas subi d’audit indépendant.

Les contacts (`friends.dat`) et le profil du service (`service.dat`) sont protégés
par Windows DPAPI dans `%LOCALAPPDATA%/Murmur`. Ils sont liés au compte Windows.
Un fichier invalide n’est pas déchiffré en partie. Une écriture utilise un fichier
temporaire et un remplacement sur le même volume.

Oublier un ami supprime son secret local et sa route de présence. Pour révoquer
un code compromis, oublier la relation et en recréer une avec un nouveau secret.
Le secret du groupe peut être renouvelé côté Cloudflare et dans les profils du
groupe ; cela impose de redistribuer la configuration aux membres autorisés.

## Gratuité et limites réseau

Le service utilise Workers Free et des Durable Objects SQLite avec WebSocket
hibernation. Il n’a ni relais média ni stockage de messages. Ne pas passer le
compte à une formule payante si une coupure aux limites gratuites est souhaitée.
Les quotas s’appliquent à l’ensemble du compte Cloudflare.

La découverte du chemin direct utilise le STUN public Cloudflare. Aucun TURN
n’est activé automatiquement et aucune offre facturable n’est souscrite par le
projet. Les réseaux qui exigent un relais peuvent toujours empêcher la session.
La mise en relation automatique ne contourne pas cette limite du P2P.

Installation du service : [service/README.md](../service/README.md).
Tarifs et limites : [Workers](https://developers.cloudflare.com/workers/platform/pricing/),
[Durable Objects](https://developers.cloudflare.com/durable-objects/platform/pricing/),
[STUN/TURN](https://developers.cloudflare.com/realtime/turn/faq/).

## Compatibilité et validation

Les anciens codes `mfriend1…` sont conservés et permettent la découverte LAN.
Sans service configuré, le bouton de connexion utilise encore cette découverte,
avec broadcast IPv4 sur UDP 49381 et ports ICE dynamiques. Le mode Internet
manuel reste accessible comme outil avancé.

Les tests locaux couvrent les invitations, la séparation des domaines de
chiffrement, les enveloppes altérées, les limites, le stockage des contacts et
le contrôleur LAN. Le test explicite `online_tests --run` utilise le service
configuré pour connecter deux processus sans consulter ni modifier les contacts
personnels. Leurs candidats ICE restent sur loopback et aucun périphérique audio
n’est ouvert. Il valide le trajet de signalisation public, pas deux NAT Internet.
