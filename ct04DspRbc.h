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

/*
 * Cachin and Tessaro 2004 §3.7 - AVID-RBC outer wrap.
 *
 * Layers a flat (k_outer = n-t, n) erasure-coded storage scheme on
 * top of the §3.6 RBC-from-AVID-H reliable broadcast.  Inner
 * Disperse uses ct04Dsp with k_inner = n - 2t; on inner STORED of
 * the full file F-bar, the outer wrap encodes F-bar with the
 * (n-t, n) code, hashes each of the n outer blocks G_j, keeps own
 * G_self plus the hash list [H(G_1), ..., H(G_n)], and outputs
 * STORED locally.  Storage blow-up at the outer layer is
 * asymptotically optimal n/(n-t) + o(1).
 *
 * Plugin-uncoupled.  The outer wrap takes caller-supplied encode
 * and hash callbacks rather than depending on a specific RS / hash
 * implementation - the inner Disperse already names its own thrDsp
 * plugin (which is moving to its codec repository), and the outer
 * doesn't need Merkle proofs (only flat hashes for the stored hash
 * list), so coupling it to thrDsp would over-fit.  The example
 * program wires rsecMk + rmd128 in; other callers can wire any
 * (n-t, n) erasure code + hash.
 *
 * Pure state machine: no I/O, no threads, no dynamic allocation.
 */

#ifndef CT04DSPRBC_H
#define CT04DSPRBC_H

#include "ct04Dsp.h"

/*
 * Outer-encode callback.  Encodes the inner-delivered file F-bar
 * into n outer blocks G_0..G_{n-1}, each of size blockSz =
 * ceil(fileLen / kOuter) where kOuter = n - t.  Caller supplies
 * blocks[i] as output buffers of blockSz bytes.
 *
 * Returns 0 on success, non-zero on failure.
 *
 *   ctx:     caller-provided closure
 *   file:    delivered F-bar
 *   fileLen: |F-bar|
 *   n, t:    process parameters; outer kOuter = n - t implicit
 *   blocks:  caller-provided output buffer array, blocks[0..n-1]
 *            each of blockSz bytes
 *   blockSz: bytes per block (= ceil(fileLen / (n - t)))
 */
typedef int (*ct04DspRbcEncodeFn)(
  void *                        /* ctx */
 ,const unsigned char *         /* file */
 ,unsigned int                  /* fileLen */
 ,unsigned int                  /* n */
 ,unsigned int                  /* t */
 ,unsigned char *const *        /* blocks */
 ,unsigned int                  /* blockSz */
);

/*
 * Hash callback.  Compute H(data, len) into out (hashSz bytes).
 *
 *   ctx:  caller-provided closure
 *   data: input bytes
 *   len:  input length
 *   out:  hashSz bytes
 */
typedef void (*ct04DspRbcHashFn)(
  void *                        /* ctx */
 ,const unsigned char *         /* data */
 ,unsigned int                  /* len */
 ,unsigned char *               /* out */
);

/* Output actions. */
#define CT04_DSP_RBC_ACT_STORED 1   /* outer storage settled */

struct ct04DspRbcAct {
  const unsigned char *block;        /* this server's G_self */
  const unsigned char *hashList;     /* [H(G_0), ..., H(G_{n-1})] */
  unsigned char act;
  unsigned char pieceIdx;            /* = self */
};

/*
 * Outer wrap state.  Caller allocates ct04DspRbcSz bytes and calls
 * Init before use.  Embeds nothing - the caller passes the inner
 * struct ct04Disperse * by reference (lifetime separate from outer).
 *
 * Variable tail layout:
 *   blockSelf [blockSz]                    G_self
 *   hashList  [n * hashSz]                 [H(G_0), ..., H(G_{n-1})]
 *   encScratch[n * blockSz + alignment]    outer-encode output area
 *                                          (released after stored)
 */
struct ct04DspRbc {
  ct04DspRbcEncodeFn encode;
  ct04DspRbcHashFn hash;
  void *encodeCtx;
  void *hashCtx;
  unsigned int fileLen;       /* |F-bar| expected from inner */
  unsigned int blockSz;       /* ceil(fileLen / (n - t)) */
  unsigned int hashSz;        /* H output bytes */
  unsigned char n;            /* actual = n + 1 */
  unsigned char t;
  unsigned char self;
  unsigned char flags;        /* CT04_DSP_RBC_F_* */
  unsigned char data[1];
};

#define CT04_DSP_RBC_F_STORED 0x01

unsigned long
ct04DspRbcSz(
  unsigned int  /* n: actual = n + 1 */
 ,unsigned int  /* t */
 ,unsigned int  /* fileLen */
 ,unsigned int  /* hashSz */
);

/*
 * Initialise an outer-wrap instance.
 *
 *   encode / encodeCtx: the (n-t, n) erasure-code encoder + closure
 *   hash / hashCtx:     the hash function + closure (called n times
 *                       per delivery, one per outer block)
 *   n, t, self:         process parameters (n encoded; actual = n + 1)
 *   fileLen:            |F-bar| the inner is expected to deliver
 *                       (the application sets this from the inner's
 *                       payloadLen)
 *   hashSz:             bytes per hash output (must match what hash()
 *                       writes)
 */
void
ct04DspRbcInit(
  struct ct04DspRbc *
 ,ct04DspRbcEncodeFn   /* encode */
 ,void *               /* encodeCtx */
 ,ct04DspRbcHashFn     /* hash */
 ,void *               /* hashCtx */
 ,unsigned char        /* n: actual = n + 1 */
 ,unsigned char        /* t */
 ,unsigned char        /* self */
 ,unsigned int         /* fileLen */
 ,unsigned int         /* hashSz */
);

/*
 * On inner-Disperse STORED for F-bar (the |F-bar|-byte delivered
 * file from the §3.6 RBC), perform the outer wrap:
 *   1. Encode F-bar with the (n-t, n) outer code into n blocks G_j
 *   2. Hash each G_j to produce [H(G_0), ..., H(G_{n-1})]
 *   3. Keep G_self and the hash list; erase the other G_j blocks
 *   4. Output STORED
 *
 * Caller passes the delivered F-bar bytes; the wrapper does the
 * outer encode + hash + selection in one shot (no streaming - inner
 * STORED is a single event, not a per-arrival update).
 *
 * On out[].act = CT04_DSP_RBC_ACT_STORED:
 *   .block    = borrowed pointer to G_self (blockSz bytes)
 *   .hashList = borrowed pointer to [H(G_0), ..., H(G_{n-1})]
 *               (n * hashSz bytes)
 *   .pieceIdx = self
 *
 * Returns 1 on STORED emitted; 0 on idempotent re-call (already
 * stored) or on encode failure.  Single STORED per instance.
 */
unsigned int
ct04DspRbcDeliver(
  struct ct04DspRbc *
 ,const unsigned char *  /* F-bar, fileLen bytes */
 ,struct ct04DspRbcAct *  /* out, room for 1 */
);

/* Borrowed pointer to G_self after STORED; 0 before. */
const unsigned char *
ct04DspRbcBlock(
  const struct ct04DspRbc *
);

/* Borrowed pointer to the n-entry hash list after STORED; 0 before. */
const unsigned char *
ct04DspRbcHashList(
  const struct ct04DspRbc *
);

#endif /* CT04DSPRBC_H */
