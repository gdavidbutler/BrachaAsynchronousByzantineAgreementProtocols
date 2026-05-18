/*
 * asynchronousByzantineAgreementProtocols - CT04 Disperse primitive
 * Copyright (C) 2026 G. David Butler <gdb@dbSystems.com>
 *
 * This file is part of asynchronousByzantineAgreementProtocols
 *
 * asynchronousByzantineAgreementProtocols is free software: you can redistribute it
 * and/or modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * asynchronousByzantineAgreementProtocols is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <string.h>
#include "ct04Dsp.h"

/* Bitmap helpers (same idiom as bracha87.c). */
#define BIT_SZ(n)     (((unsigned int)(n) + 7) >> 3)
#define BIT_TST(a, i) ((a)[(unsigned int)(i) >> 3] & (1 << ((unsigned int)(i) & 7)))
#define BIT_SET(a, i) ((a)[(unsigned int)(i) >> 3] |= (unsigned char)(1 << ((unsigned int)(i) & 7)))

#define D_N(d)        ((unsigned int)(d)->n + 1)

/*
 * Per-call sizes (plugin-derived).
 */
#define D_PIECESZ(d)  ((d)->plugin->vt->pieceSz((d)->plugin, (d)->payloadLen, D_N(d), (d)->t))
#define D_PROOFSZ(d)  ((d)->plugin->vt->proofSz((d)->plugin, D_N(d)))
#define D_ROOTSZ(d)   ((d)->plugin->vt->rootSz((d)->plugin))
#define D_K(d)        ((d)->plugin->vt->threshold(D_N(d), (d)->t))

/* Sticky "rootCmt already populated" bit kept inside d->flags. */
#define CT04_DSP_F_HAVEROOT 0x10

/*
 * Layout inside d->data[]:
 *
 *   ecFrom    [BIT_SZ(N)]                per-sender first-time ECHO dedup
 *   rdFrom    [BIT_SZ(N)]                per-sender first-time READY dedup
 *   adFilled  [BIT_SZ(N)]                A_D slot occupancy by pieceIdx
 *   pieceSelf [pieceSz]                  committed F_i (R1) / F-bar_i (R2a/R3a)
 *   proofSelf [proofSz]                  committed FP_i / FP-bar_i
 *   rootCmt   [rootSz]                   committed root D / h_r
 *   pendingPc [N * pieceSz]              dealer: all n SEND payloads
 *   pendingPr [N * proofSz]              dealer: all n SEND proofs
 *   adPieces  [N * pieceSz]              A_D piece store indexed by pieceIdx
 *   scratch   [interpScratchSz]          interpolation work area
 *
 * The pendingPc / pendingPr regions are allocated unconditionally so
 * the layout is uniform across dealer / non-dealer instances; for
 * non-dealer instances they are unused.
 *
 * scratch[] is carved internally for the decode + encode + Merkle
 * round-trip the interpolation gate triggers.  Pointer-array regions
 * inside scratch are aligned to sizeof (void *).
 */

#define D_BS(d)        BIT_SZ(D_N(d))

#define D_ECFROM(d)    ((d)->data + 0)
#define D_RDFROM(d)    (D_ECFROM(d) + D_BS(d))
#define D_ADFILLED(d)  (D_RDFROM(d) + D_BS(d))
#define D_PIECESELF(d) (D_ADFILLED(d) + D_BS(d))
#define D_PROOFSELF(d) (D_PIECESELF(d) + D_PIECESZ(d))
#define D_ROOTCMT(d)   (D_PROOFSELF(d) + D_PROOFSZ(d))
#define D_PENDINGPC(d) (D_ROOTCMT(d) + D_ROOTSZ(d))
#define D_PENDINGPR(d) (D_PENDINGPC(d) + (unsigned long)D_N(d) * D_PIECESZ(d))
#define D_ADPIECES(d)  (D_PENDINGPR(d) + (unsigned long)D_N(d) * D_PROOFSZ(d))
#define D_SCRATCH(d)   (D_ADPIECES(d) + (unsigned long)D_N(d) * D_PIECESZ(d))

