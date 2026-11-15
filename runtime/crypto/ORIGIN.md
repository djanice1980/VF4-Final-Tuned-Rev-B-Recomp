# Monocypher 4.0.3

The two-PC game's security (docs/design.md section 50) uses Monocypher, a
small cryptography library in C, for X25519, the Elligator 2 map, BLAKE2b,
SHA-512 and XChaCha20-Poly1305. Its source is here as it was released, so
the builder compiles it with the game and nothing is downloaded for it.

- From: https://monocypher.org/download/monocypher-4.0.3.tar.gz
  (sha256 8cc9bc341a66249016db9bd70e9142d8d0aef9945973744b1ac05dbc55d8ee66).
  The same files as the project's git tag 4.0.3, but for the version line at
  the top of each ("Monocypher version 4.0.3" in place of `__git__`).
- Copied as they are, none changed: `src/monocypher.c`, `src/monocypher.h`,
  `src/optional/monocypher-ed25519.c` and `src/optional/monocypher-ed25519.h`
  (here side by side), and `LICENCE.md`.
- Licence: Monocypher is offered under the 2-clause BSD licence or CC0 1.0,
  at the user's choice (LICENCE.md). This project takes it under CC0 1.0.
  The BSD notice stays in each file as it came.

What uses it: runtime/host/secure.c (the join code's key exchange, CPace,
and the sealed datagrams), checked against the CPace draft's test vectors by
tests/secure.
