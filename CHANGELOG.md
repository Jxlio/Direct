# Direct — historique des versions

## 0.7.0 — 21 octobre 2026

- Mise en relation automatique via un service privé Cloudflare Workers Free.
- Nouveau code ami : un seul échange transmet aussi la configuration du service.
- Reconnexion du service et nouvelles tentatives P2P sans échanger de codes de session.
- Descriptions de connexion chiffrées, challenge de session et routes opaques.
- Présence des amis et nom du correspondant dans la conversation.
- Aucun relais TURN activé : les réseaux bloquant le P2P peuvent rester incompatibles.

## 0.6.1 — 21 octobre 2026

- Navigation séparée entre amis et conversations, avec avatar et présence du correspondant.
- Bulles distinctes pour les messages envoyés et reçus, noms d’auteur et heures.
- Paramètres audio : choix du micro et de la sortie, volumes indépendants enregistrés localement.
- Tests du micro avec indicateur de niveau et son de test sur la sortie sélectionnée.
- Appel entrant, réponse, attente et état des micros visibles ; raccrochage des deux côtés.
- Test solo accessible depuis les paramètres : deux clients connectés automatiquement, messages chiffrés et audio simulé sans modifier les contacts personnels.

## 0.6.0 — 21 octobre 2026

- Panneau Versions et mises à jour dans l'application Windows.
- Vérification au lancement depuis les releases de Jxlio/Direct.
- Téléchargement d'un exécutable authentifié par signature ECDSA P-256.
- Installation différée à la fermeture, avec conservation de l'ancien exécutable.
- Historique local hors ligne et notes des dix dernières releases GitHub.

## 0.5.1 — 21 octobre 2026

- Nouveau nom : Direct — Messages et appels.
- Interface claire, panneaux translucides et pictogrammes Lucide.
- Connexion Internet et diagnostics réseau plus explicites.

## 0.5.0 — 20 octobre 2026

- Contacts persistants, protégés par le compte Windows.
- Ajout d'ami par code privé et découverte chiffrée sur le réseau local.

## 0.4.0 — 20 octobre 2026

- ICE/STUN/TURN et premier relais natif pour les essais.
- Messages fiables et vocal sur une connexion DTLS.

## 0.3.0 — 20 octobre 2026

- Réordonnancement audio et compensation des pertes courtes.

Ces versions sont des prototypes. La connexion entre deux box, les périphériques
audio réels et les interfaces Linux/macOS restent à valider ou à compléter.
