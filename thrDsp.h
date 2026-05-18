/*
 * asynchronousByzantineAgreementProtocols - threshold-dispersal plugin
 * Copyright (C) 2026 G. David Butler <gdb@dbSystems.com>
 *
 * This file is part of asynchronousByzantineAgreementProtocols
 *
 * asynchronousByzantineAgreementProtocols is free software: you can
 * redistribute it and/or modify it under the terms of the GNU Lesser General
 * Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * asynchronousByzantineAgreementProtocols is distributed in the hope
 * that it will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef THRDSP_H
#define THRDSP_H

/*
 * Threshold-dispersal plugin
 *
 * Split a payload into n pieces such that any k of them reassembles the
 * payload, with Merkle-tree authentication of every piece against a
 * single root hash. Two adapters ship:
 *
 *   thrDspRsec - Reed-Solomon erasure coding. Bandwidth-efficient
 *     (pieceSz ~= payloadLen / k). Pieces are linear combinations of
 *     payload bytes; t+1 pieces leak partial information about the
 *     payload (NOT confidential below threshold).
 *
 *   thrDspSss - Shamir Secret Sharing. Censorship-resistant
 *     (pieceSz = payloadLen). Pieces are polynomial evaluations; t
 *     pieces reveal nothing (information-theoretically confidential
 *     below threshold).
 *
 * The caller picks the adapter. Pieces are addressed by piece-index in
 * [0, n). Each adapter maps the index to whatever the underlying scheme
 * needs internally (RS shard position; SSS share point). Wire format
 * carries (i, piece, proof); the consumer does not see scheme-specific
 * identifiers.
 *
 * The Merkle root commits to piece integrity in transit only. It does
 * NOT commit to the decoded payload. A Byzantine encoder can ship n
 * Merkle-valid pieces that decode to garbage:
 *   RS: k shards that satisfy Merkle but are not a valid codeword.
 *   SSS: k shares from a polynomial whose value at the secret point is
 *        not the payload.
 * End-to-end payload integrity (e.g., a payload hash bundled with the
 * root in the consumer's envelope and checked after decode) is the
 * consumer's responsibility. This plugin provides piece-level integrity
 * only.
 *
 * Adapter-specific knobs (hash vtable, SSS secret point, SSS point map,
 * SSS entropy source) are exposed via per-adapter config structs passed
 * at Init. No allocation, no globals, no spawned threads, no callbacks
 * for delivery. Caller owns all memory; *Sz entries report sizes.
 */

struct thrDsp;
struct thrDspVt;

struct thrDspVt {
  /*
   * Maximum n this adapter supports.
   *   RS:  256 (GF(2^8) field size).
   *   SSS: 255 (one of 256 GF points is the secret).
   */
  unsigned int
  (*maxN)(
    const struct thrDsp *d
  );

  /*
   * Threshold: minimum pieces needed to decode.
   *   RS:  n - 2t
   *   SSS: t + 1
   * Returns 0 on invalid parameters.
   */
  unsigned int
  (*threshold)(
    unsigned int n
   ,unsigned int t
  );

  /*
   * Bytes per piece for a payload of payloadLen with (n, t).
   *   RS:  ceil(payloadLen / threshold(n, t))
   *   SSS: payloadLen
   * Returns 0 on invalid parameters.
   */
  unsigned int
  (*pieceSz)(
    const struct thrDsp *d
   ,unsigned int payloadLen
   ,unsigned int n
   ,unsigned int t
  );

  /* Bytes of the Merkle root hash (= hash vtable's 2^h). */
  unsigned int
  (*rootSz)(
    const struct thrDsp *d
  );

  /* Bytes of a per-piece Merkle proof for an n-piece tree. */
  unsigned int
  (*proofSz)(
    const struct thrDsp *d
   ,unsigned int n
  );

  /* Scratch sizes (caller-supplied work area per call). */
  unsigned int
  (*encWaSz)(
    const struct thrDsp *d
   ,unsigned int n
   ,unsigned int t
   ,unsigned int payloadLen
  );
  unsigned int
  (*vfWaSz)(
    const struct thrDsp *d
  );
  unsigned int
  (*decWaSz)(
    const struct thrDsp *d
   ,unsigned int n
   ,unsigned int t
   ,unsigned int payloadLen
  );
  unsigned int
  (*derivedRootWaSz)(
    const struct thrDsp *d
   ,unsigned int n
   ,unsigned int t
   ,unsigned int payloadLen
  );

  /*
   * Encode payload into n pieces, per-piece Merkle proofs, and root.
   *   pieces[i]: caller buffer, pieceSz(d,payloadLen,n,t) bytes
   *   proofs[i]: caller buffer, proofSz(d,n) bytes
   *   root:      caller buffer, rootSz(d) bytes
   *   work:      caller buffer, encWaSz(d,n,t,payloadLen) bytes
   * Returns 0 on success, -1 on error.
   */
  int
  (*encode)(
    const struct thrDsp *d
   ,const unsigned char *payload
   ,unsigned int payloadLen
   ,unsigned int n
   ,unsigned int t
   ,unsigned char *const *pieces
   ,unsigned char *const *proofs
   ,unsigned char *root
   ,unsigned char *work
  );

