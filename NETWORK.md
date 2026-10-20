# Connexion privee : mode IP manuelle

Depuis la version 0.4, le bouton Connexion privee ouvre le nouveau parcours ICE.
Ce document decrit uniquement le bouton IP manuelle de ce panneau. Le nouveau
mode est documente dans ICE.md ; il n'a pas besoin des deux ports fixes ci-dessous.

## Essayer sous Windows

1. Lancer build/Release/murmur.exe et ouvrir **Connexion privee**.
2. L'hote renseigne son IPv4 accessible par l'autre appareil et un port TCP,
   puis clique sur **Heberger**. Sur le meme PC, utiliser 127.0.0.1 et lancer
   une seconde instance pour le proche. Sur un LAN, utiliser l'IPv4 locale de l'hote.
3. Copier l'invitation generee et la transmettre en prive au proche.
4. Le proche colle l'invitation dans sa fenetre de connexion et clique **Rejoindre**.
5. Attendre l'etat **Connecte**. Les messages circulent alors entre les deux
   applications et conservent leur salon (general, les-petits-riens, on-joue).
6. **Deconnecter** ferme la session. Une nouvelle session genere une nouvelle cle.
7. Pour le vocal, chaque personne clique sur **Rejoindre le vocal**. Le micro
   ne s'ouvre qu'apres les deux activations et l'authentification du canal audio.
   **Muet** coupe l'envoi du micro ; **Quitter le vocal** libere les peripheriques.
   Voir VOICE.md pour les limites et les ports.

Sur Internet, l'hote doit deja disposer d'un port TCP accessible. Il n'y a ni
STUN/TURN, ni ouverture automatique de port, ni serveur de rendez-vous dans cette
version. Ne pas desactiver le pare-feu pour la faire fonctionner.
Le vocal utilise en plus le port UDP suivant celui de l'invitation : par exemple,
TCP 48152 pour le texte et UDP 48153 pour le vocal. Une invitation sur le port
65535 permet le texte mais pas le vocal. Utiliser la meme version sur les deux pairs.

## Protocole et authentification

Mbed TLS 3.6.7, TLS 1.2, une seule suite autorisee :
TLS_ECDHE_PSK_WITH_CHACHA20_POLY1305_SHA256. La cle PSK est generee par le
generateur cryptographique de Mbed TLS initialise avec l'entropie du systeme.
Il n'y a aucun protocole de chiffrement maison ni mode texte en clair de secours.

L'invitation contient 32 octets secrets, une IPv4 et un port. Elle authentifie
la possession du secret, pas l'identite humaine du proche. Toute personne qui
obtient l'invitation peut tenter de rejoindre ; ne pas la publier. Aucun compte,
certificat permanent ou carnet d'identites n'est implemente. L'identite TLS PSK
fixe murmur-session-v1 sert a distinguer le protocole, pas les personnes.

Une session accepte un seul pair. Un echec d'authentification ferme la session :
recreer une invitation pour reessayer. Le pair peut donc provoquer un refus de
service en occupant la connexion ; ce prototype n'est pas un service public durci.
Le delai de connexion/authentification est de 15 secondes, hors attente initiale
de l'hote. Les sockets sont non bloquantes ; la fenetre reste utilisable.

Le cadre MRMR existant transporte des messages texte UTF-8, precedes d'un octet
de salon, dans le flux TLS. Les limites sont de 8192 octets UTF-8 sur le reseau,
2048 unites UTF-16 dans l'interface et 16 messages par file d'envoi/reception.
Les entrees UTF-8 invalides, avec NUL ou trop longues pour l'interface sont ignorees.
L'historique garde les 128 derniers messages, sans ecriture disque. Les files et
les tampons du transport sont effaces lors de leur liberation/reutilisation.

Le worker reseau a un seul proprietaire TLS et reveille l'interface par message.
Il attend les sockets par intervalles de 40 ms pour traiter l'annulation et les
files, et passe a 5 ms lorsque les deux pairs demandent le vocal. Ce polling reste
a remplacer par un reveil reseau/envoi unifie pour limiter
les reveils CPU. L'interface n'a pas de boucle de rendu permanente.

## Ce qui a ete verifie

Tests automatiques Windows : generation/lecture d'invitation, connexion TLS sur
loopback, echange dans les deux sens avec conservation du salon, refus d'une
mauvaise cle, limites de taille et file d'envoi occupee. Le port de test est 49183.
Un test supplementaire lance un client dans un processus distinct et verifie
l'echange via le worker Windows utilise par l'interface, puis le redemarrage et
l'annulation d'une session en attente. Il utilise le port 49184.
Les tests reseau ouvrent temporairement une ecoute IPv4 sur toutes les interfaces.
Ce sont des tests fonctionnels ; ils ne remplacent pas une revue de securite.

Compilation Windows et ouverture du panneau de connexion verifiees. Le parcours
complet entre deux interfaces, deux machines, Internet, Linux et macOS reste a
valider. Ne pas presenter le test loopback comme un test Internet.

## Limites restantes

Pas de confirmation de remise : « envoi demande » signifie mise en file locale.
Les messages en attente peuvent etre perdus a la deconnexion. Pas de stockage
hors ligne, de retransmission applicative, de reconnexion automatique ni de groupe.
Le vocal est limite a deux avec peripheriques par defaut ; voir VOICE.md.
Pas encore de partage d'ecran. L'interface des autres plateformes reste
a construire. Le coeur reseau est en C portable, sans validation de compilation
sur Linux/macOS pour l'instant. Le binaire non Windows reste la demonstration
console du format de messages, et n'expose pas encore les sessions reseau.
