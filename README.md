# Murmur

**Messages et appels entre amis, avec une interface personnalisable en HTML/CSS/JS.**

Murmur est un projet de messagerie dont le cœur réseau et audio reste en C.
L’interface principale utilise HTML, CSS et JavaScript dans le moteur WebView2
de Windows. Aucun compte n’est nécessaire.

[Télécharger pour Windows](https://github.com/Jxlio/Direct/releases/latest) ·
[Historique des versions](CHANGELOG.md) · [Documentation](docs/README.md)

## Essayer Murmur

Téléchargez le fichier **Murmur-X.Y.Z-Windows-x64.zip** dans la dernière release,
extrayez le dossier entier, puis lancez **Murmur.exe**. Conservez les fichiers
du dossier ensemble. L’application est portable et destinée à Windows 10/11 64 bits.

Dans **Amis**, donnez un nom à votre ami, créez un code et envoyez-le en privé.
Votre ami ajoute ce code dans Murmur. Avec un code **Murmur 0.7** incluant le
service de mise en relation, les applications se retrouvent ensuite automatiquement
au lancement : aucun code de session à renvoyer.

Les deux personnes peuvent échanger des messages et lancer un appel.
Le correspondant doit répondre avant l’ouverture des périphériques audio.

Le service se configure une fois pour le groupe sur un compte Cloudflare gratuit.
Sa configuration est transmise aux autres personnes par leur code ami ; elles
n’ont ni compte Cloudflare ni serveur à gérer. [Fonctionnement et installation](docs/friends.md).

## Ce qui fonctionne

- Messages texte chiffrés et appels vocaux à deux.
- Choix du micro et de la sortie audio, volumes séparés et tests des périphériques.
- Amis enregistrés localement, présence et mise en relation automatique configurable.
- Connexion directe avec ICE/STUN et prise en charge d’un relais TURN configurable.
- Historique des versions et mises à jour signées depuis GitHub Releases.
- Interface principale en HTML/CSS/JS, dans une seule fenêtre avec WebView2.
- Partage vidéo intégré à l’appel, avec cible 1080p/60 et cadence réelle affichée.
- Thèmes, couleur d'accentuation au choix et feuille de style personnelle (`user.css`) ; indicateur de la personne qui parle pendant un appel.
- Serveurs avec salons et invitations (bêta), messages chiffrés de bout en bout ; limites dans [le protocole](docs/protocole.md#5-chiffrement-de-bout-en-bout).

## En développement

Murmur est encore un prototype. L’interface continue d’évoluer.
Les appels de groupe et les interfaces Linux et macOS ne sont
pas disponibles.

Certaines box nécessitent un relais TURN ; aucun relais public n’est activé
par défaut. La connexion sur tous les types de réseaux n’est donc pas garantie.
La mise en relation gratuite est soumise aux quotas Cloudflare : leur dépassement
peut suspendre temporairement la connexion automatique. Les anciens codes amis
restent utilisables sur le réseau local et le mode Internet manuel reste accessible.

Les messages restent en mémoire pendant la session. Le projet n’a pas fait
l’objet d’un audit de sécurité indépendant. Une connexion P2P directe révèle
votre adresse IP à votre interlocuteur : gardez les invitations privées et
partagez-les uniquement avec des personnes de confiance.

## Compiler

Prérequis : CMake 3.21 ou supérieur, Git, Python 3, un compilateur C17 et C++17.
Sous Windows, utilisez les outils de compilation Visual Studio avec le SDK Windows.
La première configuration récupère les dépendances à des révisions fixes, dont
le SDK WebView2. Le moteur WebView2 doit être installé sur le poste d’exécution.

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Avec Visual Studio, l’exécutable se trouve dans `build/Release/Murmur.exe`.
Le cœur du projet est portable ; les compilations Linux et macOS restent à valider.

Les détails du transport, de l’audio et de la publication sont dans la
[documentation technique](docs/README.md). Les bibliothèques et leurs licences
sont répertoriées dans les [crédits](docs/third-party.md).

L’interface se modifie dans [src/windows/ui](src/windows/ui). Voir
[les choix de rendu et la mesure mémoire](docs/interface.md).
