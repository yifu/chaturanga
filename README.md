# SDL3 Chess App

## Build

```sh
cmake -S . -B build
cmake --build build -j
```

## Run

```sh
./build/chess_app
```

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
CHESS_REMOTE_IP=192.168.1.48 CHESS_REMOTE_UUID=8b4d717f-5d56-44dd-a07a-68de8e1617f7 ./build/chess_app
```

Expected behavior in logs:
- peer is discovered
- session moves to election/connecting states
- local role is selected from IP ordering
