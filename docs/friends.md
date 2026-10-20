# Amis persistants (0.5, Windows)

## Utilisation

1. Ouvrir **Mes amis**, donner un nom au proche et **Creer un code pour cet ami**.
   L'ajout est enregistre avant d'afficher le code.
2. Transmettre ce code en prive a cette personne uniquement. Elle saisit un nom
   pour vous, colle le code puis clique **Ajouter le code recu**. Pas de code retour.
3. A chaque session, selectionner cet ami et cliquer **Retrouver sur ce reseau**
   des deux cotes. Les applications doivent etre sur le meme reseau IPv4 avec
   les broadcasts autorises. Aucun serveur ni adresse IP a saisir.
4. Une fois connectes, utiliser les salons texte et rejoindre volontairement le vocal.

Le parcours existe dans l'executable Windows natif. La decouverte entre deux
PC physiques et le parcours visuel restent a valider. Les reseaux invites avec
isolation des clients, VPN ou pare-feu peuvent empecher la decouverte.
Ne pas desactiver le pare-feu ; une autorisation reseau prive pour Murmur peut
etre necessaire. Le port de decouverte est UDP 49381, en plus des ports ICE
dynamiques. Une seule recherche simultanee par PC et une seule session a deux.

## Conservation et securite

- Maximum 16 relations. Le nom est local, il n'est pas une identite certifiee.
- Chaque code contient un secret aleatoire de 256 bits distinct. C'est une
  invitation privee reutilisable : toute personne qui la recoit peut se faire
  passer pour l'ami. Ne pas publier le code ni le reutiliser pour plusieurs amis.
- Les roles de connexion sont determines a l'ajout : le createur propose,
  l'importateur repond. Les deux peuvent commencer la recherche dans n'importe
  quel ordre. Le code ne contient aucune adresse reseau de session a conserver.
- Stockage `%LOCALAPPDATA%\Murmur\friends.dat`, chiffre avec Windows DPAPI
  pour le compte utilisateur courant. Ecriture temporaire puis remplacement.
  Un fichier altere ou indechiffrable est conserve et bloque les modifications.
  Ce stockage n'est pas une sauvegarde portable entre comptes ou ordinateurs.
- La decouverte utilise AES-256-GCM de Mbed TLS et une cle derivee par HKDF-SHA256
  avec domaine dedie. Les descriptions ICE et leur cle de session ne circulent
  pas en clair sur le reseau local. Chaque paquet utilise un nonce aleatoire.
- Une tentative utilise un identifiant aleatoire de 256 bits ; une reponse d'une
  autre tentative est ignoree. Une offre ancienne authentique peut encore etre
  rejouee pendant une recherche et provoquer son echec ; il n'y a pas de journal
  persistant anti-rejeu ni de garantie contre le deni de service sur le LAN.
- Recherche bornee a 120 secondes, jusqu'a 8 datagrammes traites par seconde,
  un envoi par seconde, arret de la minuterie et fermeture de la socket une fois
  connecte ou annule. Aucun balayage continu a l'ouverture de l'application.
- **Oublier cet ami** supprime le secret local et empeche son ancien code de
  retrouver ce PC. Une relation compromise doit etre supprimee puis recreee
  avec un nouveau code. Aucun mecanisme de revocation distant n'existe encore.
- La session media/texte conserve les protections DTLS existantes. Le protocole
  d'appairage et l'application n'ont pas fait l'objet d'un audit de securite.

## Internet

Cette etape automatise uniquement le LAN. Le mode **Connexion avancee** conserve
ICE/STUN/TURN avec echange manuel pour les tests Internet. Les codes amis ne
configurent pas de service Internet et le relais TURN existant ne remplace pas
un service de rendez-vous.

L'etape suivante est un service commun disponible en permanence pour transporter
les descriptions de session chiffrees, avec expiration, limites d'abus et
authentification. Son hebergement se configure une fois pour le groupe ; les amis
ne doivent pas lancer un serveur a chaque appel. Aucun service n'est encore
deploye et aucune promesse de connexion automatique entre logements n'est faite.

## Verification

Les tests automatises couvrent le code ami, le mauvais secret, chaque octet d'un
paquet modifie, les limites, les enveloppes autour de vraies offres/reponses ICE
(chemin direct et relais local), l'aller-retour de stockage DPAPI, l'absence de
secret litteral dans le fichier et le rejet d'une sauvegarde alteree sans ecraser
l'etat en memoire. Un test supplementaire utilise le controleur de decouverte
reel entre deux processus sur deux ports UDP loopback : echange automatique
d'offres/reponses, connexion DTLS, messages aller-retour et micro ferme. Ce test
remplace uniquement le broadcast par une destination loopback connue ; il ne
touche pas au profil utilisateur. Ils n'ouvrent pas le micro, n'utilisent pas de service public
et ne valident pas encore le broadcast entre deux machines physiques.
