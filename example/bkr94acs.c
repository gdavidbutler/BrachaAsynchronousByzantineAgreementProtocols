/*
 * asynchronousByzantineAgreementProtocols - Example BKR94 ACS program
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
 * bkr94acs.c — Standalone demonstration of the BKR94 Asynchronous
 * Common Subset protocol (Ben-Or/Kelmer/Rabin 1994 Section 4
 * Figure 3, composing Bracha87 Figures 1 and 4).
 *
 * Each of N peers proposes a string value. The BKR94 ACS protocol
 * ensures all honest peers agree on the same common subset of
 * proposals (at least n-t). The subset is then sorted
 * deterministically so every peer outputs the same ordering — the
 * core of atomic broadcast.
 *
 * Build:
 *   (from project root) make example_bkr94acs
 *
 * Usage:
 *   ./example_bkr94acs [-v] [-s seed] n t proposal0 proposal1 ...
 *
 * Example:
 *   ./example_bkr94acs 4 1 joe sam sally tim
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bkr94acs.h"

/*------------------------------------------------------------------------*/
/*  Constants                                                             */
/*------------------------------------------------------------------------*/

#define MAX_PEERS  16
#define MAX_PHASES 10
#define MAX_VLEN   255  /* max proposal bytes (including \0) */

/*------------------------------------------------------------------------*/
/*  Message queue — simulated network                                     */
/*                                                                        */
/*  Two message classes share one queue:                                   */
/*    BKR94ACS_CLS_PROPOSAL  — Fig1 messages carrying proposal values      */
/*    BKR94ACS_CLS_CONSENSUS — Fig1 messages for per-origin binary        */
/*                             consensus                                  */
/*------------------------------------------------------------------------*/

struct msg {
  unsigned char cls;         /* BKR94ACS_CLS_PROPOSAL or BKR94ACS_CLS_CONSENSUS */
  unsigned char origin;      /* which origin */
  unsigned char round;       /* consensus round (cls=CONSENSUS only) */
  unsigned char broadcaster; /* who initiated this Fig1 broadcast (CONSENSUS) */
  unsigned char type;        /* BRACHA87_INITIAL, BRACHA87_ECHO, BRACHA87_READY */
  unsigned char from;        /* sender */
  unsigned char to;          /* recipient */
  unsigned char value[MAX_VLEN];  /* proposal value or binary consensus value */
};

static struct msg *MsgQ;
static unsigned int Qcap;
static unsigned int Qhead;
static unsigned int Qtail;

static int
qAlloc(
  unsigned int cap
){
  MsgQ = (struct msg *)calloc(cap, sizeof (struct msg));
  if (!MsgQ)
    return (-1);
  Qcap = cap;
  Qhead = Qtail = 0;
  return (0);
}

static void
qFree(
  void
){
  free(MsgQ);
  MsgQ = 0;
}

static void
qPush(
  unsigned char cls
 ,unsigned char origin
 ,unsigned char round
 ,unsigned char broadcaster
 ,unsigned char type
 ,unsigned char from
 ,unsigned char to
 ,const unsigned char *value
 ,unsigned int valueLen
){
  if (Qtail >= Qcap)
    return;
  MsgQ[Qtail].cls = cls;
  MsgQ[Qtail].origin = origin;
  MsgQ[Qtail].round = round;
  MsgQ[Qtail].broadcaster = broadcaster;
  MsgQ[Qtail].type = type;
  MsgQ[Qtail].from = from;
  MsgQ[Qtail].to = to;
  memcpy(MsgQ[Qtail].value, value, valueLen);
  ++Qtail;
}

/*
 * Fisher-Yates shuffle of the unprocessed portion of the queue.
 */
static void
qShuffle(
  unsigned int *seed
){
  unsigned int n;
  unsigned int i;

  n = Qtail - Qhead;
  if (n < 2)
    return;
  for (i = n - 1; i > 0; --i) {
    unsigned int j;
    struct msg tmp;

    *seed = *seed * 1103515245u + 12345u;
    j = ((*seed >> 16) & 0x7FFF) % (i + 1);
    tmp = MsgQ[Qhead + i];
    MsgQ[Qhead + i] = MsgQ[Qhead + j];
    MsgQ[Qhead + j] = tmp;
  }
}