/*
 * Interpolation scratch size (bytes), plugin-derived.
 *
 * The interpolation step calls plugin->derivedRoot with proof + piece
 * outputs at idx = self.  derivedRoot:
 *   1. Reconstructs the polynomial from any k validated pieces
 *   2. Re-evaluates at all n piece positions
 *   3. Builds the Merkle tree
 *   4. Writes the root, the per-piece proof at self, and the
 *      re-evaluated piece at self into caller buffers
 *
 * All three outputs derive from the same internal computation;
 * pieceSelf / proofSelf are installed from those outputs regardless
 * of whether SEND was previously received.  No dependency on a
 * cached F_self from R1.
 *
 *   k pointers (decode input)               k * sizeof (void *)
 * + alignment slack                         sizeof (void *) - 1
 * + indices (k)                             k
 * + derived root buffer                     rootSz
 * + derived proof buffer (idx=self)         proofSz
 * + derived piece buffer (idx=self)         pieceSz
 * + plugin scratch (derivedRootWaSz)        derivedRootWaSz
 */
static unsigned long
interpScratchSz(
  const struct thrDsp *plugin
 ,unsigned int n
 ,unsigned int t
 ,unsigned int payloadLen
){
  unsigned long sz;
  unsigned int k;
  unsigned int pieceSz;
  unsigned int proofSz;
  unsigned int rootSz;
  unsigned int derWa;

  k = plugin->vt->threshold(n, t);
  pieceSz = plugin->vt->pieceSz(plugin, payloadLen, n, t);
  proofSz = plugin->vt->proofSz(plugin, n);
  rootSz = plugin->vt->rootSz(plugin);
  derWa = plugin->vt->derivedRootWaSz(plugin, n, t, payloadLen);

  sz = (unsigned long)k * sizeof (void *);
  sz += sizeof (void *) - 1;
  sz += k;
  sz += rootSz;
  sz += proofSz;
  sz += pieceSz;
  sz += derWa;
  return (sz);
}

unsigned long
ct04DspSz(
  const struct thrDsp *plugin
 ,unsigned int n
 ,unsigned int t
 ,unsigned int payloadLen
){
  unsigned long sz;
  unsigned int N;
  unsigned int pieceSz;
  unsigned int proofSz;
  unsigned int rootSz;

  if (!plugin)
    return (0);
  N = n + 1;
  pieceSz = plugin->vt->pieceSz(plugin, payloadLen, N, t);
  proofSz = plugin->vt->proofSz(plugin, N);
  rootSz = plugin->vt->rootSz(plugin);

  sz = sizeof (struct ct04Disperse) - 1;
  sz += 3 * BIT_SZ(N);           /* ecFrom, rdFrom, adFilled */
  sz += pieceSz;                 /* pieceSelf */
  sz += proofSz;                 /* proofSelf */
  sz += rootSz;                  /* rootCmt */
  sz += (unsigned long)N * pieceSz;   /* pendingPc */
  sz += (unsigned long)N * proofSz;   /* pendingPr */
  sz += (unsigned long)N * pieceSz;   /* adPieces */
  sz += interpScratchSz(plugin, N, t, payloadLen);
  return (sz);
}

void
ct04DspInit(
  struct ct04Disperse *d
 ,const struct thrDsp *plugin
 ,unsigned char n
 ,unsigned char t
 ,unsigned char self
 ,unsigned int payloadLen
){
  unsigned long sz;

  if (!d || !plugin)
    return;
  sz = ct04DspSz(plugin, n, t, payloadLen);
  memset(d, 0, sz);
  d->plugin = plugin;
  d->payloadLen = payloadLen;
  d->n = n;
  d->t = t;
  d->self = self;
  /* flags already zeroed */
}

