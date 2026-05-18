/*
 * asynchronousByzantineAgreementProtocols - Example CT04 Disperse program
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

/*
 * ct04Dsp.c - Standalone demonstration of Cachin/Tessaro 2004
 * AVID-H Disperse + Retrieve (Sections 3.3 / 3.5 / 3.6).
 *
 * Disperse n pieces of a file F to n servers such that any k of
 * them suffice to retrieve F, with Merkle commitment binding each
 * piece to a single dealer-chosen root.  Reliable broadcast on the
 * commitment is achieved by the protocol itself (no Bracha layer
 * underneath the dispersal - that is what §3.5 AVID-H buys).
 *
 * Two plugin choices ship: thrDspRsec (Reed-Solomon shards,
 * bandwidth-efficient pieceSz ~= |F|/k) and thrDspSss (Shamir
 * shares, pieceSz = |F|, information-theoretically confidential
 * below threshold).  Select with -p {rsec, sss}.
 *
 * Scope: synchronous in-memory delivery, no message loss.
 * Exercises the Disperse rules of CT04 Fig. 1 plus the Retrieve
 * rules of CT04 Fig. 2.  Does NOT exercise BPR replay under loss
 * (that is what the test_ct04Dsp suite covers, and the SEND-loss
 * resilience path is documented as a known limitation in README).
 *
 * Usage:
 *   ./example_ct04Dsp [-p {rsec,sss}] [-n N] [-t T] [-L len]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ct04Dsp.h"
#include "ct04Rtv.h"
#include "thrDsp.h"
#include "thrDspRsec.h"
#include "thrDspSss.h"
#include "rsecMk.h"
#include "sssMk.h"
#include "rmd128.h"

#define MAX_PEERS 64
#define MAX_TICKS 1024

/*------------------------------------------------------------------------*/

static void *
rmd128Allocate(
  void
){
  return (malloc(rmd128tsize()));
}

/* SSS deterministic share-point map. */
static unsigned char
sharePt(
  unsigned int i
){
  return ((unsigned char)(i + 1));
}

static unsigned long SssSeed = 0xC0DECAFEu;
static void
detRandBytes(
  void *ctx
 ,unsigned char *buf
 ,unsigned int len
){
  unsigned int i;
  (void)ctx;
  for (i = 0; i < len; ++i) {
    SssSeed = SssSeed * 1103515245UL + 12345UL;
    buf[i] = (unsigned char)(SssSeed >> 16);
  }
}

/*------------------------------------------------------------------------*/
/*  Tiny in-memory wire                                                   */
/*------------------------------------------------------------------------*/

struct wireMsg {
  unsigned int rootOff;
  unsigned int proofOff;
  unsigned int pieceOff;
  unsigned char type;
  unsigned char from;
  unsigned char to;
  unsigned char pieceIdx;
};

#define MAX_MSGS 4096
static struct wireMsg Q[MAX_MSGS];
static unsigned int Qh;
static unsigned int Qt;

static unsigned char Blob[1 << 20];
static unsigned int  BlobLen;

static unsigned int
push(
  const unsigned char *b
 ,unsigned int len
){
  unsigned int off;
  off = BlobLen;
  if (BlobLen + len > sizeof (Blob)) {
    fprintf(stderr, "blob overflow\n");
    exit(1);
  }
  memcpy(Blob + off, b, len);
  BlobLen += len;
  return (off);
}

static void
emit(
  unsigned char from
 ,unsigned int n
 ,const struct ct04DspAct *a
 ,unsigned int rootSz
 ,unsigned int proofSz
 ,unsigned int pieceSz
){
  unsigned int rOff;
  unsigned int pfOff;
  unsigned int pcOff;
  unsigned int firstTo;
  unsigned int lastTo;
  unsigned int to;
  unsigned char type;

  if (a->act == CT04_DSP_ACT_SEND) {
    type = CT04_DSP_SEND;
    firstTo = a->dest;
    lastTo  = a->dest;
  } else if (a->act == CT04_DSP_ACT_ECHO) {
    type = CT04_DSP_ECHO;
    firstTo = 0;
    lastTo  = n - 1;
  } else if (a->act == CT04_DSP_ACT_READY) {
    type = CT04_DSP_READY;
    firstTo = 0;
    lastTo  = n - 1;
  } else {
    return;
  }

  rOff  = a->root  ? push(a->root,  rootSz)  : 0;
  pfOff = a->proof ? push(a->proof, proofSz) : 0;
  pcOff = a->piece ? push(a->piece, pieceSz) : 0;
  for (to = firstTo; to <= lastTo; ++to) {
    if (Qt >= MAX_MSGS) {
      fprintf(stderr, "queue overflow\n");
      exit(1);
    }
    Q[Qt].rootOff  = rOff;
    Q[Qt].proofOff = pfOff;
    Q[Qt].pieceOff = pcOff;
    Q[Qt].type     = type;
    Q[Qt].from     = from;
    Q[Qt].to       = (unsigned char)to;
    Q[Qt].pieceIdx = a->pieceIdx;
    ++Qt;
  }
}

