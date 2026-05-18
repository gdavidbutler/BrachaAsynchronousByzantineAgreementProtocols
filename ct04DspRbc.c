/*
 * asynchronousByzantineAgreementProtocols - CT04 AVID-RBC outer wrap
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
#include "ct04DspRbc.h"

#define R_N(r) ((unsigned int)(r)->n + 1)

/*
 * Layout inside r->data[]:
 *   blockSelf [blockSz]
 *   hashList  [N * hashSz]
 *   ptrSlack  [sizeof (void *) - 1]      pointer-array alignment
 *   encOut    [N * sizeof (void *)]      output pointer array
 *   encBlocks [N * blockSz]              outer-encode block buffers
 *                                        (retained after STORED for
 *                                         the borrowed-pointer view;
 *                                         caller may discard via Init
 *                                         re-call if it wants the
 *                                         storage back)
 */

#define R_BLOCKSELF(r) ((r)->data + 0)
#define R_HASHLIST(r)  (R_BLOCKSELF(r) + (r)->blockSz)
#define R_PTRBASE(r)   (R_HASHLIST(r) + (unsigned long)R_N(r) * (r)->hashSz)

unsigned long
ct04DspRbcSz(
  unsigned int n
 ,unsigned int t
 ,unsigned int fileLen
 ,unsigned int hashSz
){
  unsigned long sz;
  unsigned int N;
  unsigned int kOuter;
  unsigned int blockSz;

  (void)t;
  N = n + 1;
  if (N <= t)
    return (0);
  kOuter = N - t;
  if (!kOuter)
    return (0);
  blockSz = (fileLen + kOuter - 1) / kOuter;

  sz = sizeof (struct ct04DspRbc) - 1;
  sz += blockSz;                                  /* blockSelf */
  sz += (unsigned long)N * hashSz;                /* hashList */
  sz += sizeof (void *) - 1;                      /* alignment slack */
  sz += (unsigned long)N * sizeof (void *);       /* encOut[] */
  sz += (unsigned long)N * blockSz;               /* encBlocks */
  return (sz);
}

void
ct04DspRbcInit(
  struct ct04DspRbc *r
 ,ct04DspRbcEncodeFn encode
 ,void *encodeCtx
 ,ct04DspRbcHashFn hash
 ,void *hashCtx
 ,unsigned char n
 ,unsigned char t
 ,unsigned char self
 ,unsigned int fileLen
 ,unsigned int hashSz
){
  unsigned long sz;
  unsigned int N;
  unsigned int kOuter;

  if (!r || !encode || !hash)
    return;
  sz = ct04DspRbcSz(n, t, fileLen, hashSz);
  memset(r, 0, sz);
  N = (unsigned int)n + 1;
  kOuter = N - t;
  r->encode = encode;
  r->encodeCtx = encodeCtx;
  r->hash = hash;
  r->hashCtx = hashCtx;
  r->fileLen = fileLen;
  r->blockSz = (fileLen + kOuter - 1) / kOuter;
  r->hashSz = hashSz;
  r->n = n;
  r->t = t;
  r->self = self;
}

unsigned int
ct04DspRbcDeliver(
  struct ct04DspRbc *r
 ,const unsigned char *file
 ,struct ct04DspRbcAct *out
){
  unsigned int N;
  unsigned int i;
  unsigned char **encOut;
  unsigned char *encBlocks;
  unsigned char *base;

  if (!r || !file || !out)
    return (0);
  if (r->flags & CT04_DSP_RBC_F_STORED)
    return (0);

  N = R_N(r);

  base = R_PTRBASE(r);
  encOut = (unsigned char **)(((unsigned long)base
    + (sizeof (void *) - 1))
    & ~(unsigned long)(sizeof (void *) - 1));
  encBlocks = (unsigned char *)(encOut + N);

  for (i = 0; i < N; ++i)
    encOut[i] = encBlocks + (unsigned long)i * r->blockSz;

  if (r->encode(r->encodeCtx, file, r->fileLen, N, r->t,
   encOut, r->blockSz))
    return (0);

  /* Compute hash list. */
  for (i = 0; i < N; ++i)
    r->hash(r->hashCtx, encOut[i], r->blockSz,
     R_HASHLIST(r) + (unsigned long)i * r->hashSz);

  /* Keep G_self; erase the other blocks (the encBlocks region for
   * j != self is no longer referenced by ct04DspRbcBlock and may
   * be overwritten on a future Init).  Storage discipline is the
   * caller's; we just hold blockSelf and the hash list. */
  memcpy(R_BLOCKSELF(r), encOut[r->self], r->blockSz);

  r->flags |= CT04_DSP_RBC_F_STORED;
  out->act = CT04_DSP_RBC_ACT_STORED;
  out->pieceIdx = r->self;
  out->block = R_BLOCKSELF(r);
  out->hashList = R_HASHLIST(r);
  return (1);
}

const unsigned char *
ct04DspRbcBlock(
  const struct ct04DspRbc *r
){
  if (!r || !(r->flags & CT04_DSP_RBC_F_STORED))
    return (0);
  return (R_BLOCKSELF(r));
}

const unsigned char *
ct04DspRbcHashList(
  const struct ct04DspRbc *r
){
  if (!r || !(r->flags & CT04_DSP_RBC_F_STORED))
    return (0);
  return (R_HASHLIST(r));
}
