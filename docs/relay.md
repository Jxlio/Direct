# Relais STUN/TURN a heberger plus tard

Aucun serveur Internet n'a ete cree ou deploye. Le projet fournit un executable
murmur-relay (Windows : build/Release/murmur-relay.exe), base sur le serveur
STUN/TURN de libjuice. Il est facultatif pour les connexions directes possibles.
La connexion du client n'utilise pas de relais implicite ou de compte payant.

## Configuration

Le serveur lit uniquement les variables d'environnement suivantes. Il ne lit
pas automatiquement un fichier .env et n'ecrit pas de secret sur disque.

| Variable | Valeur / defaut |
| --- | --- |
| MURMUR_RELAY_BIND | 127.0.0.1, donc machine locale uniquement |
| MURMUR_RELAY_EXTERNAL | Adresse IP annoncee aux clients ; requise avec bind 0.0.0.0 ou :: |
| MURMUR_RELAY_PORT | 3478 / UDP |
| MURMUR_RELAY_MIN_PORT | 50000 / UDP |
| MURMUR_RELAY_MAX_PORT | 50099 / UDP |
| MURMUR_RELAY_USERNAME | Identifiant obligatoire, maximum 127 octets |
| MURMUR_RELAY_PASSWORD | Secret obligatoire, 32 a 127 caracteres/octets UTF-8 |

Exemple de preparation locale dans PowerShell, sans afficher le secret :

```powershell
$env:MURMUR_RELAY_USERNAME = 'murmur'
$env:MURMUR_RELAY_PASSWORD = (& .\build\Release\murmur-relay.exe --new-secret)
.\build\Release\murmur-relay.exe --check
.\build\Release\murmur-relay.exe
```

--new-secret genere 32 octets aleatoires sous forme de 64 caracteres hexadecimaux.
--check valide la syntaxe sans ouvrir de socket ; il ne verifie pas l'accessibilite
reseau. Le serveur reste au premier plan jusqu'a Ctrl+C. Aucun service Windows,
daemon systeme, regle de pare-feu ou port de box n'est configure automatiquement.
L'identifiant et le secret du relais doivent etre transmis en prive aux personnes
autorisees et renseignes dans les champs TURN du client. Ils sont distincts de
l'invitation de session. Les variables d'environnement peuvent etre lues par des
processus ayant les droits suffisants sur la machine.

## Hebergement Internet futur

Il faudra une machine ayant une adresse publique accessible, ou des redirections
UDP appropriees. L'hote devra choisir l'adresse d'ecoute et l'adresse externe,
et rendre accessibles le port UDP principal et la plage de ports relais. La plage
peut etre reduite selon le nombre de sessions. Aucun hebergement n'a ete choisi.

Ce binaire limite les allocations a 8 et les pairs du serveur a 16, avec un quota
de 8 allocations pour l'identifiant fourni. Il demande une authentification TURN
et n'active pas de journaux libjuice contenant les adresses. Il s'agit d'un premier
relais prive, pas d'un service public durci : supervision, rotation des secrets,
quotas de bande passante et protection contre les abus restent a preparer avant
une exposition Internet. Les identifiants restent valides tant que ce processus
fonctionne ; les changer et redemarrer le serveur permet de les remplacer.

Le relais transporte des datagrammes DTLS deja chiffres par les clients et ne
recoit pas la cle des conversations. Il observe les adresses et les volumes de
trafic. Il ne remplace pas un service de signalisation : les codes invitation/reponse
restent a copier entre proches. Il ne fournit pas de TURN sur TCP/TLS.

Le binaire Windows compile et la validation de configuration sont testes ; le
backend serveur est aussi utilise par les tests de relais sur loopback. Le
deploiement Internet et la compilation Linux/macOS de cet executable restent
a verifier. Conserver le dossier licenses/ et third-party.md avec les binaires.
