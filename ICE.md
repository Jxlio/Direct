# Connexion ICE (0.4)

Depuis 0.5, ce panneau est accessible via **Mes amis > Connexion avancee**.
Le nouveau parcours local sans codes de session est decrit dans [FRIENDS.md](FRIENDS.md).

## Retrouver un proche

1. Ouvrir Connexion avancee. Le champ STUN contient stun.cloudflare.com, port 3478.
   Il est modifiable ; le vider limite la recherche aux adresses locales.
2. Le premier proche clique Creer une invitation, attend l'apparition de son code
   dans Votre code, et le copie en prive a l'autre personne.
3. L'autre colle ce code dans Code recu et clique Repondre. Il attend son propre
   code de reponse et le renvoie en prive au premier proche.
4. Le premier colle la reponse dans Code recu et clique Connecter. Le statut
   indique ensuite la connexion directe ou le passage par un relais configure.
   Renvoyer la reponse rapidement (environ deux minutes maximum cote repondant).
5. Les messages et le vocal utilisent ensuite cette connexion. Le vocal demande
   toujours une activation locale des deux cotes ; aucun micro n'est ouvert
   simplement en repondant a l'invitation.

Aucun serveur de signalisation n'est necessaire pour copier ces deux codes.
Ils contiennent les descriptions reseau et des adresses IP. L'invitation contient
aussi le secret de session : ne pas la publier. La reponse ne contient pas ce secret
et est authentifiee avec lui. Toute personne qui obtient l'invitation peut usurper
un proche ; aucune identite permanente n'est encore verifiee.
Utiliser la meme version sur les deux machines. Les anciennes invitations
murmur1 utilisent IP manuelle ; les nouveaux codes commencent par murmur2:.

## Serveurs et limites reelles

Le serveur STUN public Cloudflare est propose dans l'interface pour decouvrir
l'adresse visible depuis Internet. Il voit l'adresse source des requetes STUN.
Il ne transporte pas le texte ou le son. Sa disponibilite n'est pas controlee
par Murmur. Reference officielle :
[Cloudflare, Network connectivity](https://developers.cloudflare.com/realtime/sfu/get-started/connection-patterns/).

STUN ne garantit pas une connexion sur tous les types de box/NAT. Si un relais
TURN est disponible, renseigner son hote, son port UDP, son identifiant et son mot
de passe dans les deux applications avant de creer/repondre. ICE choisit un
chemin et peut utiliser les candidats du relais. Aucun relais public Murmur n'est
deploye et aucune cle de service payant n'est embarquee. Voir RELAY.md pour le
binaire de serveur fourni. Les reseaux bloquant l'UDP restent incompatibles :
ce mode n'implemente pas de TURN sur TCP/TLS 443.

Les connexions restent a deux. Pas de service de rendez-vous, de codes courts,
de compte, de liste de proches ou de reconnexion automatique dans cette version.
La resolution DNS des serveurs depend du systeme et peut retarder la fermeture
du worker ; elle ne bloque pas le fil d'interface.

## Couches et bornes

libjuice 1.7.4 assure ICE, STUN, TURN et consent freshness. Ses callbacks ne
font que mettre les datagrammes dans une file de 16 elements de 1600 octets maximum
et reveiller le worker. Ils ne manipulent ni TLS ni l'interface.

Mbed TLS assure DTLS 1.2 / ECDHE-PSK / ChaCha20-Poly1305 sur le chemin ICE,
avec anti-rejeu et cookies actifs. La cle est le secret aleatoire de l'invitation.
Le relais TURN ne recoit pas ce secret. Les codes de signalisation sont proteges
par HMAC-SHA256, avec une cle derivee via HKDF-SHA256 et le domaine
murmur/ice/signaling/v1. Ce format maison sert a l'encodage des descriptions ;
il n'est pas un remplacement du protocole DTLS. Il n'a pas ete audite.

Les enregistrements DTLS portent soit du KCP fiable pour le texte/les controles,
soit un paquet Opus temps reel, soit un heartbeat. L'audio ne subit donc pas
la retransmission des messages texte. KCP a un MTU de 1000, une fenetre d'envoi
de 16 segments, une fenetre de reception de 32, une file d'envoi applicative
limitee a 32 segments et un plafond d'ACK en attente de 256 entre deux flushes.
Les fragments recus et les tailles de message sont controles avant traitement.
Le controle de congestion standard de KCP est actif ; l'audio reste a debit cible fixe.

Les files sortantes fiables et audio entrantes sont limitees a 16 elements.
Un seul nouveau paquet audio sortant est conserve en plus de l'ecriture DTLS en
cours, pour privilegier le son recent. Ces bornes ne remplacent pas des mesures
de memoire totale et n'incluent pas tous les tampons internes des bibliotheques.

Le worker Windows est reveille par les callbacks ICE et les actions utilisateur.
Une session authentifiee inactive sans messages a acquitter peut attendre jusqu'a
un heartbeat (une seconde), au lieu de boucler toutes les 5 ms. Les negociations
et retransmissions ont leurs echeances courtes ; le vocal actif garde un tick
de 5 ms. La reception et l'annulation reveillent immediatement l'attente.
Une absence de trafic authentifie de plus de 15 secondes ferme la session.

## Verification effectuee

Dix tests Windows passent. Les nouveaux couvrent :

- invitation/reponse, refus d'un code altere et copie differee de la reponse ;
- connexion ICE directe sur loopback puis connexion obligeant un chemin TURN
  verifie, via un serveur STUN/TURN local avec identifiants de test ;
- texte fiable jusqu'a 8192 octets avec conservation du salon ;
- controles vocaux et paquet Opus chiffre, puis decodage effectif ;
- worker Windows et client dans un autre processus, codes transmis par pipes
  prives, texte dans les deux sens, refus du micro sans activation locale et arret.

Les tests ne contactent pas Cloudflare, n'ouvrent aucun peripherique audio et
ne prouvent pas le fonctionnement sur deux box Internet differentes. L'interface
complete, l'audio entre machines, la bande passante, la consommation et la bascule
sur perte de route restent a verifier. Linux/macOS restent a compiler et tester.