/*------------------------------------------------------------------------*/

int
main(
  int argc
 ,char **argv
){
  const char *plugName;
  unsigned int n;
  unsigned int t;
  unsigned int payloadLen;
  unsigned int i;
  unsigned int tick;
  unsigned int storedCnt;
  unsigned int pieceSz;
  unsigned int proofSz;
  unsigned int rootSz;
  unsigned int totalWire;
  unsigned char *payload;
  unsigned char *encWork;
  unsigned char *vfWork;
  unsigned char *decWork;
  struct ct04Disperse *inst[MAX_PEERS];
  struct bracha87Pump pumps[MAX_PEERS];
  unsigned long sz;
  rsecMkHsh_t Hrsec;
  sssMkHsh_t  Hsss;
  struct thrDspRsecCfg cfgR;
  struct thrDspSssCfg  cfgS;
  struct thrDsp dRsec;
  struct thrDsp dSss;
  struct thrDsp *plug;
  int ai;
  struct ct04DspAct outActs[CT04_DSP_MAX_ACTS > CT04_DSP_PUMP_MAX_ACTS
                            ? CT04_DSP_MAX_ACTS : CT04_DSP_PUMP_MAX_ACTS];

  /* Defaults. */
  plugName   = "rsec";
  n          = 4;
  t          = 1;
  payloadLen = 96;

  for (ai = 1; ai < argc; ++ai) {
    if (!strcmp(argv[ai], "-p") && ai + 1 < argc)
      plugName = argv[++ai];
    else if (!strcmp(argv[ai], "-n") && ai + 1 < argc)
      n = (unsigned int)atoi(argv[++ai]);
    else if (!strcmp(argv[ai], "-t") && ai + 1 < argc)
      t = (unsigned int)atoi(argv[++ai]);
    else if (!strcmp(argv[ai], "-L") && ai + 1 < argc)
      payloadLen = (unsigned int)atoi(argv[++ai]);
    else {
      fprintf(stderr,
       "usage: %s [-p {rsec,sss}] [-n N] [-t T] [-L payloadLen]\n",
       argv[0]);
      return (1);
    }
  }
  if (n < 4 || n > MAX_PEERS) {
    fprintf(stderr, "n=%u out of range [4, %u]\n", n, MAX_PEERS);
    return (1);
  }
  if (n <= 3 * t) {
    fprintf(stderr, "need n > 3t (got n=%u t=%u)\n", n, t);
    return (1);
  }

  Hrsec.a = rmd128Allocate;
  Hrsec.i = (void(*)(void *))rmd128init;
  Hrsec.u = (void(*)(void *, const unsigned char *, unsigned int))rmd128update;
  Hrsec.f = (void(*)(void *, unsigned char *))rmd128final;
  Hrsec.d = free;
  Hrsec.h = 4;

  Hsss.a = rmd128Allocate;
  Hsss.i = (void(*)(void *))rmd128init;
  Hsss.u = (void(*)(void *, const unsigned char *, unsigned int))rmd128update;
  Hsss.f = (void(*)(void *, unsigned char *))rmd128final;
  Hsss.d = free;
  Hsss.h = 4;

  cfgR.h = &Hrsec;
  cfgS.h = &Hsss;
  cfgS.sharePt = sharePt;
  cfgS.randBytes = detRandBytes;
  cfgS.randCtx = 0;
  cfgS.secretPoint = 0;

  if (thrDspRsecInit(&dRsec, &cfgR) || thrDspSssInit(&dSss, &cfgS)) {
    fprintf(stderr, "plugin init\n");
    return (1);
  }
  if (!strcmp(plugName, "rsec"))
    plug = &dRsec;
  else if (!strcmp(plugName, "sss"))
    plug = &dSss;
  else {
    fprintf(stderr, "unknown plugin '%s' (choose rsec or sss)\n", plugName);
    return (1);
  }

  pieceSz = plug->vt->pieceSz(plug, payloadLen, n, t);
  proofSz = plug->vt->proofSz(plug, n);
  rootSz  = plug->vt->rootSz(plug);

  printf("CT04 AVID-H Disperse + Retrieve demo\n");
  printf("  plugin    : %s\n", plugName);
  printf("  n, t      : %u, %u\n", n, t);
  printf("  payload   : %u bytes\n", payloadLen);
  printf("  pieceSz   : %u\n", pieceSz);
  printf("  proofSz   : %u\n", proofSz);
  printf("  rootSz    : %u\n", rootSz);
  printf("  threshold : %u pieces to decode\n",
   plug->vt->threshold(n, t));
  printf("\n");

  sz = ct04DspSz(plug, n - 1, t, payloadLen);
  payload = malloc(payloadLen);
  encWork = malloc(plug->vt->encWaSz(plug, n, t, payloadLen) + 1);
  vfWork  = malloc(plug->vt->vfWaSz(plug) + 1);
  decWork = malloc(plug->vt->derivedRootWaSz(plug, n, t, payloadLen) + 1);
  if (!payload || !encWork || !vfWork || !decWork) {
    fprintf(stderr, "malloc\n");
    return (1);
  }
  for (i = 0; i < payloadLen; ++i)
    payload[i] = (unsigned char)((i * 53 + 11) & 0xff);

  for (i = 0; i < n; ++i) {
    inst[i] = malloc(sz);
    if (!inst[i]) {
      fprintf(stderr, "malloc inst\n");
      return (1);
    }
    ct04DspInit(inst[i], plug, (unsigned char)(n - 1), (unsigned char)t,
     (unsigned char)i, payloadLen);
    bracha87PumpInit(&pumps[i]);
  }

  if (ct04DspOrigin(inst[0], payload, encWork)) {
    fprintf(stderr, "dealer encode failed\n");
    return (1);
  }
  printf("dealer (peer 0) committed root: ");
  {
    const unsigned char *r = ct04DspRoot(inst[0]);
    for (i = 0; i < rootSz; ++i)
      printf("%02x", r[i]);
    printf("\n\n");
  }

  totalWire = 0;
  Qh = Qt = 0;
  BlobLen = 0;

  /* Main tick loop. */
  storedCnt = 0;
  for (tick = 0; tick < MAX_TICKS && storedCnt < n; ++tick) {
    unsigned int p;
    unsigned int npump;
    unsigned int u;

    for (p = 0; p < n; ++p) {
      npump = ct04DspBpr(inst[p], &pumps[p], outActs);
      for (u = 0; u < npump; ++u) {
        emit((unsigned char)p, n, &outActs[u], rootSz, proofSz, pieceSz);
        ++totalWire;
      }
    }
    while (Qh < Qt) {
      struct wireMsg *m = &Q[Qh++];
      unsigned int na;
      unsigned int a;
      na = ct04DspInput(inst[m->to], m->type, m->from, m->pieceIdx,
       Blob + m->rootOff, Blob + m->proofOff, Blob + m->pieceOff,
       vfWork, decWork, outActs);
      for (a = 0; a < na; ++a) {
        if (outActs[a].act == CT04_DSP_ACT_ABORT) {
          fprintf(stderr, "peer %u aborted unexpectedly\n", m->to);
          return (1);
        }
        if (outActs[a].act != CT04_DSP_ACT_STORED)
          emit(m->to, n, &outActs[a], rootSz, proofSz, pieceSz);
      }
    }
    storedCnt = 0;
    for (p = 0; p < n; ++p)
      if (ct04DspStored(inst[p]))
        ++storedCnt;
  }
  printf("Disperse: %u/%u peers STORED after %u ticks (wire actions: %u)\n",
   storedCnt, n, tick, totalWire);
  if (storedCnt != n)
    return (1);

  /* Retrieve: client collects k pieces, decodes payload. */
  {
    struct ct04RtvClient *cli;
    unsigned long cliSz;
    unsigned int k;
    struct ct04RtvServerResp resp;
    struct ct04RtvClientAct rcact;
    unsigned int collected;
    unsigned char *cliDec;

    k = plug->vt->threshold(n, t);
    cliSz = ct04RtvClientSz(plug, n - 1, t, payloadLen);
    cli = malloc(cliSz);
    cliDec = malloc(plug->vt->decWaSz(plug, n, t, payloadLen) + 1);
    if (!cli || !cliDec) {
      fprintf(stderr, "rtv malloc\n");
      return (1);
    }
    ct04RtvClientInit(cli, plug, (unsigned char)(n - 1),
     (unsigned char)t, payloadLen);

    collected = 0;
    rcact.act = 0;
    for (i = 0; i < n; ++i) {
      if (!ct04RtvServerRespond(inst[i], &resp))
        continue;
      if (ct04RtvClientInput(cli, (unsigned char)i, resp.pieceIdx,
       resp.root, resp.proof, resp.piece,
       vfWork, cliDec, &rcact))
        ++collected;
      else
        ++collected;
      if (rcact.act == CT04_RTV_ACT_RETRIEVED)
        break;
    }
    printf("Retrieve: collected %u/%u blocks; ", collected, k);
    if (rcact.act == CT04_RTV_ACT_RETRIEVED) {
      if (!memcmp(rcact.payload, payload, payloadLen))
        printf("payload matches original.\n");
      else {
        printf("PAYLOAD MISMATCH.\n");
        return (1);
      }
    } else {
      printf("never RETRIEVED.\n");
      return (1);
    }
    free(cli);
    free(cliDec);
  }

  for (i = 0; i < n; ++i)
    free(inst[i]);
  free(payload);
  free(encWork);
  free(vfWork);
  free(decWork);
  thrDspRsecFini(&dRsec);
  thrDspSssFini(&dSss);
  return (0);
}