int
ct04DspOrigin(
  struct ct04Disperse *d
 ,const unsigned char *payload
 ,unsigned char *encWork
){
  unsigned int N;
  unsigned int pieceSz;
  unsigned int proofSz;
  unsigned int rootSz;
  unsigned int i;
  unsigned char *pc;
  unsigned char *pr;
  unsigned char *pieces[256];
  unsigned char *proofs[256];

  if (!d || !payload || !encWork)
    return (-1);

  N = D_N(d);
  pieceSz = D_PIECESZ(d);
  proofSz = D_PROOFSZ(d);
  rootSz = D_ROOTSZ(d);

  pc = D_PENDINGPC(d);
  pr = D_PENDINGPR(d);
  for (i = 0; i < N; ++i) {
    pieces[i] = pc + (unsigned long)i * pieceSz;
    proofs[i] = pr + (unsigned long)i * proofSz;
  }

  if (d->plugin->vt->encode(d->plugin, payload, d->payloadLen, N, d->t,
   pieces, proofs, D_ROOTCMT(d), encWork))
    return (-1);

  /* The dealer commits to its own (root, F_self, FP_self) immediately:
   * its pendingPc[self] is also the value it will ECHO when its own
   * SEND arrives via loopback (R1).  Stash for the loopback path. */
  memcpy(D_PIECESELF(d), pieces[d->self], pieceSz);
  memcpy(D_PROOFSELF(d), proofs[d->self], proofSz);
  (void)rootSz; /* rootCmt populated above via encode's root arg */
  d->flags |= CT04_DSP_F_ORIGIN | CT04_DSP_F_HAVEROOT;
  return (0);
}

/*
 * Run the interpolation step.  Pull any k validated pieces from
 * A_D and call plugin->derivedRoot with proof + piece outputs at
 * idx = self, reconstructing the polynomial, deriving the Merkle
 * root, and extracting both the per-piece proof and the
 * re-evaluated piece for this server's index.  Compare derived
 * root to committed; on match, install F-bar_self / FP-bar_self
 * into pieceSelf / proofSelf and return 1.
 *
 * No dependency on a cached F_self from R1: the polynomial's
 * F-bar_self is what we ship in READY, whether or not SEND was
 * previously received.  For an honest dealer F-bar_self equals the
 * original F_self by Merkle binding; for a Byzantine dealer whose
 * tree we just accepted via root match, the paper assures the
 * polynomial is consistent.
 */
static int
doInterpolate(
  struct ct04Disperse *d
){
  unsigned int N;
  unsigned int k;
  unsigned int pieceSz;
  unsigned int proofSz;
  unsigned int rootSz;
  unsigned int derWa;
  unsigned int i;
  unsigned int j;
  unsigned char *adPieces;
  unsigned char *adFilled;
  unsigned char *scratch;
  unsigned char **dpiecePtrs;
  unsigned char *indices;
  unsigned char *derRoot;
  unsigned char *derProof;
  unsigned char *derPiece;
  unsigned char *plugWork;

  N = D_N(d);
  k = D_K(d);
  if (!k || k > N)
    return (0);
  pieceSz = D_PIECESZ(d);
  proofSz = D_PROOFSZ(d);
  rootSz = D_ROOTSZ(d);
  derWa = d->plugin->vt->derivedRootWaSz(d->plugin, N, d->t, d->payloadLen);

  adPieces = D_ADPIECES(d);
  adFilled = D_ADFILLED(d);
  scratch = D_SCRATCH(d);

  /* Carve scratch.  Pointer array first, aligned to sizeof (void *). */
  dpiecePtrs = (unsigned char **)(((unsigned long)scratch
    + (sizeof (void *) - 1))
    & ~(unsigned long)(sizeof (void *) - 1));
  indices  = (unsigned char *)(dpiecePtrs + k);
  derRoot  = indices + k;
  derProof = derRoot + rootSz;
  derPiece = derProof + proofSz;
  plugWork = derPiece + pieceSz;
  (void)derWa;

  /* Walk adFilled bitmap; collect the first k validated pieces. */
  j = 0;
  for (i = 0; i < N && j < k; ++i) {
    if (BIT_TST(adFilled, i)) {
      indices[j] = (unsigned char)i;
      dpiecePtrs[j] = adPieces + (unsigned long)i * pieceSz;
      ++j;
    }
  }
  if (j < k)
    return (0);  /* not enough distinct validated pieces */

  if (d->plugin->vt->derivedRoot(d->plugin, N, d->t, d->payloadLen,
   indices, (const unsigned char *const *)dpiecePtrs,
   derRoot, d->self, derProof, derPiece, plugWork))
    return (0);

  if (memcmp(derRoot, D_ROOTCMT(d), rootSz) != 0)
    return (0);

  memcpy(D_PIECESELF(d), derPiece, pieceSz);
  memcpy(D_PROOFSELF(d), derProof, proofSz);
  return (1);
}

