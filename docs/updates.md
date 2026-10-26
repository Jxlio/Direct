# Versions et mises à jour — Murmur 0.6

## Comportement utilisateur

- Cible : Windows x64, application portable dans un dossier où l'utilisateur
  dispose du droit d'écriture. Pas d'élévation administrateur.
- Le panneau **Paramètres → Versions** affiche l'historique local intégré, même hors ligne,
  et les notes des dix dernières releases publiques de `Jxlio/Direct`.
- Une vérification HTTPS au lancement cherche le manifeste de la dernière
  release stable. Le dépôt public ne nécessite aucun compte ou jeton côté client.
- Une version plus récente est téléchargée automatiquement en arrière-plan,
  sauf si une vérification manuelle est effectuée pendant un appel. Dans ce cas,
  un bouton propose le téléchargement. Il n'y a pas de vérification périodique.
- Une mise à jour authentifiée est appliquée à la fermeture normale, sans fermer
  une conversation à la place de l'utilisateur. Murmur ne redémarre pas seul.
- Avant 0.6, une installation manuelle du nouveau ZIP est nécessaire une fois.
- Les versions publiques sont disponibles sur [GitHub Releases](https://github.com/Jxlio/Direct/releases).

## Fichiers de release

Le tag doit être `vX.Y.Z` et correspondre à la version numérique de CMake et
à la ressource VERSIONINFO Windows. Exemples pour 0.6.0 :

- `Murmur-0.6.0-Windows-x64.zip` : première installation et dépannage.
- `Direct-win-x64.exe` : exécutable autonome de mise à jour.
- `Direct-win-x64.manifest` : plateforme, version, taille et SHA-256.
- `Direct-win-x64.sig` : signature ECDSA P-256/SHA-256 du manifeste exact.

Le client consulte `releases/latest/download/Direct-win-x64.manifest` puis
vérifie sa signature avec la clé publique de `src/update_key.h`. Le téléchargement
du binaire utilise ensuite le chemin du tag précis `releases/download/vX.Y.Z/`.
La signature est revérifiée par le processus d'installation, ainsi que le hash
et la taille de l'exécutable. Les versions égales ou antérieures sont ignorées.
Le plafond de téléchargement est 32 Mio. Les erreurs réseau, signatures absentes,
mauvaises plateformes et fichiers altérés conservent le programme actuel.

Le téléchargement n'est pas une signature Windows Authenticode : SmartScreen
peut toujours signaler un exécutable non signé. Les protections TLS Windows sont
conservées et les redirections HTTPS vers HTTP sont refusées. Aucun texte de note
de release n'est exécuté ou rendu en HTML.

## Stockage et installation

Les fichiers temporaires restent dans `%LOCALAPPDATA%/Murmur/updates` pour garder
la compatibilité avec le profil existant. Une copie du programme courant y sert
d'assistant natif, avec une copie du runtime Visual C++ portable s'il est présent
à côté du programme. Il attend la fin du PID appelant, vérifie les fichiers, écrit
un fichier voisin `.update-new`, conserve `.update-backup`, puis remplace
l'exécutable par renommage sur le même volume. Les profils et contacts ne sont
pas modifiés. En cas d'échec avant remplacement, le fichier initial reste intact.
La sauvegarde conserve une seule version. Il n'y a pas de restauration automatique
après un crash de la nouvelle version ; la sauvegarde permet un dépannage manuel.

Cette première génération remplace **uniquement Murmur.exe**. Les bibliothèques
du projet sont statiques et les notices de licences sont intégrées à l'exécutable.
Le runtime Visual C++ initial reste dans le dossier. Toute future version ajoutant
une DLL ou exigeant un nouveau runtime doit passer par un nouveau ZIP/installeur
ou une extension du protocole. Ne pas publier cette version comme une mise à jour
automatique si ses dépendances supplémentaires ne sont pas déjà présentes.
Les téléchargements HTTP sont bornés et temporisés, mais la résolution DNS et
les proxys système peuvent encore ajouter une latence. L'interface n'attend pas
le worker réseau à la fermeture ; un téléchargement inachevé n'est pas installé.

## Publication

Le workflow `.github/workflows/release.yml` compile et teste sur Windows x64,
prépare le ZIP, signe les assets et crée une **release en brouillon** sur un tag.
Le lancement manuel du workflow produit seulement le ZIP de test.
La publication finale du brouillon rend la version visible aux applications.

La clé privée a été créée localement dans le dossier `DirectPublisher` de
LOCALAPPDATA, avec un accès limité au compte courant. Elle n'est ni dans Git ni
dans les archives. Conserver une sauvegarde sûre : sa perte empêcherait la
signature de mises à jour reconnues par les clients actuels.

Pour la CI, ajouter **le contenu du fichier privé** au secret GitHub Actions
`DIRECT_UPDATE_SIGNING_KEY`. Ne jamais coller cette clé dans une issue, un chat,
le code ou les notes de release. La clé publique versionnée doit rester identique.
La protection du compte GitHub, des workflows et de ce secret fait partie de la
chaîne de confiance. La rotation de clé et un mécanisme anti-gel ne sont pas encore
implémentés ; une signature valide ne prouve pas que le programme est exempt de bugs.

Préparation locale, sans publication :

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
./scripts/package_windows.ps1
python scripts/sign_update.py --version 0.6.0 --exe build/Release/Murmur.exe --key "$env:LOCALAPPDATA/DirectPublisher/update-signing.pem" --output out/release
```

## Validation effectuée

Tests de manifeste signé, modification de chaque octet, mauvaise plateforme,
troncature, taille/hash, comparaisons numériques des versions et séparation des
clés de test/publication. Test entre processus : attente de fermeture, remplacement
correct, sauvegarde exacte de l'ancien fichier et refus d'un payload modifié sans
modifier le programme initial. Ces tests utilisent uniquement des fichiers
temporaires et une clé de test distincte ; aucun serveur public ni profil n'est
nécessaire. L’installation automatique depuis une release publique reste à valider. Le workflow
GitHub et la signature des fichiers publiés ont été vérifiés pour la version 0.6.0.
