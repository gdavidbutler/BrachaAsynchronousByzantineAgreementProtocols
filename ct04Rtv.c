/*
 * asynchronousByzantineAgreementProtocols - CT04 Retrieve primitive
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
#include "ct04Dsp.h"   /* for struct ct04Disperse field access */
#include "ct04Rtv.h"

#define BIT_SZ(n)     (((unsigned int)(n) + 7) >> 3)
#define BIT_TST(a, i) ((a)[(unsigned int)(i) >> 3] & (1 << ((unsigned int)(i) & 7)))
#define BIT_SET(a, i) ((a)[(unsigned int)(i) >> 3] |= (unsigned char)(1 << ((unsigned int)(i) & 7)))

/* ===================================================================
 * Server side - paper R5
 * =================================================================== */

unsigned int
ct04RtvServerRespond(
  const struct ct04Disperse *d
 ,struct ct04RtvServerResp *out
){
  if (!d || !out)
    return (0);

  out->act = 0;
  out->root = ct04DspRoot(d);
  out->proof = ct04DspStoredProof(d);
  out->piece = ct04DspStored(d);
  out->pieceIdx = 0;

  /* R5': Data[ID'] unbound -> no response (paper allows the server
   * to wait silently; the client's k-distinct collector is what
   * provides liveness, not any per-server reply). */
  if (!out->root || !out->proof || !out->piece)
    return (0);

  out->act = CT04_RTV_ACT_SEND_BLOCK;
  out->pieceIdx = d->self;
  return (1);
}

/* ===================================================================
 * Client side - paper R4 / R6 wait clause
 *
 * Layout inside data[]:
 *   expectedRoot     [rootSz]
 *   collectedBmp     [BIT_SZ(N)]      pieceIdx-collected dedup
 *   collectedPieces  [N * pieceSz]    indexed by pieceIdx
 *   payload          [payloadLen]     decoded F'
 *
 * The decode is invoked by the caller-provided decWork buffer on
 * the arrival that reaches the k-distinct threshold.  Per-call
 * scratch (indices, pointer array) is computed on the stack since
 * k is bounded by n <= 256.
 * =================================================================== */

#define C_N(c)        ((unsigned int)(c)->n + 1)
#define C_PIECESZ(c)  ((c)->plugin->vt->pieceSz((c)->plugin, (c)->payloadLen, C_N(c), (c)->t))
#define C_ROOTSZ(c)   ((c)->plugin->vt->rootSz((c)->plugin))
#define C_K(c)        ((c)->plugin->vt->threshold(C_N(c), (c)->t))

#define C_BS(c)            BIT_SZ(C_N(c))
#define C_EXPROOT(c)       ((c)->data + 0)
#define C_COLBMP(c)        (C_EXPROOT(c) + C_ROOTSZ(c))
#define C_COLPIECES(c)     (C_COLBMP(c) + C_BS(c))
#define C_PAYLOAD(c)       (C_COLPIECES(c) + (unsigned long)C_N(c) * C_PIECESZ(c))

unsigned long
ct04RtvClientSz(
  const struct thrDsp *plugin
 ,unsigned int n
 ,unsigned int t
 ,unsigned int payloadLen
){
  unsigned long sz;
  unsigned int N;

  if (!plugin)
    return (0);
  (void)t;
  N = n + 1;
  sz = sizeof (struct ct04RtvClient) - 1;
  sz += plugin->vt->rootSz(plugin);
  sz += BIT_SZ(N);
  sz += (unsigned long)N * plugin->vt->pieceSz(plugin, payloadLen, N, t);
  sz += payloadLen;
  return (sz);
}

void
ct04RtvClientInit(
  struct ct04RtvClient *c
 ,const struct thrDsp *plugin
 ,unsigned char n
 ,unsigned char t
 ,unsigned int payloadLen
){
  unsigned long sz;

  if (!c || !plugin)
    return;
  sz = ct04RtvClientSz(plugin, n, t, payloadLen);
  memset(c, 0, sz);
  c->plugin = plugin;
  c->payloadLen = payloadLen;
  c->n = n;
  c->t = t;
}

void
ct04RtvClientPinRoot(
  struct ct04RtvClient *c
 ,const unsigned char *root
){
  if (!c || !root)
    return;
  memcpy(C_EXPROOT(c), root, C_ROOTSZ(c));
  c->flags |= CT04_RTV_F_HAVEROOT;
}

unsigned int
ct04RtvClientInput(
  struct ct04RtvClient *c
 ,unsigned char from
 ,unsigned char pieceIdx
 ,const unsigned char *root
 ,const unsigned char *proof
 ,const unsigned char *piece
 ,unsigned char *vfWork
 ,unsigned char *decWork
 ,struct ct04RtvClientAct *out
){
  unsigned int N;
  unsigned int k;
  unsigned int pieceSz;
  unsigned int rootSz;
  unsigned int i;
  unsigned int j;
  unsigned char indices[256];
  const unsigned char *pcPtrs[256];
  unsigned char *colPieces;
  unsigned char *colBmp;

  (void)from;
  if (!c || !root || !proof || !piece || !vfWork || !decWork || !out)
    return (0);
  out->act = 0;
  out->payload = 0;
  if (c->flags & CT04_RTV_F_RETRIEVED)
    return (0);

  N = C_N(c);
  if (pieceIdx >= N)
    return (0);

  pieceSz = C_PIECESZ(c);
  rootSz = C_ROOTSZ(c);
  k = C_K(c);
  if (!k || k > N)
    return (0);

  /* Per-pieceIdx dedup. */
  colBmp = C_COLBMP(c);
  if (BIT_TST(colBmp, pieceIdx))
    return (0);

  /* Piece-valid via plugin. */
  if (c->plugin->vt->verify(c->plugin, pieceIdx, N, c->t, c->payloadLen,
   piece, proof, root, vfWork))
    return (0);

  /* Root commitment: first valid arrival commits (if not pinned);
   * subsequent must match. */
  if (c->flags & CT04_RTV_F_HAVEROOT) {
    if (memcmp(C_EXPROOT(c), root, rootSz) != 0)
      return (0);
  } else {
    memcpy(C_EXPROOT(c), root, rootSz);
    c->flags |= CT04_RTV_F_HAVEROOT;
  }

  colPieces = C_COLPIECES(c);
  memcpy(colPieces + (unsigned long)pieceIdx * pieceSz, piece, pieceSz);
  BIT_SET(colBmp, pieceIdx);
  if (c->collected < 0xFFFFu)
    ++c->collected;

  if (c->collected < k)
    return (0);

  /* Threshold reached: interpolate.  Walk colBmp to gather the
   * first k pieceIdx slots. */
  j = 0;
  for (i = 0; i < N && j < k; ++i) {
    if (BIT_TST(colBmp, i)) {
      indices[j] = (unsigned char)i;
      pcPtrs[j] = colPieces + (unsigned long)i * pieceSz;
      ++j;
    }
  }
  if (j < k)
    return (0);

  if (c->plugin->vt->decode(c->plugin, N, c->t, c->payloadLen,
   indices, pcPtrs, C_PAYLOAD(c), decWork))
    return (0);

  c->flags |= CT04_RTV_F_RETRIEVED;
  out->act = CT04_RTV_ACT_RETRIEVED;
  out->payload = C_PAYLOAD(c);
  return (1);
}

const unsigned char *
ct04RtvClientPayload(
  const struct ct04RtvClient *c
){
  if (!c || !(c->flags & CT04_RTV_F_RETRIEVED))
    return (0);
  return (C_PAYLOAD(c));
}