unsigned int
ct04DspInput(
  struct ct04Disperse *d
 ,unsigned char type
 ,unsigned char from
 ,unsigned char pieceIdx
 ,const unsigned char *root
 ,const unsigned char *proof
 ,const unsigned char *piece
 ,unsigned char *vfWork
 ,unsigned char *decWork
 ,struct ct04DspAct *out
){
  unsigned int N;
  unsigned int k;
  unsigned int pieceSz;
  unsigned int proofSz;
  unsigned int rootSz;
  unsigned int maxThresh;
  unsigned int nout;
  unsigned char ecAtThresh;
  unsigned char rdLtK;
  unsigned char rdEqK;
  unsigned char rdEqKpT;
  unsigned char decodeOk;
  unsigned char haveEchoed;
  unsigned char haveRdsent;
  unsigned char sendEcho;
  unsigned char sendReady;
  unsigned char outputAbort;
  unsigned char outputStored;
  unsigned char replayEcho;
  unsigned char replayReady;

  (void)decWork; /* interpolation scratch comes from instance per-allocation */

  if (!d || !root || !proof || !piece || !vfWork || !out)
    return (0);

  N = D_N(d);
  if (pieceIdx >= N || from >= N)
    return (0);

  /* 1. have-terminated guard (abort or stored already fired). */
  if (d->flags & CT04_DSP_F_TERMINATED)
    return (0);

  /* 2. First-time / kind dispatch. */
  switch (type) {
  case CT04_DSP_SEND:
    /* Per-tag SEND dedup: paper allows one dealer per tag.  Once
     * F_ECHOED is set, R1 has already fired - reject further SENDs.
     * The dealer's piece is for this server's index; reject
     * pieceIdx mismatches (Byzantine dealer trying to mis-route). */
    if (d->flags & CT04_DSP_F_ECHOED)
      return (0);
    if (pieceIdx != d->self)
      return (0);
    break;
  case CT04_DSP_ECHO:
    if (BIT_TST(D_ECFROM(d), from))
      return (0);
    break;
  case CT04_DSP_READY:
    if (BIT_TST(D_RDFROM(d), from))
      return (0);
    break;
  default:
    return (0);
  }

  pieceSz = D_PIECESZ(d);
  proofSz = D_PROOFSZ(d);
  rootSz = D_ROOTSZ(d);
  k = D_K(d);
  if (!k || k > N)
    return (0);

  /* 3. Piece-valid via plugin's per-piece Merkle check. */
  if (d->plugin->vt->verify(d->plugin, pieceIdx, N, d->t, d->payloadLen,
   piece, proof, root, vfWork))
    return (0);

  /* 4. Root commitment.  Single dealer per tag, so once we have a
   * root, subsequent arrivals must agree bit-for-bit.  Otherwise
   * commit this root. */
  if (d->flags & CT04_DSP_F_HAVEROOT) {
    if (memcmp(D_ROOTCMT(d), root, rootSz) != 0)
      return (0);
  } else {
    memcpy(D_ROOTCMT(d), root, rootSz);
    d->flags |= CT04_DSP_F_HAVEROOT;
  }

  /* 5. Per-kind A_D / counter updates. */
  switch (type) {
  case CT04_DSP_SEND:
    /* SEND doesn't touch A_D or counters; just stash payload + proof
     * so the subsequent (R1-triggered) ECHO carries them. */
    memcpy(D_PIECESELF(d), piece, pieceSz);
    memcpy(D_PROOFSELF(d), proof, proofSz);
    break;
  case CT04_DSP_ECHO:
    BIT_SET(D_ECFROM(d), from);
    if (d->eD < 0xFFFFu)
      d->eD++;
    if (!BIT_TST(D_ADFILLED(d), pieceIdx)) {
      memcpy(D_ADPIECES(d) + (unsigned long)pieceIdx * pieceSz, piece, pieceSz);
      BIT_SET(D_ADFILLED(d), pieceIdx);
    }
    break;
  case CT04_DSP_READY:
    BIT_SET(D_RDFROM(d), from);
    if (d->rD < 0xFFFFu)
      d->rD++;
    if (!BIT_TST(D_ADFILLED(d), pieceIdx)) {
      memcpy(D_ADPIECES(d) + (unsigned long)pieceIdx * pieceSz, piece, pieceSz);
      BIT_SET(D_ADFILLED(d), pieceIdx);
    }
    break;
  }

  /* 6. Threshold predicates (post-increment, edge-triggered).
   *    maxThresh = max(ceil((n+t+1)/2), k).  ceil((n+t+1)/2) is
   *    (n+t+2)/2 in C integer arithmetic. */
  maxThresh = (N + (unsigned int)d->t + 2) / 2;
  if (k > maxThresh)
    maxThresh = k;
  ecAtThresh = (d->eD == maxThresh);
  rdLtK = (d->rD < k);
  rdEqK = (d->rD == k);
  rdEqKpT = (d->rD == k + (unsigned int)d->t);

  /* 7. Interpolation gate: fire only on the threshold-crossing
   *    arrival.  Mutually exclusive across kinds and gates per the
   *    paper's else-if structure. */
  decodeOk = 0;
  if ((type == CT04_DSP_ECHO && ecAtThresh && rdLtK)
   || (type == CT04_DSP_READY && !ecAtThresh && rdEqK))
    decodeOk = (unsigned char)doInterpolate(d);

  /* 8. BPR sticky flags as boundary inputs. */
  haveEchoed = (d->flags & CT04_DSP_F_ECHOED) ? 1 : 0;
  haveRdsent = (d->flags & CT04_DSP_F_RDSENT) ? 1 : 0;

  sendEcho     = 0;
  sendReady    = 0;
  outputAbort  = 0;
  outputStored = 0;
  replayEcho   = 0;
  replayReady  = 0;

#include "ct04DspRules.c"

  /* Entry-point discriminator: BPR replay outputs are exhaustiveness-
   * only on the Input path and discarded.  Replays would be redundant
   * on a fresh-fire dispatch. */
  (void)replayEcho;
  (void)replayReady;

  /* 9. Apply outputs in API-contract order: echo, ready, stored / abort. */
  nout = 0;
  if (sendEcho) {
    d->flags |= CT04_DSP_F_ECHOED;
    out[nout].act = CT04_DSP_ACT_ECHO;
    out[nout].dest = 0;
    out[nout].pieceIdx = d->self;
    out[nout].root = D_ROOTCMT(d);
    out[nout].proof = D_PROOFSELF(d);
    out[nout].piece = D_PIECESELF(d);
    ++nout;
  }
  if (sendReady) {
    d->flags |= CT04_DSP_F_RDSENT;
    out[nout].act = CT04_DSP_ACT_READY;
    out[nout].dest = 0;
    out[nout].pieceIdx = d->self;
    out[nout].root = D_ROOTCMT(d);
    out[nout].proof = D_PROOFSELF(d);
    out[nout].piece = D_PIECESELF(d);
    ++nout;
  }
  if (outputStored) {
    d->flags |= CT04_DSP_F_TERMINATED;
    out[nout].act = CT04_DSP_ACT_STORED;
    out[nout].dest = 0;
    out[nout].pieceIdx = d->self;
    out[nout].root = D_ROOTCMT(d);
    out[nout].proof = D_PROOFSELF(d);
    out[nout].piece = D_PIECESELF(d);
    ++nout;
  }
  if (outputAbort) {
    d->flags |= CT04_DSP_F_TERMINATED;
    out[nout].act = CT04_DSP_ACT_ABORT;
    out[nout].dest = 0;
    out[nout].pieceIdx = 0;
    out[nout].root = 0;
    out[nout].proof = 0;
    out[nout].piece = 0;
    ++nout;
  }
  return (nout);
}

