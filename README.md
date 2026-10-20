# Direct

**Messages et appels entre amis, dans une application native légère.**

Direct est un projet de messagerie en C, conçu pour communiquer directement
avec ses proches. Pas d’Electron, de moteur web ou de compte à créer.

[Télécharger pour Windows](https://github.com/Jxlio/Direct/releases/latest) ·
[Historique des versions](CHANGELOG.md) · [Documentation](docs/README.md)

## Essayer Direct

Téléchargez le fichier **Direct-X.Y.Z-Windows-x64.zip** dans la dernière release,
extrayez le dossier entier, puis lancez **Direct.exe**. Conservez les fichiers
du dossier ensemble. L’application est portable et destinée à Windows 10/11 64 bits.

Pour rejoindre un ami sur Internet, ouvrez **Mes amis → Connexion Internet** :

1. Créez une invitation et envoyez-la à votre ami en privé.
2. Votre ami colle l’invitation et vous renvoie sa réponse.
3. Collez sa réponse et cliquez sur **Appliquer la réponse**.

Une fois connectés, vous pouvez échanger des messages et démarrer un appel.
Pour le vocal, les deux personnes doivent activer l’appel.

## Ce qui fonctionne

- Messages texte chiffrés et appels vocaux à deux.
- Liste d’amis enregistrée localement et recherche sur le réseau local.
- Connexion directe avec ICE/STUN et prise en charge d’un relais TURN configurable.
- Historique des versions et mises à jour signées depuis GitHub Releases.
- Interface Windows native, sans navigateur embarqué.

## En développement

Direct est encore un prototype. L’interface et les réglages audio doivent évoluer.
Le partage d’écran, les appels de groupe et les interfaces Linux et macOS ne sont
pas disponibles.

Entre deux réseaux, l’échange des codes reste manuel. Certaines box nécessitent
un relais TURN ; aucun relais public n’est fourni actuellement. La connexion
sur tous les types de réseaux n’est donc pas garantie.

Les messages restent en mémoire pendant la session. Le projet n’a pas fait
l’objet d’un audit de sécurité indépendant. Une connexion P2P directe révèle
votre adresse IP à votre interlocuteur : gardez les invitations privées et
partagez-les uniquement avec des personnes de confiance.

## Compiler

Prérequis : CMake 3.21 ou supérieur, Git, Python 3 et un compilateur C17.
Sous Windows, utilisez les outils de compilation Visual Studio avec le SDK Windows.
La première configuration récupère les dépendances à des révisions fixes.

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Avec Visual Studio, l’exécutable se trouve dans `build/Release/Direct.exe`.
Le cœur du projet est portable ; les compilations Linux et macOS restent à valider.

Les détails du transport, de l’audio et de la publication sont dans la
[documentation technique](docs/README.md). Les bibliothèques et leurs licences
sont répertoriées dans les [crédits](docs/third-party.md).
