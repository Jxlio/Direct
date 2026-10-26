# Performance

La performance est le critère principal du projet. Ce document fixe le protocole
de mesure, les objectifs et les résultats de référence.

## Mesurer

```powershell
.\scripts\measure_performance.ps1 -Exe out\build-latest\Murmur.exe
```

Le script lance l'application, attend 8 s, puis mesure pendant 20 s la mémoire
privée, la mémoire résidente (working set) et le CPU de l'arbre de processus
complet (Murmur plus les processus WebView2 enfants). Le résultat est un JSON.
Il mesure l'application au repos, fenêtre principale ouverte, sans ami connecté.
Il ne mesure pas encore un appel, une conversation chargée ni la latence.

Attention : d'anciens binaires (`Direct.exe`, `murmur.exe`) peuvent traîner dans
`build\` et `out\`. Vérifier la date avant de mesurer. Un résultat à 1 seul
processus signifie que l'interface web n'a pas démarré.

## Référence du 25 octobre 2026 (Windows 11, `out\build-latest\Murmur.exe`)

| Étape | Processus | Mémoire privée | Mémoire résidente | CPU au repos |
| --- | --- | --- | --- | --- |
| Avant optimisation | 7 | ~169 Mo | ~360 Mo | ~0 % |
| Avec les options du moteur (ci-dessous) | 6 | ~158 Mo | ~325 Mo | ~0 % |
| Fenêtre réduite (page suspendue si aucun appel) | 6 | ~147 Mo | ~281 Mo | ~0 % |

Taille de l'exécutable : 1,7 Mo. La dispersion entre deux lancements est d'environ 5 Mo.

### Ce qui a été essayé

Variantes testées avec `-BrowserArgs` (sans recompiler), mémoire privée / résidente :

| Option | Résultat | Décision |
| --- | --- | --- |
| aucune | 169 / 365 Mo, 7 processus | référence |
| `NetworkServiceInProcess2` | 159 / 322 Mo, 6 processus | **retenue** |
| + `--disable-features=…` (SmartScreen, traduction, indices d'optimisation…) et `--disable-background-networking` | 150–156 / 320 Mo | **retenue** |
| `--in-process-gpu` | 146 / 350 Mo, mais CPU au repos plus élevé et un plantage du GPU emporterait l'application | refusée |
| `--disable-gpu` | 119 / 375 Mo, CPU au repos ~1 % | refusée : le partage d'écran 1080p60 a besoin du GPU |
| `--enable-low-end-device-mode` | 158 / 333 Mo | refusée : effets incertains sur la vidéo |
| `--disable-gpu-compositing` | 142 / 340 Mo | refusée : dessin logiciel |

Le moteur démarre aussi avec son profil dans `%LOCALAPPDATA%\Murmur\webview2` au lieu du
dossier du programme. Une fenêtre réduite abaisse la cible mémoire du moteur et, hors appel,
suspend la page ; elle est relancée à la restauration et la liste des messages est renvoyée.

### Où est la mémoire

| Processus | Privé |
| --- | --- |
| GPU | ~82 Mo |
| Navigateur | ~40 Mo |
| Rendu de la page | ~23 Mo |
| Services (réseau, stockage) | ~19 Mo |
| Murmur lui-même | ~6 Mo |

Le processus GPU est la plus grosse part et il est nécessaire au partage d'écran. L'objectif
de 250 Mo résidents n'est donc pas atteignable sans sacrifier cette fonction ; il est ramené à
**300 Mo**, et la mémoire privée (~158 Mo) est le chiffre le plus fidèle (la mémoire résidente
compte plusieurs fois les bibliothèques partagées entre processus).

## Objectifs

| Mesure | Objectif | État |
| --- | --- | --- |
| Mémoire résidente au repos | < 300 Mo | ~325 Mo : presque |
| Mémoire privée au repos | < 160 Mo | atteint (~158 Mo) |
| CPU au repos | ~ 0 % | atteint |
| Liste de 10 000 messages | < 100 ms, ≤ 200 nœuds | atteint (9 ms, 100 nœuds) |
| Appel de groupe | < 5 % d'un cœur | non mesuré, pas d'appel de groupe |

Chaque nouvelle fonction doit être mesurée avec ce script avant d'être fusionnée,
et le tableau de référence est mis à jour quand les résultats changent.

## Appel à deux fenêtres (salon vocal)

`scripts/e2e_voice.cjs` lance deux fenêtres Murmur dans un salon vocal, micro simulé, sur la boucle locale, et appelle
`scripts/measure_call.ps1`. Mesure du 26 octobre 2026 : **26,6 % d'un cœur pour les deux fenêtres ensemble** (1,66 % de la machine),
470,9 Mo de mémoire privée pour les deux fenêtres et leurs 14 processus (environ 235 Mo chacune, GPU et moteur compris).
Ce n'est pas une mesure sur un vrai réseau : la perte, la latence et la gigue réelles restent à mesurer à plusieurs.