unsigned int
ct04DspBpr(
  struct ct04Disperse *d
 ,struct bracha87Pump *p
 ,struct ct04DspAct *out
){
  unsigned int N;
  unsigned int pieceSz;
  unsigned int proofSz;
  unsigned int nout;
  unsigned char type;
  unsigned char ecAtThresh;
  unsigned char rdLtK;
  unsigned char rdEqK;
  unsigned char rdEqKpT;
  unsigned char decodeOk;
  unsigned char haveEchoed;
  unsigned char haveRdsent;
  unsigned char sendEcho;
  unsigned char sendReady;
  unsigned char outputAbort;
  unsigned char outputStored;
  unsigned char replayEcho;
  unsigned char replayReady;

  if (!d || !p || !out)
    return (0);
  if (!(d->flags & (CT04_DSP_F_ORIGIN | CT04_DSP_F_ECHOED | CT04_DSP_F_RDSENT)))
    return (0);

  N = D_N(d);
  pieceSz = D_PIECESZ(d);
  proofSz = D_PROOFSZ(d);
  nout = 0;

  /*
   * SEND replay: one packet per call to the destination at the
   * current per-instance cursor (p->sendDest).  Advances modulo N
   * after emit.  Single-bit guard ("am origin"); DTC has no
   * ordering insight over independent per-destination payload
   * reads, so this stays C-side (analogue of bracha87Fig1Bpr's
   * INITIAL_ALL early-emit).  Per the anti-flood discipline
   * (README), continue replay regardless of who has echoed back -
   * the local "P_j echoed" optimisation strands honest peers that
   * missed the bootstrap in the n = 3t + 1 boundary regime (CT04
   * inventory Q4, symmetric with bracha87 pitfall 11).
   */
  if (d->flags & CT04_DSP_F_ORIGIN) {
    if (p->sendDest >= N)
      p->sendDest = 0;
    out[nout].act = CT04_DSP_ACT_SEND;
    out[nout].dest = (unsigned char)p->sendDest;
    out[nout].pieceIdx = (unsigned char)p->sendDest;
    out[nout].root = D_ROOTCMT(d);
    out[nout].proof = D_PENDINGPR(d) + (unsigned long)p->sendDest * proofSz;
    out[nout].piece = D_PENDINGPC(d) + (unsigned long)p->sendDest * pieceSz;
    ++nout;
    if (++p->sendDest >= N)
      p->sendDest = 0;
  }

  /*
   * ECHO / READY replays via the merged paper+BPR dispatch.  Enter
   * with inputs that cannot fire any paper rule given current
   * committed state (kind = ECHO, all counter predicates 0, decode 0).
   * Paper outputs come back all "no"; the chained BPR rules fire
   * from "send-X = no AND have-X-state = yes".
   */
  if (d->flags & (CT04_DSP_F_ECHOED | CT04_DSP_F_RDSENT)) {
    type        = CT04_DSP_ECHO;
    ecAtThresh  = 0;
    rdLtK       = 0;
    rdEqK       = 0;
    rdEqKpT     = 0;
    decodeOk    = 0;
    haveEchoed  = (d->flags & CT04_DSP_F_ECHOED) ? 1 : 0;
    haveRdsent  = (d->flags & CT04_DSP_F_RDSENT) ? 1 : 0;
    sendEcho    = 0;
    sendReady   = 0;
    outputAbort = 0;
    outputStored= 0;
    replayEcho  = 0;
    replayReady = 0;

#include "ct04DspRules.c"

    /* Paper outputs are guaranteed 0 by the inputs passed; discard. */
    (void)sendEcho;
    (void)sendReady;
    (void)outputAbort;
    (void)outputStored;

    if (replayEcho) {
      out[nout].act = CT04_DSP_ACT_ECHO;
      out[nout].dest = 0;
      out[nout].pieceIdx = d->self;
      out[nout].root = D_ROOTCMT(d);
      out[nout].proof = D_PROOFSELF(d);
      out[nout].piece = D_PIECESELF(d);
      ++nout;
    }
    if (replayReady) {
      out[nout].act = CT04_DSP_ACT_READY;
      out[nout].dest = 0;
      out[nout].pieceIdx = d->self;
      out[nout].root = D_ROOTCMT(d);
      out[nout].proof = D_PROOFSELF(d);
      out[nout].piece = D_PIECESELF(d);
      ++nout;
    }
  }
  return (nout);
}

