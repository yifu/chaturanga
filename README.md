# SDL3 Chess App

## Compilation du projet

### Prerequis

Le projet utilise:
- SDL3
- SDL3_ttf
- CMake
- un compilateur C compatible C11
- bibliotheque de threads POSIX (pthread, fournie par le systeme sur Linux/macOS)

Dependance optionnelle pour la decouverte reseau:
- Avahi client (Linux): active automatiquement le backend Avahi si present
- Bonjour / DNS-SD (macOS): utilise le SDK systeme, pas de paquet additionnel requis

### macOS

Installer les dependances:

```sh
brew update
brew install cmake sdl3 sdl3_ttf pkg-config
```

Compiler:

```sh
cmake -S . -B build
cmake --build build -j
```

Optionnel (decouverte reseau):
- aucun paquet supplementaire (Bonjour / DNS-SD est fourni par macOS)

### Linux (Debian/Ubuntu)

Installer les dependances:

```sh
sudo apt update
sudo apt install -y build-essential cmake pkg-config libsdl3-dev libsdl3-ttf-dev
```

Optionnel (decouverte reseau mDNS/Avahi), un paquet par ligne:

```sh
sudo apt install -y libavahi-client-dev
sudo apt install -y avahi-daemon
```

### Linux (Fedora)

Installer les dependances:

```sh
sudo dnf install -y gcc gcc-c++ make cmake pkgconf-pkg-config SDL3-devel SDL3_ttf-devel
```

Optionnel (decouverte reseau mDNS/Avahi), un paquet par ligne:

```sh
sudo dnf install -y avahi-compat-libdns_sd-devel
sudo dnf install -y avahi-devel
sudo dnf install -y avahi
```

### Linux (Arch)

Installer les dependances:

```sh
sudo pacman -Syu --needed base-devel cmake pkgconf sdl3 sdl3_ttf
```

Optionnel (decouverte reseau mDNS/Avahi), un paquet par ligne:

```sh
sudo pacman -S --needed avahi
```

Activer le service Avahi (optionnel):

```sh
sudo systemctl enable --now avahi-daemon
```

Compiler:

```sh
cmake -S . -B build
cmake --build build -j
```

## Run

```sh
./build/chess_app
```

## Tests automatises

Commande unique depuis la racine du depot:

```sh
cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure
```

Ce que couvre la suite actuelle:
- regles avancees du moteur: roque, en passant, promotion, echec/mat, regle des 50 coups
- transitions reseau de base: election de role et progression de session jusqu'au mode in-game
- echange de paquets reseau basique: HELLO, ACK et MOVE (integration locale via socketpair)
- synchronisation locale/distante via les memes primitives de validation de coups

Quality gate minimal (recommande):
- tout nouveau changement de regles ou flux reseau doit garder `ctest` vert
- aucun PR ne doit etre merge si `ctest` echoue

## LAN Discovery (current implementation)

The networking layer now has:
- deterministic host election: smaller IPv4 becomes server
- session state machine
- discovery API wired to the main loop
- TCP listener bound with port 0 (kernel picks a free ephemeral port)

If Avahi is available at build time, the project enables the Avahi backend flag.
The full mDNS browse/publish logic is the next step.

The TCP listener does not use a fixed application port. It binds with port 0,
then reads the actual allocated port with `getsockname()`. This is the port to
publish through mDNS.

Until then, discovery can be tested using environment simulation:

```sh
CHESS_REMOTE_IP=192.168.1.48 CHESS_REMOTE_PORT=53021 CHESS_REMOTE_UUID=8b4d717f-5d56-44dd-a07a-68de8e1617f7 ./build/chess_app
```

Expected behavior in logs:
- peer is discovered
- session moves to election/connecting states
- local role is selected from IP ordering
- TCP connection is established and HELLO handshake completes