  /*
   * Verify a single piece against the root (streaming).
   * Receiver calls per arriving (i, piece, proof).
   * Returns 0 if the piece is bound to root by proof, -1 otherwise.
   * Comparison is NOT promised constant-time.
   */
  int
  (*verify)(
    const struct thrDsp *d
   ,unsigned int i
   ,unsigned int n
   ,unsigned int t
   ,unsigned int payloadLen
   ,const unsigned char *piece
   ,const unsigned char *proof
   ,const unsigned char *root
   ,unsigned char *work
  );

  /*
   * Decode from any threshold(n, t) pieces.
   *   indices[]: piece indices in [0, n), all distinct, length
   *              threshold(n, t). pieces[j] is the piece at indices[j].
   *   pieces[]:  threshold(n, t) pointers to piece bytes
   *   payload:   caller buffer, payloadLen bytes
   *   work:      caller buffer, decWaSz(d, n, t, payloadLen) bytes
   * Returns 0 on success, -1 on error.
   * Decode trusts its inputs; caller verifies pieces first.
   */
  int
  (*decode)(
    const struct thrDsp *d
   ,unsigned int n
   ,unsigned int t
   ,unsigned int payloadLen
   ,const unsigned char *indices
   ,const unsigned char *const *pieces
   ,unsigned char *payload
   ,unsigned char *work
  );

  /*
   * Re-derive Merkle root (and optionally per-piece proof / piece)
   * from any threshold(n, t) pieces by reconstructing the underlying
   * polynomial and re-evaluating at all n piece positions, then
   * building the Merkle tree.
   *
   * Supports protocols whose Byzantine-dealer detection requires a
   * polynomial-consistency check (e.g., CT04 §3.5 AVID-H Disperse
   * ECHO-handler predicate: "do these threshold pieces interpolate
   * to a polynomial whose Merkle root matches the dealer's
   * commitment?"). Caller compares the returned root against
   * whatever commitment its protocol holds.
   *
   * Root, proof, and piece are three projections of the same
   * polynomial reconstruction + Merkle tree build. Pass any subset
   * of (proof, piece) as non-null to request its output at index
   * idx; both share the idx argument and the underlying work. Pass
   * both 0 (idx then ignored) for root-only.
   *
   * If proof != 0, extract the per-piece Merkle proof for piece
   * index idx (in [0, n)) from the re-derived tree, written to the
   * caller buffer at proof (proofSz(d, n) bytes). Supports CT04
   * §3.5 AVID-H's READY broadcast (root, FP-bar_i, F-bar_i):
   * receivers run verify against the arriving (j, F-bar_j,
   * FP-bar_j).
   *
   * If piece != 0, extract the reconstructed piece at index idx
   * (in [0, n)) from the rebuilt polynomial, written to the caller
   * buffer at piece (pieceSz(d, payloadLen, n, t) bytes). Supports
   * CT04 §3.5 AVID-H servers that reach the interpolation gate
   * without having received the dealer's SEND (so they have no
   * cached F_self) and must broadcast READY using F-bar_self from
   * the polynomial.
   *
   *   indices[]: piece indices in [0, n), distinct, length
   *              threshold(n, t)
   *   pieces[]:  threshold(n, t) pointers to piece bytes
   *   root:      caller buffer, rootSz(d) bytes (re-derived root out)
   *   idx:       piece index in [0, n) when proof != 0 OR piece != 0;
   *              ignored if both are 0
   *   proof:     caller buffer, proofSz(d, n) bytes, or 0
   *   piece:     caller buffer, pieceSz(d, payloadLen, n, t) bytes,
   *              or 0
   *   work:      caller buffer, derivedRootWaSz(d, n, t, payloadLen)
   * Returns 0 on success, -1 on error.
   * Like decode, this trusts its inputs; caller verifies pieces first.
   */
  int
  (*derivedRoot)(
    const struct thrDsp *d
   ,unsigned int n
   ,unsigned int t
   ,unsigned int payloadLen
   ,const unsigned char *indices
   ,const unsigned char *const *pieces
   ,unsigned char *root
   ,unsigned int idx
   ,unsigned char *proof
   ,unsigned char *piece
   ,unsigned char *work
  );
};

/*
 * Transparent struct; fields are adapter-private. Caller owns storage;
 * adapter Init wires .vt and stashes its config in .opaque.
 */
struct thrDsp {
  const struct thrDspVt *vt;
  const void *opaque;
};

#endif /* THRDSP_H */