unsigned int
ct04DspPumpStep(
  struct ct04Disperse *const *instances
 ,unsigned int count
 ,struct bracha87Pump *p
 ,struct ct04DspAct *out
 ,unsigned int outCap
){
  unsigned int idx;
  unsigned int n;
  unsigned int prevDest;
  unsigned int N;

  if (!instances || !p || !out || !count
   || outCap < CT04_DSP_PUMP_MAX_ACTS)
    return (0);

  for (;;) {
    if (p->pos >= count) {
      p->pos = 0;
      if (p->sweepActs == 0)
        return (0);
      p->sweepActs = 0;
    }
    idx = p->pos;
    if (!instances[idx]) {
      ++p->pos;
      p->sendDest = 0;
      continue;
    }
    prevDest = p->sendDest;
    n = ct04DspBpr(instances[idx], p, out);
    if (n) {
      p->sweepActs += n;
      /*
       * Advance pos when this instance has finished a full pass over
       * its SEND-destination ring (sendDest wrapped to 0 below the
       * starting point) OR when there is no per-destination SEND to
       * walk (instance not the dealer).  Non-dealer instances always
       * wrap pos immediately on emit since sendDest is unused.
       */
      N = D_N(instances[idx]);
      if (!(instances[idx]->flags & CT04_DSP_F_ORIGIN)
       || p->sendDest <= prevDest
       || p->sendDest >= N) {
        ++p->pos;
        p->sendDest = 0;
      }
      return (n);
    }
    /* idle instance — skip ahead. */
    ++p->pos;
    p->sendDest = 0;
  }
}