/*------------------------------------------------------------------------*/
/*  Coin — deterministic alternating, adequate for demonstration only.    */
/*  Adversarial deployments should pass a local random source             */
/*  (e.g. arc4random).                                                    */
/*------------------------------------------------------------------------*/

static unsigned char
demoCoin(
  void *closure
 ,unsigned char phase
){
  (void)closure;
  return (phase % 2);
}

/*------------------------------------------------------------------------*/
/*  Verbose helpers                                                       */
/*------------------------------------------------------------------------*/

static const char *
typeName(
  unsigned char type
){
  switch (type) {
  case BRACHA87_INITIAL: return ("INITIAL");
  case BRACHA87_ECHO:    return ("ECHO");
  case BRACHA87_READY:   return ("READY");
  default:               return ("???");
  }
}

/*------------------------------------------------------------------------*/
/*  String comparison for qsort                                           */
/*------------------------------------------------------------------------*/

static int
strPtrCmp(
  const void *a
 ,const void *b
){
  return (strcmp(*(const char *const *)a, *(const char *const *)b));
}

/*------------------------------------------------------------------------*/
/*  Main simulation                                                       */
/*------------------------------------------------------------------------*/

int
main(
  int argc
 ,char *argv[]
){
  /* Configuration */
  unsigned int n;
  unsigned int t;
  unsigned int verbose;
  unsigned int shuffleSeed;
  unsigned int origSeed;
  unsigned int vLen;

  /* Per-peer BKR94 ACS state */
  struct bkr94acs *peers[MAX_PEERS];
  unsigned long acsSize;

  /* Proposal strings */
  char proposals[MAX_PEERS][MAX_VLEN];

  /* Loop / temp */
  unsigned int i;
  unsigned int j;
  int arg;
  int exitCode;

  /*----------------------------------------------------------------------*/
  /*  Parse command line                                                   */
  /*----------------------------------------------------------------------*/

  verbose = 0;
  shuffleSeed = 0;
  exitCode = 0;

  arg = 1;
  while (arg < argc && argv[arg][0] == '-') {
    if (argv[arg][1] == 'v' && argv[arg][2] == '\0') {
      verbose = 1;
      ++arg;
    } else if (argv[arg][1] == 's' && argv[arg][2] == '\0') {
      ++arg;
      if (arg >= argc) goto usage;
      shuffleSeed = (unsigned int)atoi(argv[arg]);
      ++arg;
    } else {
      goto usage;
    }
  }

  if (argc - arg < 2) goto usage;
  n = (unsigned int)atoi(argv[arg++]);
  t = (unsigned int)atoi(argv[arg++]);

  if (n < 1 || n > MAX_PEERS) {
    fprintf(stderr, "n must be 1..%d\n", MAX_PEERS);
    return (1);
  }
  if (n <= 3 * t) {
    fprintf(stderr, "need n > 3t (n=%u, t=%u)\n", n, t);
    return (1);
  }
  if ((unsigned int)(argc - arg) < n) {
    fprintf(stderr, "need %u proposal strings\n", n);
    return (1);
  }

  origSeed = shuffleSeed;

  /* Read proposals, find max length for vLen */
  vLen = 1;
  memset(proposals, 0, sizeof (proposals));
  for (i = 0; i < n; ++i) {
    unsigned int len;

    len = (unsigned int)strlen(argv[arg]) + 1; /* include \0 */
    if (len > MAX_VLEN) {
      fprintf(stderr, "proposal too long: %s (max %d bytes)\n",
              argv[arg], MAX_VLEN - 1);
      return (1);
    }
    memcpy(proposals[i], argv[arg], len);
    if (len > vLen)
      vLen = len;
    ++arg;
  }

  /*----------------------------------------------------------------------*/
  /*  Allocate per-peer BKR94 ACS state                                   */
  /*----------------------------------------------------------------------*/

  /* vLen encoding: actual length = vLen, encoding = vLen - 1 */
  acsSize = bkr94acsSz((unsigned int)(n - 1), (unsigned int)(vLen - 1), MAX_PHASES);

  memset(peers, 0, sizeof (peers));
  for (i = 0; i < n; ++i) {
    peers[i] = (struct bkr94acs *)calloc(1, acsSize);
    if (!peers[i]) {
      fprintf(stderr, "allocation failed\n");
      exitCode = 1;
      goto cleanup;
    }
    bkr94acsInit(peers[i], (unsigned char)(n - 1), (unsigned char)t,
                 (unsigned char)(vLen - 1), MAX_PHASES, (unsigned char)i,
                 demoCoin, 0);
  }

  /*----------------------------------------------------------------------*/
  /*  Allocate message queue                                              */
  /*  BKR94 ACS generates more messages than plain consensus:             */
  /*  N proposal broadcasts + N consensus pipelines, each with rounds.    */
  /*----------------------------------------------------------------------*/

  if (qAlloc(64u * n * n * (unsigned int)MAX_PHASES * 3 + 1024)) {
    fprintf(stderr, "queue allocation failed\n");
    exitCode = 1;
    goto cleanup;
  }

  /*----------------------------------------------------------------------*/
  /*  Bootstrap: each peer Proposes their value                           */
  /*                                                                      */
  /*  bkr94acsPropose marks the local proposal Fig1 as the broadcast      */
  /*  originator and emits one BKR94ACS_ACT_PROP_SEND action (.type =     */
  /*  BRACHA87_INITIAL) for the application to broadcast to all peers.    */
  /*  Replay thereafter is intrinsic to BPR (bkr94acsPump) -- no          */
  /*  application ledger required.                                        */
  /*----------------------------------------------------------------------*/

  for (i = 0; i < n; ++i) {
    struct bkr94acsAct propAct;
    unsigned int nProp;

    nProp = bkr94acsPropose(peers[i],
                            (const unsigned char *)proposals[i],
                            &propAct);
    if (nProp != 1)
      continue;
    for (j = 0; j < n; ++j)
      qPush(BKR94ACS_CLS_PROPOSAL, propAct.origin, 0, 0,
            BRACHA87_INITIAL, (unsigned char)i, (unsigned char)j,
            (const unsigned char *)proposals[i], vLen);
  }

  if (shuffleSeed)
    qShuffle(&shuffleSeed);

  /*----------------------------------------------------------------------*/
  /*  Process message queue                                               */
  /*----------------------------------------------------------------------*/

  while (Qhead < Qtail) {
    struct msg *m;
    struct bkr94acs *st;
    struct bkr94acsAct acts[BKR94ACS_MAX_ACTS(MAX_PEERS, MAX_PHASES)];
    unsigned int nacts;
    unsigned int k;
    unsigned int oldTail;

    m = &MsgQ[Qhead++];
    st = peers[m->to];

    /*
     * Do NOT skip messages addressed to locally-complete peers.
     * A peer that has decided all N BAs must keep processing
     * incoming messages so its Fig1 echoes/readys continue to
     * reach peers still working on some BAs.  Skipping replicates
     * the post-decide stall the library itself was fixed to avoid
     * (see bkr94acs.c bkr94acsConsensusInput comment on
     * a->complete).  The simulation loop terminates when the
     * message queue drains, not when any one peer reaches complete.
     */
    oldTail = Qtail;

    if (m->cls == BKR94ACS_CLS_PROPOSAL) {
      if (verbose)
        printf("peer %u: recv PROP %s(origin=%u) from %u\n",
               (unsigned)m->to, typeName(m->type),
               (unsigned)m->origin, (unsigned)m->from);

      nacts = bkr94acsProposalInput(st, m->origin, m->type, m->from,
                                    m->value, acts);
    } else {
      if (verbose)
        printf("peer %u: recv CON %s(origin=%u, round=%u, val=%u) from %u\n",
               (unsigned)m->to, typeName(m->type),
               (unsigned)m->origin, (unsigned)m->round,
               (unsigned)m->value[0], (unsigned)m->from);

      nacts = bkr94acsConsensusInput(st, m->origin, m->round,
                                     m->broadcaster, m->type,
                                     m->from, m->value[0], acts);
    }

    /* Enqueue output actions as network messages */
    for (k = 0; k < nacts; ++k) {
      unsigned int p;

      switch (acts[k].act) {

      case BKR94ACS_ACT_PROP_SEND:
        if (!acts[k].value)
          break;
        if (verbose)
          printf("peer %u: -> PROP %s(origin=%u)\n",
                 (unsigned)m->to, typeName(acts[k].type),
                 (unsigned)acts[k].origin);
        for (p = 0; p < n; ++p)
          qPush(BKR94ACS_CLS_PROPOSAL, acts[k].origin, 0, 0,
                acts[k].type, m->to, (unsigned char)p,
                acts[k].value, vLen);
        break;

      case BKR94ACS_ACT_CON_SEND:
        if (verbose)
          printf("peer %u: -> CON %s(origin=%u, round=%u, bcaster=%u, val=%u)\n",
                 (unsigned)m->to, typeName(acts[k].type),
                 (unsigned)acts[k].origin, (unsigned)acts[k].round,
                 (unsigned)acts[k].broadcaster,
                 (unsigned)acts[k].conValue);
        for (p = 0; p < n; ++p)
          qPush(BKR94ACS_CLS_CONSENSUS, acts[k].origin, acts[k].round,
                acts[k].broadcaster, acts[k].type,
                m->to, (unsigned char)p,
                &acts[k].conValue, 1);
        break;

      case BKR94ACS_ACT_BA_DECIDED:
        if (verbose)
          printf("peer %u: BA[%u] decided %u\n",
                 (unsigned)m->to, (unsigned)acts[k].origin,
                 (unsigned)acts[k].conValue);
        break;

      case BKR94ACS_ACT_COMPLETE:
        if (verbose)
          printf("peer %u: BKR94 ACS COMPLETE\n", (unsigned)m->to);
        break;
      }
    }

    if (shuffleSeed && Qtail > oldTail)
      qShuffle(&shuffleSeed);
  }

  /*----------------------------------------------------------------------*/
  /*  Output: each peer's agreed common subset in sorted order            */
  /*----------------------------------------------------------------------*/

  printf("\n--- BKR94 ACS Results (n=%u, t=%u, seed=%u) ---\n", n, t, origSeed);

  {
    /* Verify all honest peers agree on the same subset and ordering */
    int allAgree;
    unsigned char firstSubset[MAX_PEERS];
    unsigned int firstCnt;

    allAgree = 1;
    firstCnt = 0;

    for (i = 0; i < n; ++i) {
      unsigned char subset[MAX_PEERS];
      unsigned int cnt;
      const char *sorted[MAX_PEERS];

      if (!peers[i]->complete) {
        printf("Peer %u: BKR94 ACS did not complete\n", i);
        exitCode = 1;
        continue;
      }

      cnt = bkr94acsSubset(peers[i], subset);
      printf("Peer %u: common subset (%u/%u proposals):\n", i, cnt, n);

      /* Collect proposal strings for sorted output */
      for (j = 0; j < cnt; ++j) {
        const unsigned char *pv;

        pv = bkr94acsProposalValue(peers[i], subset[j]);
        sorted[j] = pv ? (const char *)pv : "(null)";
      }

      /* Sort lexicographically — deterministic ordering */
      qsort(sorted, cnt, sizeof (sorted[0]), strPtrCmp);

      for (j = 0; j < cnt; ++j)
        printf("  %s\n", sorted[j]);

      /* Check agreement with first peer */
      if (i == 0) {
        firstCnt = cnt;
        memcpy(firstSubset, subset, cnt);
      } else {
        if (cnt != firstCnt)
          allAgree = 0;
        else if (memcmp(subset, firstSubset, cnt))
          allAgree = 0;
      }
    }

    printf("\nAll peers agree on subset: %s\n",
           allAgree ? "ok" : "FAIL");
    if (!allAgree)
      exitCode = 1;
  }

  /*----------------------------------------------------------------------*/
  /*  Cleanup                                                             */
  /*----------------------------------------------------------------------*/

cleanup:
  for (i = 0; i < n; ++i)
    free(peers[i]);
  qFree();

  return (exitCode);

usage:
  fprintf(stderr,
    "usage: example_bkr94acs [-v] [-s seed] n t proposal0 proposal1 ...\n"
    "  n            total peers (1-%d)\n"
    "  t            max Byzantine faults\n"
    "  proposal*    per-peer proposal strings\n"
    "  -v           verbose: trace every message\n"
    "  -s seed      shuffle seed (0 = ordered delivery)\n",
    MAX_PEERS);
  return (1);
}
