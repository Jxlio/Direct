# Test solo à deux fenêtres

Depuis 0.6.1, **Paramètres → Test solo · ouvrir deux clients** ouvre Client A et Client B.
Leurs fenêtres sont placées côte à côte si l'écran est assez large, sinon en cascade.
La fenêtre principale qui lance le test est minimisée et reste disponible.

Les deux processus utilisent de vrais agents ICE, une authentification DTLS,
le transport fiable des messages et les codecs audio existants. Les descriptions
ICE sont échangées automatiquement par des pipes Windows anonymes, hérités
uniquement par le client enfant. Les secrets ne passent ni dans les arguments,
ni dans un fichier, ni dans le presse-papiers. Les agents sont liés à 127.0.0.1,
sans STUN ni TURN : aucun accès réseau externe n'est requis pour ce test.

Chaque fenêtre affiche l'autre client comme un ami temporaire connecté. Les
profils personnels et leurs fichiers de contacts ne sont pas ouverts. Les
contrôles de modification des contacts et les mises à jour sont désactivés
dans ces fenêtres de test, pour éviter de modifier l'installation ou le profil.

Deux messages sont envoyés à la connexion. Ensuite, écrivez dans l'une des deux
fenêtres : l'autre doit recevoir le texte dans la même discussion.
Cliquez **Tester l’appel** dans une fenêtre puis **Répondre** dans l’autre : le transport envoie de l'audio
synthétique à travers la connexion chiffrée. Les compteurs reçus et décodés doivent
progresser. Le bouton micro teste l'état muet et **Raccrocher** quitte l'appel.
Le microphone et les haut-parleurs ne sont jamais ouverts dans ce mode : vous
n'entendrez donc pas de son. Les vrais périphériques doivent être testés séparément.

Fermez les deux fenêtres de test à la fin. Le pair restant constate la déconnexion
après expiration du heartbeat/consentement réseau ; cela peut prendre quelques
secondes. L'historique de messages est seulement en mémoire, comme dans le mode normal.

Pour lancer depuis une commande : `Direct.exe --test-pair`. Le mode `--test-peer`
est réservé au processus enfant, avec ses pipes hérités. Le rendu reste déclenché
par événements au repos ; un rafraîchissement modéré est utilisé pendant la
négociation et l'appel simulé pour afficher les compteurs.

Ce test ne reproduit **pas** deux box ou deux accès Internet. Il sert à tester
la connexion locale chiffrée, les vues avec un ami connecté, les messages et les
transitions d'appel. Le partage d'écran reste indisponible.