unsigned int
ct04DspCommittedCount(
  struct ct04Disperse *const *instances
 ,unsigned int count
){
  unsigned int cnt;
  unsigned int i;

  if (!instances)
    return (0);
  cnt = 0;
  for (i = 0; i < count; ++i)
    if (instances[i]
     && (instances[i]->flags & (CT04_DSP_F_ORIGIN
                              | CT04_DSP_F_ECHOED
                              | CT04_DSP_F_RDSENT)))
      ++cnt;
  return (cnt);
}

const unsigned char *
ct04DspStored(
  const struct ct04Disperse *d
){
  if (!d
   || !(d->flags & CT04_DSP_F_TERMINATED)
   || !(d->flags & CT04_DSP_F_RDSENT))
    return (0);
  return (D_PIECESELF(d));
}

const unsigned char *
ct04DspStoredProof(
  const struct ct04Disperse *d
){
  if (!d
   || !(d->flags & CT04_DSP_F_TERMINATED)
   || !(d->flags & CT04_DSP_F_RDSENT))
    return (0);
  return (D_PROOFSELF(d));
}

const unsigned char *
ct04DspRoot(
  const struct ct04Disperse *d
){
  if (!d || !(d->flags & CT04_DSP_F_HAVEROOT))
    return (0);
  return (D_ROOTCMT(d));
}
