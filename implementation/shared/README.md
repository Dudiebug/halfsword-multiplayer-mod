# HSM Shared Protocol

This directory defines the **owned multiplayer contract** shared by the server and client bridge.

Rules:

- Stable network IDs, never process pointers.
- Server owns combat results, successful grabs, weapon ownership, death, dismemberment, and match state.
- High-rate input/pose lanes are unreliable/sequenced.
- Discrete state transitions are reliable/ordered.
- Every schema change increments the protocol revision.
- Generated Half Sword SDK types must not leak into the wire contract.

The initial C++ structs live in the server module for compile convenience. As the client bridge
matures, move the canonical schema to a code-generated or language-neutral contract.
