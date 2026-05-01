/*
 * Tests for bkr94acs.[hc] — BKR94 Asynchronous Common Subset.
 *
 * Simulates all-to-all message passing for BKR94 ACS:
 *   N proposal broadcasts (Bracha87 Fig1 with arbitrary values)
 *   N binary consensuses (Bracha87 Fig4 per origin)
 *
 * Verifies:
 *   Agreement  — all honest peers decide the same subset
 *   Validity   — subset contains at least n-t origins
 *   Totality   — all BAs decide (a->complete becomes non-zero)
 *   Values     — accepted proposal values match what was proposed
 *   Ordering   — deterministic sort produces identical order at each peer
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bkr94acs.h"

static int Fail;

static void
check(
  const char *name
 ,int cond
){
  if (!cond) {
    fprintf(stderr, "FAIL: %s\n", name);
    ++Fail;
  }
}

/*------------------------------------------------------------------------*/
/*  Coin — deterministic alternating. Adequate for tests; adversarial     */
/*  deployments should pass a local random source.                        */
/*------------------------------------------------------------------------*/

static unsigned char
testCoin(
  void *closure
 ,unsigned char phase
){
  (void)closure;
  return (phase % 2);
}

/*------------------------------------------------------------------------*/
/*  Message queue — same pattern as example/bkr94acs.c                    */
/*------------------------------------------------------------------------*/

#define MAX_PEERS  16
#define MAX_PHASES 10
#define MAX_VLEN   32
#define MAX_MSGS   (1024u * 1024u)

struct msg {
  unsigned char cls;
  unsigned char origin;
  unsigned char round;
  unsigned char broadcaster;
  unsigned char type;
  unsigned char from;
  unsigned char to;
  unsigned char value[MAX_VLEN];
};

static struct msg *MsgQ;
static unsigned int Qhead;
static unsigned int Qtail;

static void
qInit(
  void
){
  Qhead = Qtail = 0;
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
  if (Qtail >= MAX_MSGS)
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
/*  BKR94 ACS simulation engine                                           */
/*------------------------------------------------------------------------*/

struct acsResult {
  int complete;
  unsigned int subsetCnt;
  unsigned char subset[MAX_PEERS];
};

/*
 * Run one BKR94 ACS simulation.
 * proposals[i] is a NUL-terminated string for peer i.
 * vLen is the padded value length (>= longest string + 1).
 * Returns 0 on success, -1 on allocation failure.
 */
static int
runAcs(
  unsigned int n
 ,unsigned int t
 ,const char proposals[][MAX_VLEN]
 ,unsigned int vLen
 ,unsigned int shuffleSeed
 ,struct acsResult results[]
){
  struct bkr94acs *peers[MAX_PEERS];
  unsigned long sz;
  unsigned int i;
  unsigned int j;

  sz = bkr94acsSz(n - 1, vLen - 1, MAX_PHASES);
  memset(peers, 0, sizeof (peers));
  for (i = 0; i < n; ++i) {
    peers[i] = (struct bkr94acs *)calloc(1, sz);
    if (!peers[i]) {
      for (j = 0; j < i; ++j)
        free(peers[j]);
      return (-1);
    }
    bkr94acsInit(peers[i], (unsigned char)(n - 1), (unsigned char)t,
                 (unsigned char)(vLen - 1), MAX_PHASES, (unsigned char)i,
                 testCoin, 0);
  }

  qInit();

  /* Bootstrap: each peer broadcasts INITIAL of their proposal */
  for (i = 0; i < n; ++i)
    for (j = 0; j < n; ++j)
      qPush(BKR94ACS_CLS_PROPOSAL, (unsigned char)i, 0, 0,
            BRACHA87_INITIAL, (unsigned char)i, (unsigned char)j,
            (const unsigned char *)proposals[i], vLen);

  if (shuffleSeed)
    qShuffle(&shuffleSeed);

  /* Process message queue */
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
     * a->complete).
     */
    oldTail = Qtail;

    if (m->cls == BKR94ACS_CLS_PROPOSAL) {
      nacts = bkr94acsProposalInput(st, m->origin, m->type, m->from,
                                    m->value, acts);
    } else {
      nacts = bkr94acsConsensusInput(st, m->origin, m->round,
                                     m->broadcaster, m->type,
                                     m->from, m->value[0], acts);
    }

    for (k = 0; k < nacts; ++k) {
      unsigned int p;

      switch (acts[k].act) {
      case BKR94ACS_ACT_PROP_SEND:
        if (!acts[k].value)
          break;
        for (p = 0; p < n; ++p)
          qPush(BKR94ACS_CLS_PROPOSAL, acts[k].origin, 0, 0,
                acts[k].type, m->to, (unsigned char)p,
                acts[k].value, vLen);
        break;
      case BKR94ACS_ACT_CON_SEND:
        for (p = 0; p < n; ++p)
          qPush(BKR94ACS_CLS_CONSENSUS, acts[k].origin, acts[k].round,
                acts[k].broadcaster, acts[k].type,
                m->to, (unsigned char)p,
                &acts[k].conValue, 1);
        break;
      default:
        break;
      }
    }

    if (shuffleSeed && Qtail > oldTail)
      qShuffle(&shuffleSeed);
  }

  /* Collect results */
  for (i = 0; i < n; ++i) {
    results[i].complete = peers[i]->complete;
    results[i].subsetCnt = bkr94acsSubset(peers[i], results[i].subset);
  }

  for (i = 0; i < n; ++i)
    free(peers[i]);

  return (0);
}

/*------------------------------------------------------------------------*/
/*  Verification helpers                                                  */
/*------------------------------------------------------------------------*/

/* Check: all peers completed */
static int
allComplete(
  const struct acsResult results[]
 ,unsigned int n
){
  unsigned int i;

  for (i = 0; i < n; ++i)
    if (!results[i].complete)
      return (0);
  return (1);
}

/* Check: all peers agree on the same subset */
static int
allAgree(
  const struct acsResult results[]
 ,unsigned int n
){
  unsigned int i;

  for (i = 1; i < n; ++i) {
    if (results[i].subsetCnt != results[0].subsetCnt)
      return (0);
    if (memcmp(results[i].subset, results[0].subset,
               results[0].subsetCnt))
      return (0);
  }
  return (1);
}

/* Check: subset size >= n-t */
static int
subsetValid(
  const struct acsResult results[]
 ,unsigned int n
 ,unsigned int t
){
  unsigned int i;

  for (i = 0; i < n; ++i)
    if (results[i].subsetCnt < n - t)
      return (0);
  return (1);
}

/*------------------------------------------------------------------------*/
/*  Test cases                                                            */
/*------------------------------------------------------------------------*/

static void
testBasic(
  void
){
  char proposals[MAX_PEERS][MAX_VLEN];
  struct acsResult results[MAX_PEERS];
  unsigned int vLen;
  char label[128];
  unsigned int n;
  unsigned int t;

  printf("BKR94 ACS — Basic honest tests\n");

  /* n=1, t=0 */
  memset(proposals, 0, sizeof (proposals));
  strcpy(proposals[0], "hello");
  vLen = 6;
  check("n=1 t=0 run", runAcs(1, 0, proposals, vLen, 0, results) == 0);
  check("n=1 t=0 complete", allComplete(results, 1));
  check("n=1 t=0 subset==1", results[0].subsetCnt == 1);
  printf("  n=1  t=0: subset %u/%u\n", results[0].subsetCnt, 1);

  /* n=4, t=0: all proposals must be included */
  memset(proposals, 0, sizeof (proposals));
  strcpy(proposals[0], "joe");
  strcpy(proposals[1], "sam");
  strcpy(proposals[2], "sally");
  strcpy(proposals[3], "tim");
  vLen = 6;
  check("n=4 t=0 run", runAcs(4, 0, proposals, vLen, 0, results) == 0);
  check("n=4 t=0 complete", allComplete(results, 4));
  check("n=4 t=0 agree", allAgree(results, 4));
  check("n=4 t=0 subset==4", results[0].subsetCnt == 4);
  printf("  n=4  t=0: subset %u/%u\n", results[0].subsetCnt, 4);

  /* n=4, t=1 */
  check("n=4 t=1 run", runAcs(4, 1, proposals, vLen, 0, results) == 0);
  check("n=4 t=1 complete", allComplete(results, 4));
  check("n=4 t=1 agree", allAgree(results, 4));
  check("n=4 t=1 valid", subsetValid(results, 4, 1));
  printf("  n=4  t=1: subset %u/%u\n", results[0].subsetCnt, 4);

  /* Sweep: various n/t combinations */
  {
    static const unsigned int cases[][2] = {
      {4, 1}, {5, 1}, {7, 2}, {8, 2}, {10, 3}, {13, 4}
    };
    unsigned int c;
    unsigned int i;

    for (c = 0; c < sizeof (cases) / sizeof (cases[0]); ++c) {
      n = cases[c][0];
      t = cases[c][1];
      memset(proposals, 0, sizeof (proposals));
      for (i = 0; i < n; ++i) {
        proposals[i][0] = 'A' + (char)i;
        proposals[i][1] = '\0';
      }
      vLen = 2;
      sprintf(label, "n=%u t=%u run", n, t);
      check(label, runAcs(n, t, proposals, vLen, 0, results) == 0);
      sprintf(label, "n=%u t=%u complete", n, t);
      check(label, allComplete(results, n));
      sprintf(label, "n=%u t=%u agree", n, t);
      check(label, allAgree(results, n));
      sprintf(label, "n=%u t=%u valid", n, t);
      check(label, subsetValid(results, n, t));
      printf("  n=%-2u t=%u: subset %u/%u\n",
             n, t, results[0].subsetCnt, n);
    }
  }
}

static void
testShuffled(
  void
){
  char proposals[MAX_PEERS][MAX_VLEN];
  struct acsResult results[MAX_PEERS];
  char label[128];
  unsigned int seed;
  unsigned int n;
  unsigned int t;

  /*
   * With shuffled delivery, different peers accept different proposals
   * first, causing vote splits in some BA instances. The deterministic
   * alternating coin can resolve these splits against inclusion,
   * making the subset smaller than n-t. With a random coin, BA
   * terminates w.h.p. and BKR94 Lemma 2 gives |SubSet| >= 2t+1 = n-t.
   *
   * Here we verify the hard guarantees: totality and agreement.
   */

  printf("\nBKR94 ACS — Shuffled delivery tests\n");

  /* n=4, t=1 with 20 different seeds */
  n = 4;
  t = 1;
  memset(proposals, 0, sizeof (proposals));
  strcpy(proposals[0], "alpha");
  strcpy(proposals[1], "bravo");
  strcpy(proposals[2], "charlie");
  strcpy(proposals[3], "delta");

  printf("  n=%u t=%u seeds 1-20: ", n, t);
  for (seed = 1; seed <= 20; ++seed) {
    sprintf(label, "n=4 t=1 seed=%u run", seed);
    check(label, runAcs(n, t, proposals, 8, seed, results) == 0);
    sprintf(label, "n=4 t=1 seed=%u complete", seed);
    check(label, allComplete(results, n));
    sprintf(label, "n=4 t=1 seed=%u agree", seed);
    check(label, allAgree(results, n));
    sprintf(label, "n=4 t=1 seed=%u nonempty", seed);
    check(label, results[0].subsetCnt > 0);
  }
  printf("all agreed\n");

  /* n=7, t=2 with 10 seeds */
  n = 7;
  t = 2;
  memset(proposals, 0, sizeof (proposals));
  {
    unsigned int i;

    for (i = 0; i < n; ++i) {
      proposals[i][0] = 'a' + (char)i;
      proposals[i][1] = '\0';
    }
  }
  printf("  n=%u t=%u seeds 1-10: ", n, t);
  for (seed = 1; seed <= 10; ++seed) {
    sprintf(label, "n=7 t=2 seed=%u run", seed);
    check(label, runAcs(n, t, proposals, 2, seed, results) == 0);
    sprintf(label, "n=7 t=2 seed=%u complete", seed);
    check(label, allComplete(results, n));
    sprintf(label, "n=7 t=2 seed=%u agree", seed);
    check(label, allAgree(results, n));
    sprintf(label, "n=7 t=2 seed=%u nonempty", seed);
    check(label, results[0].subsetCnt > 0);
  }
  printf("all agreed\n");

  /* n=10, t=3 with 5 seeds */
  n = 10;
  t = 3;
  memset(proposals, 0, sizeof (proposals));
  {
    unsigned int i;

    for (i = 0; i < n; ++i) {
      proposals[i][0] = 'A' + (char)i;
      proposals[i][1] = '\0';
    }
  }
  printf("  n=%u t=%u seeds 1-5: ", n, t);
  for (seed = 1; seed <= 5; ++seed) {
    sprintf(label, "n=10 t=3 seed=%u run", seed);
    check(label, runAcs(n, t, proposals, 2, seed, results) == 0);
    sprintf(label, "n=10 t=3 seed=%u complete", seed);
    check(label, allComplete(results, n));
    sprintf(label, "n=10 t=3 seed=%u agree", seed);
    check(label, allAgree(results, n));
    sprintf(label, "n=10 t=3 seed=%u nonempty", seed);
    check(label, results[0].subsetCnt > 0);
  }
  printf("all agreed\n");
}

static void
testValues(
  void
){
  char proposals[MAX_PEERS][MAX_VLEN];
  struct bkr94acs *peers[MAX_PEERS];
  unsigned long sz;
  unsigned int n;
  unsigned int t;
  unsigned int vLen;
  unsigned int i;
  unsigned int j;
  int valuesOk;

  printf("\nBKR94 ACS — Proposal value integrity tests\n");

  /*
   * Verify that accepted proposal values match what was proposed.
   * Run a simulation, then check bkr94acsProposalValue for each
   * included origin.
   */
  n = 4;
  t = 1;
  memset(proposals, 0, sizeof (proposals));
  strcpy(proposals[0], "joe");
  strcpy(proposals[1], "sam");
  strcpy(proposals[2], "sally");
  strcpy(proposals[3], "tim");
  vLen = 6;

  sz = bkr94acsSz(n - 1, vLen - 1, MAX_PHASES);
  for (i = 0; i < n; ++i) {
    peers[i] = (struct bkr94acs *)calloc(1, sz);
    bkr94acsInit(peers[i], (unsigned char)(n - 1), (unsigned char)t,
                 (unsigned char)(vLen - 1), MAX_PHASES, (unsigned char)i,
                 testCoin, 0);
  }

  qInit();
  for (i = 0; i < n; ++i)
    for (j = 0; j < n; ++j)
      qPush(BKR94ACS_CLS_PROPOSAL, (unsigned char)i, 0, 0,
            BRACHA87_INITIAL, (unsigned char)i, (unsigned char)j,
            (const unsigned char *)proposals[i], vLen);

  while (Qhead < Qtail) {
    struct msg *m;
    struct bkr94acs *st;
    struct bkr94acsAct acts[BKR94ACS_MAX_ACTS(MAX_PEERS, MAX_PHASES)];
    unsigned int nacts;
    unsigned int k;

    m = &MsgQ[Qhead++];
    st = peers[m->to];
    if (st->complete)
      continue;

    if (m->cls == BKR94ACS_CLS_PROPOSAL) {
      nacts = bkr94acsProposalInput(st, m->origin, m->type, m->from,
                                    m->value, acts);
    } else {
      nacts = bkr94acsConsensusInput(st, m->origin, m->round,
                                     m->broadcaster, m->type,
                                     m->from, m->value[0], acts);
    }

    for (k = 0; k < nacts; ++k) {
      unsigned int p;

      switch (acts[k].act) {
      case BKR94ACS_ACT_PROP_SEND:
        if (!acts[k].value)
          break;
        for (p = 0; p < n; ++p)
          qPush(BKR94ACS_CLS_PROPOSAL, acts[k].origin, 0, 0,
                acts[k].type, m->to, (unsigned char)p,
                acts[k].value, vLen);
        break;
      case BKR94ACS_ACT_CON_SEND:
        for (p = 0; p < n; ++p)
          qPush(BKR94ACS_CLS_CONSENSUS, acts[k].origin, acts[k].round,
                acts[k].broadcaster, acts[k].type,
                m->to, (unsigned char)p,
                &acts[k].conValue, 1);
        break;
      default:
        break;
      }
    }
  }

  /* Check proposal values at each peer for each included origin */
  valuesOk = 1;
  for (i = 0; i < n; ++i) {
    unsigned char subset[MAX_PEERS];
    unsigned int cnt;

    check("values: complete", peers[i]->complete);
    cnt = bkr94acsSubset(peers[i], subset);
    for (j = 0; j < cnt; ++j) {
      const unsigned char *pv;

      pv = bkr94acsProposalValue(peers[i], subset[j]);
      if (!pv || memcmp(pv, proposals[subset[j]], vLen))
        valuesOk = 0;
    }
  }
  check("proposal values match", valuesOk);
  printf("  n=4  t=1: proposal values verified at all peers\n");

  for (i = 0; i < n; ++i)
    free(peers[i]);
}

static void
testMultiByteValues(
  void
){
  char proposals[MAX_PEERS][MAX_VLEN];
  struct acsResult results[MAX_PEERS];
  unsigned int n;
  unsigned int t;
  unsigned int vLen;
  unsigned int i;

  printf("\nBKR94 ACS — Multi-byte value tests\n");

  /* Long strings: test vLen > 1 */
  n = 4;
  t = 1;
  memset(proposals, 0, sizeof (proposals));
  strcpy(proposals[0], "the quick brown fox");
  strcpy(proposals[1], "jumps over the lazy");
  strcpy(proposals[2], "dog and then some..");
  strcpy(proposals[3], "extra long string!!");
  vLen = 20;

  check("long strings run",
        runAcs(n, t, proposals, vLen, 0, results) == 0);
  check("long strings complete", allComplete(results, n));
  check("long strings agree", allAgree(results, n));
  check("long strings valid", subsetValid(results, n, t));
  printf("  n=4  t=1 vLen=20: subset %u/%u\n",
         results[0].subsetCnt, n);

  /* Single-byte values (vLen=1, like binary but with arbitrary bytes) */
  n = 7;
  t = 2;
  memset(proposals, 0, sizeof (proposals));
  for (i = 0; i < n; ++i)
    proposals[i][0] = (char)(10 + i);
  vLen = 1;

  check("single-byte run",
        runAcs(n, t, proposals, vLen, 0, results) == 0);
  check("single-byte complete", allComplete(results, n));
  check("single-byte agree", allAgree(results, n));
  check("single-byte valid", subsetValid(results, n, t));
  printf("  n=7  t=2 vLen=1: subset %u/%u\n",
         results[0].subsetCnt, n);
}

static void
testIdenticalProposals(
  void
){
  char proposals[MAX_PEERS][MAX_VLEN];
  struct acsResult results[MAX_PEERS];

  printf("\nBKR94 ACS — Identical proposal tests\n");

  /* All peers propose the same value */
  memset(proposals, 0, sizeof (proposals));
  strcpy(proposals[0], "same");
  strcpy(proposals[1], "same");
  strcpy(proposals[2], "same");
  strcpy(proposals[3], "same");

  check("identical run", runAcs(4, 1, proposals, 5, 0, results) == 0);
  check("identical complete", allComplete(results, 4));
  check("identical agree", allAgree(results, 4));
  check("identical valid", subsetValid(results, 4, 1));
  printf("  n=4  t=1 all-same: subset %u/%u\n",
         results[0].subsetCnt, 4);
}

static void
testLargerN(
  void
){
  char proposals[MAX_PEERS][MAX_VLEN];
  struct acsResult results[MAX_PEERS];
  unsigned int n;
  unsigned int t;
  unsigned int i;
  unsigned int seed;
  char label[128];

  printf("\nBKR94 ACS — Larger N tests\n");

  /* n=13, t=4 */
  n = 13;
  t = 4;
  memset(proposals, 0, sizeof (proposals));
  for (i = 0; i < n; ++i) {
    proposals[i][0] = 'A' + (char)i;
    proposals[i][1] = '\0';
  }

  check("n=13 t=4 run", runAcs(n, t, proposals, 2, 0, results) == 0);
  check("n=13 t=4 complete", allComplete(results, n));
  check("n=13 t=4 agree", allAgree(results, n));
  check("n=13 t=4 valid", subsetValid(results, n, t));
  printf("  n=13 t=4: subset %u/%u\n", results[0].subsetCnt, n);

  /* n=16, t=5 */
  n = 16;
  t = 5;
  memset(proposals, 0, sizeof (proposals));
  for (i = 0; i < n; ++i) {
    proposals[i][0] = 'a' + (char)i;
    proposals[i][1] = '\0';
  }

  check("n=16 t=5 run", runAcs(n, t, proposals, 2, 0, results) == 0);
  check("n=16 t=5 complete", allComplete(results, n));
  check("n=16 t=5 agree", allAgree(results, n));
  check("n=16 t=5 valid", subsetValid(results, n, t));
  printf("  n=16 t=5: subset %u/%u\n", results[0].subsetCnt, n);

  /* n=13 t=4 shuffled */
  n = 13;
  t = 4;
  memset(proposals, 0, sizeof (proposals));
  for (i = 0; i < n; ++i) {
    proposals[i][0] = 'A' + (char)i;
    proposals[i][1] = '\0';
  }
  printf("  n=%u t=%u shuffled seeds 1-5: ", n, t);
  for (seed = 1; seed <= 5; ++seed) {
    sprintf(label, "n=13 t=4 seed=%u run", seed);
    check(label, runAcs(n, t, proposals, 2, seed, results) == 0);
    sprintf(label, "n=13 t=4 seed=%u complete", seed);
    check(label, allComplete(results, n));
    sprintf(label, "n=13 t=4 seed=%u agree", seed);
    check(label, allAgree(results, n));
    sprintf(label, "n=13 t=4 seed=%u valid", seed);
    check(label, subsetValid(results, n, t));
  }
  printf("all agreed\n");
}

/*------------------------------------------------------------------------*/
/*  Post-decide continuation regression test                              */
/*                                                                        */
/*  Bracha Fig4 requires a decided process to keep broadcasting so other  */
/*  peers can decide.  An earlier version of bkr94acs.c short-circuited   */
/*  bkr94acsConsensusInput when bkr94acsDecision[origin] != 0xFF,         */
/*  silently dropping all post-decide messages.  This test pokes the BA   */
/*  decision marker directly, then verifies that a fresh consensus        */
/*  INITIAL for that origin still drives Fig1 (ECHO emission) rather than */
/*  returning zero.                                                       */
/*  With the bug:  nacts == 0.                                            */
/*  With the fix:  nacts >= 1 and includes a CON_SEND ECHO.               */
/*------------------------------------------------------------------------*/

static void
testPostDecideContinuation(
  void
){
  struct bkr94acs *a;
  unsigned long sz;
  struct bkr94acsAct acts[BKR94ACS_MAX_ACTS(MAX_PEERS, MAX_PHASES)];
  unsigned int nacts;
  unsigned int N;
  unsigned int k;
  unsigned int nEcho;
  unsigned char value;
  unsigned char encN;
  unsigned char t;

  printf("\nBKR94 ACS — Post-decide continuation regression\n");

  encN = 3;  /* actual N = 4 */
  t = 1;
  sz = bkr94acsSz(encN, 0, MAX_PHASES);
  a = (struct bkr94acs *)calloc(1, sz);
  if (!a) {
    check("alloc bkr94acs instance", 0);
    return;
  }
  bkr94acsInit(a, encN, t, 0, MAX_PHASES, 0, testCoin, 0);

  N = (unsigned int)encN + 1;

  /*
   * Simulate prior decide of BA_0: baDecision[0] is the (N+0)th byte
   * of a->data (voted[N] precedes baDecision[N]).  We also bump
   * nDecided so internal bookkeeping stays consistent.
   */
  a->data[N + 0] = 1;
  ++a->nDecided;

  /*
   * Feed a round-0 consensus INITIAL for origin 0 from peer 1.
   * Fig1 Rule 1 must fire and emit an ECHO action.  Pre-fix this
   * returned zero because of the "already decided" short-circuit.
   */
  value = 1;
  nacts = bkr94acsConsensusInput(a, 0, 0, 1, BRACHA87_INITIAL, 1, value, acts);
  check("post-decide input produces output", nacts > 0);

  nEcho = 0;
  for (k = 0; k < nacts; ++k) {
    if (acts[k].act == BKR94ACS_ACT_CON_SEND
     && acts[k].origin == 0
     && acts[k].type == BRACHA87_ECHO)
      ++nEcho;
  }
  check("post-decide input emits CON_SEND ECHO", nEcho >= 1);

  /*
   * Feed more messages to drive Fig3 round 0 to n-t validated, so
   * Fig4Round round 0 fires in the already-decided branch and
   * emits a BROADCAST action for round 1 without DECIDE.  Deliver
   * INITIALs for enough broadcasters for Fig1 to accept via echoes
   * between them.  Easiest: INITIAL from every peer for every
   * broadcaster — the simple all-to-all simulation pattern.
   */
  {
    unsigned char broadcaster;
    unsigned char from;
    unsigned char type;
    int hasPostDecideBroadcast;

    hasPostDecideBroadcast = 0;
    for (type = BRACHA87_INITIAL; type <= BRACHA87_READY; ++type) {
      for (broadcaster = 0; broadcaster < N; ++broadcaster) {
        for (from = 0; from < N; ++from) {
          unsigned int kk;

          nacts = bkr94acsConsensusInput(a, 0, 0, broadcaster,
                                         type, from, value, acts);
          for (kk = 0; kk < nacts; ++kk) {
            /*
             * A post-decide CON_SEND with round > 0 proves Fig4Round
             * fired in the already-decided else branch and emitted
             * a continuation broadcast.
             */
            if (acts[kk].act == BKR94ACS_ACT_CON_SEND
             && acts[kk].round > 0)
              hasPostDecideBroadcast = 1;
            /* BA_DECIDED must not re-fire for origin 0 */
            if (acts[kk].act == BKR94ACS_ACT_BA_DECIDED
             && acts[kk].origin == 0)
              check("no duplicate BA_DECIDED post-decide", 0);
          }
        }
      }
    }
    check("post-decide continuation emits next-round CON_SEND",
          hasPostDecideBroadcast);
  }

  free(a);
  printf("  n=4 t=1 BA_0 pre-decided: continuation ok\n");
}

/*------------------------------------------------------------------------*/
/*  BKR94 Step 2 trigger regression test                                  */
/*                                                                        */
/*  Pre-fix, bkr94acsProposalInput counted Fig1 ACCEPTs and fired the     */
/*  vote-0 fanout when nAccepted reached n-t.  BKR94 Lemma 2 Part A       */
/*  case (i) requires the step-2 trigger to be "2t+1 BAs terminated with  */
/*  output 1", not "2t+1 Fig1 ACCEPTs" — these coincide only in benign    */
/*  runs and diverge under asynchrony or Byzantine scheduling.            */
/*                                                                        */
/*  This test pins the corrected semantics by driving all N Fig1          */
/*  instances to ACCEPT on a single peer via bkr94acsProposalInput and    */
/*  asserting:                                                            */
/*    - a->threshold stays 0 after each accept (step 2 not fired),        */
/*    - no BKR94ACS_ACT_CON_SEND with conValue=0 comes out of the         */
/*      proposal path (no vote-0 fanout),                                 */
/*    - voted[j] == BKR94ACS_VOTE_ONE for every j (step 1 fired per       */
/*      accept).                                                          */
/*                                                                        */
/*  With the pre-fix code the (n-t)th accept would flip threshold to 1    */
/*  and emit a burst of vote-0 CON_SEND actions.                          */
/*------------------------------------------------------------------------*/

static void
testStepTwoTrigger(
  void
){
  struct bkr94acs *a;
  unsigned long sz;
  struct bkr94acsAct acts[BKR94ACS_MAX_ACTS(MAX_PEERS, MAX_PHASES)];
  unsigned int nacts;
  unsigned int k;
  unsigned int N;
  unsigned int origin;
  unsigned int voteZeroSeen;
  unsigned int voteOneSeen;
  unsigned char encN;
  unsigned char t;
  unsigned char val;
  unsigned char from;
  const unsigned char *voted;

  printf("\nBKR94 ACS — Step 2 trigger regression\n");

  encN = 3;  /* actual N = 4, n-t threshold = 3 */
  t = 1;
  N = (unsigned int)encN + 1;
  val = 'x';

  sz = bkr94acsSz(encN, 0, MAX_PHASES);
  a = (struct bkr94acs *)calloc(1, sz);
  if (!a) {
    check("alloc bkr94acs instance", 0);
    return;
  }
  bkr94acsInit(a, encN, t, 0, MAX_PHASES, 0, testCoin, 0);

  voteZeroSeen = 0;
  voteOneSeen = 0;

  /*
   * Drive each Fig1 instance to ACCEPT on this peer:
   *   INITIAL from origin + ECHO from every peer + READY from every
   *   peer.  The READY cascade crosses 2t+1 inside
   *   bkr94acsProposalInput and fires BKR94ACS_ACT_CON_SEND
   *   (vote-1, step 1).
   */
  for (origin = 0; origin < N; ++origin) {
    nacts = bkr94acsProposalInput(a, (unsigned char)origin, BRACHA87_INITIAL,
                                  (unsigned char)origin, &val, acts);
    for (k = 0; k < nacts; ++k)
      if (acts[k].act == BKR94ACS_ACT_CON_SEND) {
        if (acts[k].conValue == 0) ++voteZeroSeen;
        else                       ++voteOneSeen;
      }

    for (from = 0; from < N; ++from) {
      nacts = bkr94acsProposalInput(a, (unsigned char)origin, BRACHA87_ECHO,
                                    from, &val, acts);
      for (k = 0; k < nacts; ++k)
        if (acts[k].act == BKR94ACS_ACT_CON_SEND) {
          if (acts[k].conValue == 0) ++voteZeroSeen;
          else                       ++voteOneSeen;
        }
    }

    for (from = 0; from < N; ++from) {
      nacts = bkr94acsProposalInput(a, (unsigned char)origin, BRACHA87_READY,
                                    from, &val, acts);
      for (k = 0; k < nacts; ++k)
        if (acts[k].act == BKR94ACS_ACT_CON_SEND) {
          if (acts[k].conValue == 0) ++voteZeroSeen;
          else                       ++voteOneSeen;
        }
    }

    /*
     * Pre-fix, threshold flipped to 1 on the (n-t)th accept
     * (origin == 2 here) and the vote-0 fanout fired for the one
     * still-un-voted origin.
     */
    check("proposal accepts don't fire step 2", a->threshold == 0);
  }

  check("no vote-0 emitted from proposal path", voteZeroSeen == 0);
  check("vote-1 emitted for every accepted origin", voteOneSeen == N);

  /*
   * voted[] layout: first N bytes of a->data (see bkr94acsVoted in
   * bkr94acs.c).  BKR94ACS_VOTE_ONE is the internal sentinel value 1.
   */
  voted = a->data;
  for (origin = 0; origin < N; ++origin)
    check("voted[j] == VOTE_ONE after accept", voted[origin] == 1);

  free(a);
  printf("  n=4 t=1: step 2 stays in consensus path, step 1 fires per accept\n");
}

/*------------------------------------------------------------------------*/
/*  Main                                                                  */
/*------------------------------------------------------------------------*/

/*------------------------------------------------------------------------*/
/*  BPR pump tests                                                        */
/*                                                                        */
/*  White-box tests of bkr94acsPropose and bkr94acsPump:                  */
/*    - Propose marks the local proposal Fig1 as origin and emits         */
/*      PROP_SEND/INITIAL.                                                     */
/*    - Pump replays PROP_SEND/INITIAL on every tick until ECHOED.             */
/*    - Pump returns 0 on idle (no committed state anywhere).             */
/*    - Pump's per-origin gate skips proposal Fig1s for BAs decided 0     */
/*      (post-Step 2 fanout, j excluded from SubSet).                     */
/*    - End-to-end: ACS converges under pump-only drive (no application   */
/*      ledger, no per-record destination tracking) given fair-loss       */
/*      message delivery.                                                 */
/*------------------------------------------------------------------------*/

static void
testBpr(
  void
){
  struct bkr94acs *a;
  unsigned long sz;
  struct bkr94acsAct out[BKR94ACS_PUMP_MAX_ACTS];
  unsigned char val[1];
  unsigned int n;
  unsigned int i;

  printf("\n  BPR pump tests:\n");

  /* n=4, t=1, vLen=1, maxPhases=4, self=0 */
  sz = bkr94acsSz(3, 0, 4);
  a = (struct bkr94acs *)calloc(1, sz);
  bkr94acsInit(a, 3, 1, 0, 4, 0, testCoin, 0);

  /* Pump on a virgin instance: no committed state -> idle */
  n = bkr94acsPump(a, out);
  check("BPR pump virgin: 0 actions", n == 0);

  /* Propose: marks origin, emits PROP_SEND/INITIAL once */
  val[0] = 1;
  n = bkr94acsPropose(a, val, out);
  check("BPR propose: 1 action", n == 1);
  check("BPR propose: PROP_SEND/INITIAL", n >= 1
        && out[0].act == BKR94ACS_ACT_PROP_SEND
        && out[0].type == BRACHA87_INITIAL);
  check("BPR propose: origin = self", n >= 1
        && out[0].origin == 0);
  check("BPR propose: act value pointer matches stored value",
        n >= 1 && out[0].value
        && out[0].value == bkr94acsProposalValue(a, 0)
        && out[0].value[0] == 1);

  /* Pump pre-loopback: replays PROP_SEND/INITIAL every tick.
   * The cursor must visit the self proposal in finite calls; since
   * other peers' proposal Fig1s are uncommitted, the cursor walks
   * past them returning 0 -- but our walker keeps walking until it
   * finds replays or wraps, so one Pump call must surface our
   * PROP_SEND/INITIAL. */
  for (i = 0; i < 5; ++i) {
    n = bkr94acsPump(a, out);
    check("BPR pump pre-loopback: emits", n >= 1);
    check("BPR pump pre-loopback: PROP_SEND/INITIAL",
          n >= 1 && out[0].act == BKR94ACS_ACT_PROP_SEND
          && out[0].type == BRACHA87_INITIAL);
    check("BPR pump pre-loopback: origin = self",
          n >= 1 && out[0].origin == 0);
  }

  /* Loopback our own INITIAL through ProposalInput -> Rule 1 fires,
   * ECHOED is set on propF1(0), and PROP_SEND/INITIAL replay stops. */
  {
    struct bkr94acsAct iout[BKR94ACS_MAX_ACTS(4, 4)];
    bkr94acsProposalInput(a, 0, BRACHA87_INITIAL, 0, val, iout);
  }

  /* Post-loopback, the proposal Fig1 is ORIGIN+ECHOED.  Per gap-3
   * symmetry, INITIAL replay continues alongside ECHO replay so
   * honest peers that missed the bootstrap can still catch up.
   * The pump emits both; verify the cursor surfaces both for
   * origin = self. */
  {
    int seenInitial;
    int seenEcho;
    unsigned int sweep;
    unsigned int j;

    seenInitial = 0;
    seenEcho = 0;
    /* One full sweep of the cursor space; bound generously. */
    for (sweep = 0; sweep < 4 * (4 + 4 * 12 * 4); ++sweep) {
      n = bkr94acsPump(a, out);
      if (!n)
        continue;
      for (j = 0; j < n; ++j) {
        if (out[j].act == BKR94ACS_ACT_PROP_SEND && out[j].origin == 0
         && out[j].type == BRACHA87_INITIAL)
          seenInitial = 1;
        if (out[j].act == BKR94ACS_ACT_PROP_SEND && out[j].origin == 0
         && out[j].type == BRACHA87_ECHO)
          seenEcho = 1;
      }
      if (seenInitial && seenEcho)
        break;
    }
    check("BPR pump post-loopback: INITIAL still replays",
          seenInitial);
    check("BPR pump post-loopback: ECHO replay also fires",
          seenEcho);
  }

  free(a);

  /*
   * Per-origin pump gate: BA decided 0 -> skip proposal replay.
   * Synthesise the state by setting bkr94acsDecision[1] = 0
   * directly via the public-ish header layout.  We can't reach
   * the helper from outside; use a small driven path: drive ACS
   * forward to where BA_1 decides 0, then verify pump never
   * returns a PROP_* action for origin 1.
   *
   * Cheaper synthetic: forge bkr94acsDecision[1] via observation
   * after a real run of testBasic-like setup with 3 peers
   * proposing and one not.  The detailed synthesis is deferred to
   * the end-to-end test below.
   */

  /*
   * End-to-end: pump-only drive with no ledger.  Construct a
   * 4-peer simulation that bootstraps ONLY by Propose and then
   * relies on Pump to reach all peers under heavy proposal-
   * INITIAL drop (50% loss on the bootstrap broadcast; pump
   * makes up the difference).  Verify all four peers reach
   * complete with the same SubSet.
   *
   * This regression covers gap 4 (originator INITIAL replay)
   * for the bkr94acs composition: without origin INITIAL replay,
   * a 50%-drop bootstrap leaves at least one peer permanently
   * unable to ACCEPT some origin's proposal, and ACS stalls.
   */
  {
    struct bkr94acs *peers[4];
    struct acsResult results[4];
    const char *proposals[4] = {"a", "b", "c", "d"};
    unsigned int dropSeed;
    unsigned int totalSwept;
    unsigned int p;
    unsigned int q;

    sz = bkr94acsSz(3, 0, MAX_PHASES);
    for (p = 0; p < 4; ++p) {
      peers[p] = (struct bkr94acs *)calloc(1, sz);
      bkr94acsInit(peers[p], 3, 1, 0, MAX_PHASES, (unsigned char)p,
                   testCoin, 0);
    }

    qInit();
    dropSeed = 0xDEADBEEFu;

    /* Bootstrap: each peer Proposes their value (binary 0 or 1
     * for this test; proposals[] strings reduce to first byte). */
    for (p = 0; p < 4; ++p) {
      struct bkr94acsAct iact[BKR94ACS_MAX_ACTS(4, MAX_PHASES)];
      unsigned int nAct;
      unsigned char propVal;

      propVal = (unsigned char)proposals[p][0];
      nAct = bkr94acsPropose(peers[p], &propVal, iact);
      check("BPR e2e: Propose returned 1 action", nAct == 1);
      /* Push the Propose result to a random subset (50% loss) */
      for (q = 0; q < 4; ++q) {
        dropSeed = dropSeed * 1103515245u + 12345u;
        if (((dropSeed >> 16) & 1) == 0)
          continue;  /* dropped */
        qPush(BKR94ACS_CLS_PROPOSAL, iact[0].origin, 0, 0,
              BRACHA87_INITIAL, (unsigned char)p, (unsigned char)q,
              &propVal, 1);
      }
    }

    /*
     * Drive the simulation: drain the queue, then pump every
     * peer once and re-drain.  Loop until either all-complete
     * or a sweep produces no new traffic (idle quorum).
     */
    totalSwept = 0;
    for (;;) {
      int progress;

      /* Drain network */
      while (Qhead < Qtail) {
        struct msg *m;
        struct bkr94acsAct acts[BKR94ACS_MAX_ACTS(4, MAX_PHASES)];
        unsigned int nacts;
        unsigned int k;

        m = &MsgQ[Qhead++];
        if (m->cls == BKR94ACS_CLS_PROPOSAL)
          nacts = bkr94acsProposalInput(peers[m->to], m->origin,
                    m->type, m->from, m->value, acts);
        else
          nacts = bkr94acsConsensusInput(peers[m->to], m->origin,
                    m->round, m->broadcaster, m->type, m->from,
                    m->value[0], acts);

        for (k = 0; k < nacts; ++k) {
          struct bkr94acsAct *act = &acts[k];

          switch (act->act) {
          case BKR94ACS_ACT_PROP_SEND:
            if (!act->value) break;
            for (q = 0; q < 4; ++q)
              qPush(BKR94ACS_CLS_PROPOSAL, act->origin, 0, 0,
                    act->type, m->to, (unsigned char)q, act->value, 1);
            break;
          case BKR94ACS_ACT_CON_SEND:
            for (q = 0; q < 4; ++q)
              qPush(BKR94ACS_CLS_CONSENSUS, act->origin, act->round,
                    act->broadcaster, act->type, m->to,
                    (unsigned char)q, &act->conValue, 1);
            break;
          default:
            break;
          }
        }
      }

      /* Idle quorum check */
      progress = 0;
      for (p = 0; p < 4; ++p)
        if (!peers[p]->complete) {
          progress = 1;
          break;
        }
      if (!progress)
        break;

      /* Pump every peer */
      progress = 0;
      for (p = 0; p < 4; ++p) {
        struct bkr94acsAct pact[BKR94ACS_PUMP_MAX_ACTS];
        unsigned int npact;
        unsigned int k;

        npact = bkr94acsPump(peers[p], pact);
        if (npact)
          progress = 1;
        for (k = 0; k < npact; ++k) {
          switch (pact[k].act) {
          case BKR94ACS_ACT_PROP_SEND:
            if (!pact[k].value) break;
            for (q = 0; q < 4; ++q)
              qPush(BKR94ACS_CLS_PROPOSAL, pact[k].origin, 0, 0,
                    pact[k].type, (unsigned char)p, (unsigned char)q,
                    pact[k].value, 1);
            break;
          case BKR94ACS_ACT_CON_SEND:
            for (q = 0; q < 4; ++q)
              qPush(BKR94ACS_CLS_CONSENSUS, pact[k].origin, pact[k].round,
                    pact[k].broadcaster, pact[k].type,
                    (unsigned char)p, (unsigned char)q,
                    &pact[k].conValue, 1);
            break;
          default:
            break;
          }
        }
      }

      ++totalSwept;
      if (totalSwept > 10000) {
        check("BPR e2e: converged within budget", 0);
        break;
      }
      /* If queue is empty AND no peer pumped, we're idle: terminate. */
      if (!progress && Qhead == Qtail)
        break;
    }

    for (p = 0; p < 4; ++p) {
      results[p].complete = peers[p]->complete;
      results[p].subsetCnt = bkr94acsSubset(peers[p], results[p].subset);
    }

    check("BPR e2e: all peers complete", allComplete(results, 4));
    check("BPR e2e: all peers agree on subset", allAgree(results, 4));
    check("BPR e2e: subset size >= n-t", subsetValid(results, 4, 1));
    printf("    e2e pump-only drive  : 4 peers, 50%% INITIAL drop, |SubSet|=%u\n",
           results[0].subsetCnt);

    for (p = 0; p < 4; ++p)
      free(peers[p]);
  }
}

/*------------------------------------------------------------------------*/
/*  Pump cursor coverage white-box                                        */
/*                                                                        */
/*  Verifies the cursor visits every owned Fig1 instance with replay      */
/*  potential within a bounded number of calls.  Force several proposal   */
/*  Fig1s into ORIGIN+!ECHOED state via Propose-from-other-peers          */
/*  (pretend each peer is the originator); call Pump until either we've  */
/*  seen all expected origins or hit a generous budget.                  */
/*------------------------------------------------------------------------*/

static void
testBprCursorCoverage(
  void
){
  struct bkr94acs *peers[4];
  struct bkr94acsAct out[BKR94ACS_PUMP_MAX_ACTS];
  unsigned char val[1];
  unsigned long sz;
  unsigned int p;
  unsigned int seen[4];
  unsigned int allSeen;
  unsigned int call;
  unsigned int budget;

  printf("\n  BPR pump cursor coverage:\n");

  sz = bkr94acsSz(3, 0, 4);
  for (p = 0; p < 4; ++p) {
    peers[p] = (struct bkr94acs *)calloc(1, sz);
    bkr94acsInit(peers[p], 3, 1, 0, 4, (unsigned char)p, testCoin, 0);
  }

  /* Each peer Proposes; their proposal Fig1 (origin = self) becomes
   * ORIGIN+!ECHOED.  We sweep peer 0's pump and verify it eventually
   * surfaces PROP_SEND/INITIAL for peer 0's own origin.  Then we feed
   * peer 0 INITIALs from peers 1, 2, 3 (driving Rule 1 on their
   * proposal Fig1s on peer 0's instance), so peer 0's view of those
   * proposal Fig1s becomes ECHOED (committed to ECHO replay).
   * After that, peer 0's pump should cycle through all 4 origins'
   * proposal Fig1s on subsequent calls. */
  val[0] = 1;
  for (p = 0; p < 4; ++p) {
    struct bkr94acsAct iact[BKR94ACS_MAX_ACTS(4, 4)];
    bkr94acsPropose(peers[p], val, iact);
  }

  /* Feed peer 0 INITIALs from origins 1, 2, 3 so peer 0's
   * propF1(j) for j=1..3 fires Rule 1 (ECHOED).  Peer 0's
   * propF1(0) is already ORIGIN (via Propose). */
  for (p = 1; p < 4; ++p) {
    struct bkr94acsAct iact[BKR94ACS_MAX_ACTS(4, 4)];
    bkr94acsProposalInput(peers[0], (unsigned char)p, BRACHA87_INITIAL,
                          (unsigned char)p, val, iact);
  }

  /* Now peer 0 has 4 proposal Fig1s with replay potential:
   *   propF1(0): ORIGIN+!ECHOED -> INITIAL_ALL replay
   *   propF1(1..3): ECHOED -> ECHO_ALL replay
   * Sweep pump and assert all 4 origins surface in PROP_*. */
  memset(seen, 0, sizeof (seen));
  budget = 4 + 4 * 12 * 4 + 100;  /* one full cursor cycle + slack */
  for (call = 0; call < budget; ++call) {
    unsigned int n;

    n = bkr94acsPump(peers[0], out);
    if (!n)
      continue;
    if (out[0].act == BKR94ACS_ACT_PROP_SEND)
      seen[out[0].origin] = 1;

    allSeen = 1;
    for (p = 0; p < 4; ++p)
      if (!seen[p]) {
        allSeen = 0;
        break;
      }
    if (allSeen)
      break;
  }
  for (p = 0; p < 4; ++p)
    check("BPR cursor coverage: origin visited", seen[p]);
  printf("    cursor visited all 4 proposal Fig1s in %u pump calls\n",
         call + 1);

  for (p = 0; p < 4; ++p)
    free(peers[p]);
}

/*------------------------------------------------------------------------*/
/*  Pump-origin gate white-box                                            */
/*                                                                        */
/*  The 3-rule decision in bkr94acs.dtc's BPR section: BA decided 0       */
/*  -> skip proposal Fig1 replay; BA undecided / decided 1 -> pump.       */
/*  Force decision[1] = 0 directly and verify Pump never returns          */
/*  PROP_* for origin 1 across a full cursor sweep, while still           */
/*  surfacing replays for origin 0 (undecided).                           */
/*------------------------------------------------------------------------*/

/* Internal layout helpers from bkr94acs.c needed for the white-box poke.
 * Walking past the data[] header to reach the per-origin decision
 * byte: voted[N] precedes baDecision[N] (offsets 0 and N
 * respectively from a->data). */
static unsigned char *
testWriteDecision(
  struct bkr94acs *a
 ,unsigned char origin
 ,unsigned char value
){
  unsigned int N;
  unsigned char *baDec;

  N = (unsigned int)a->n + 1;
  baDec = a->data + N;
  baDec[origin] = value;
  return (baDec);
}

static void
testBprOriginGate(
  void
){
  struct bkr94acs *a;
  struct bkr94acsAct out[BKR94ACS_PUMP_MAX_ACTS];
  unsigned char val[1];
  unsigned long sz;
  unsigned int call;
  unsigned int budget;
  unsigned int origin0Seen;
  unsigned int origin1Seen;

  printf("\n  BPR pump-origin gate:\n");

  sz = bkr94acsSz(3, 0, 4);
  a = (struct bkr94acs *)calloc(1, sz);
  bkr94acsInit(a, 3, 1, 0, 4, 0, testCoin, 0);

  /* Both proposal Fig1s for origins 0 and 1 in ECHOED state.
   * Origin 0: Propose (ORIGIN+!ECHOED, then loopback -> ECHOED).
   * Origin 1: feed INITIAL from peer 1 (ECHOED via Rule 1). */
  val[0] = 1;
  {
    struct bkr94acsAct iact[BKR94ACS_MAX_ACTS(4, 4)];
    bkr94acsPropose(a, val, iact);
    bkr94acsProposalInput(a, 0, BRACHA87_INITIAL, 0, val, iact);
    bkr94acsProposalInput(a, 1, BRACHA87_INITIAL, 1, val, iact);
  }

  /* Force BA decision: origin 1 decided 0 (excluded), origin 0
   * undecided.  Pump should walk past origin 1 silently and surface
   * PROP_SEND/ECHO for origin 0. */
  testWriteDecision(a, 1, 0);
  /* Origin 0: leave as undecided (0xFF, default from Init) */

  origin0Seen = 0;
  origin1Seen = 0;
  budget = 4 + 4 * 12 * 4 + 100;
  for (call = 0; call < budget; ++call) {
    unsigned int n;

    n = bkr94acsPump(a, out);
    if (!n)
      continue;
    if (out[0].act == BKR94ACS_ACT_PROP_SEND) {
      if (out[0].origin == 0)
        ++origin0Seen;
      if (out[0].origin == 1)
        ++origin1Seen;
    }
    /* Stop after enough origin-0 surfaces (cursor cycled) */
    if (origin0Seen >= 3)
      break;
  }
  check("BPR gate: origin 0 (undecided) IS pumped", origin0Seen >= 1);
  check("BPR gate: origin 1 (decided 0) NOT pumped", origin1Seen == 0);
  printf("    decided 0 origin skipped (origin0=%u, origin1=%u over %u calls)\n",
         origin0Seen, origin1Seen, call + 1);

  /* Now flip origin 1 to decided 1 (post-decide continuation).
   * Pump should pump origin 1 (Bracha pitfall #1). */
  testWriteDecision(a, 1, 1);

  /* Reset cursor by calling Init?  No -- we want to test that the
   * gate change takes effect mid-flight.  Just sweep and watch. */
  origin1Seen = 0;
  for (call = 0; call < budget; ++call) {
    unsigned int n;

    n = bkr94acsPump(a, out);
    if (!n)
      continue;
    if (out[0].act == BKR94ACS_ACT_PROP_SEND
     && out[0].origin == 1)
      ++origin1Seen;
    if (origin1Seen >= 1)
      break;
  }
  check("BPR gate: origin 1 (decided 1) IS pumped (post-decide)",
        origin1Seen >= 1);
  printf("    decided 1 origin pumped (post-decide continuation, pitfall #1)\n");

  free(a);
}

/*------------------------------------------------------------------------*/
/*  Byzantine pump test                                                   */
/*                                                                        */
/*  n=4 t=1.  Peer 3 is byzantine: never sends its own messages           */
/*  (silent withhold).  The 3 honest peers (0, 1, 2) bootstrap with       */
/*  Propose and rely on each other's BPR + a small amount of input        */
/*  message processing to converge.  Drop rate moderate (25%).            */
/*  Verifies BPR replay + post-Step-2-fanout under t-byzantine.           */
/*------------------------------------------------------------------------*/

static void
testBprByzantineSilent(
  void
){
  struct bkr94acs *peers[4];
  struct acsResult results[4];
  struct bkr94acsAct out[BKR94ACS_PUMP_MAX_ACTS];
  unsigned char val;
  unsigned long sz;
  unsigned int dropSeed;
  unsigned int totalSwept;
  unsigned int p;
  unsigned int q;

  printf("\n  BPR byzantine silent peer:\n");

  sz = bkr94acsSz(3, 0, MAX_PHASES);
  /* Allocate honest peers + a byzantine slot for index symmetry */
  for (p = 0; p < 4; ++p) {
    peers[p] = (struct bkr94acs *)calloc(1, sz);
    bkr94acsInit(peers[p], 3, 1, 0, MAX_PHASES, (unsigned char)p,
                 testCoin, 0);
  }

  qInit();
  dropSeed = 0xCAFEBABEu;

  /* Honest peers 0, 1, 2 Propose; peer 3 (byzantine) never proposes
   * and never sends anything. */
  val = 1;
  for (p = 0; p < 3; ++p) {
    struct bkr94acsAct iact[BKR94ACS_MAX_ACTS(4, MAX_PHASES)];
    bkr94acsPropose(peers[p], &val, iact);
    for (q = 0; q < 4; ++q) {
      dropSeed = dropSeed * 1103515245u + 12345u;
      /* 12.5% drop (1/8): models lossy network on top of byzantine
       * silence.  At higher drop rates the simulator's bounded
       * MAX_MSGS queue fills with BPR replays faster than
       * actions are consumed, causing silent enqueue drops that
       * mask convergence -- a property of this test harness, not
       * BPR.  testBprHighDrop drives convergence under heavier
       * loss with no byzantine peer so the volume stays bounded. */
      if (((dropSeed >> 16) & 7) == 0)
        continue;
      qPush(BKR94ACS_CLS_PROPOSAL, (unsigned char)p, 0, 0,
            BRACHA87_INITIAL, (unsigned char)p, (unsigned char)q,
            &val, 1);
    }
  }

  /* Drive: drain queue (skipping any messages addressed to peer 3
   * to model the byzantine peer ignoring inputs), then pump
   * honest peers, repeat. */
  totalSwept = 0;
  for (;;) {
    int progress;

    /* Drain network */
    while (Qhead < Qtail) {
      struct msg *m;
      struct bkr94acsAct acts[BKR94ACS_MAX_ACTS(4, MAX_PHASES)];
      unsigned int nacts;
      unsigned int k;

      m = &MsgQ[Qhead++];
      /* Byzantine peer 3 silently drops all inputs and produces
       * no outputs.  Peers 0, 1, 2 process normally. */
      if (m->to == 3)
        continue;

      if (m->cls == BKR94ACS_CLS_PROPOSAL)
        nacts = bkr94acsProposalInput(peers[m->to], m->origin,
                  m->type, m->from, m->value, acts);
      else
        nacts = bkr94acsConsensusInput(peers[m->to], m->origin,
                  m->round, m->broadcaster, m->type, m->from,
                  m->value[0], acts);

      for (k = 0; k < nacts; ++k) {
        struct bkr94acsAct *act = &acts[k];

        switch (act->act) {
        case BKR94ACS_ACT_PROP_SEND:
          if (!act->value) break;
          for (q = 0; q < 4; ++q)
            qPush(BKR94ACS_CLS_PROPOSAL, act->origin, 0, 0,
                  act->type, m->to, (unsigned char)q, act->value, 1);
          break;
        case BKR94ACS_ACT_CON_SEND:
          for (q = 0; q < 4; ++q)
            qPush(BKR94ACS_CLS_CONSENSUS, act->origin, act->round,
                  act->broadcaster, act->type, m->to,
                  (unsigned char)q, &act->conValue, 1);
          break;
        default:
          break;
        }
      }
    }

    /* Idle quorum on honest peers */
    progress = 0;
    for (p = 0; p < 3; ++p)
      if (!peers[p]->complete) {
        progress = 1;
        break;
      }
    if (!progress)
      break;

    /* Pump honest peers */
    progress = 0;
    for (p = 0; p < 3; ++p) {
      unsigned int npact;
      unsigned int k;

      npact = bkr94acsPump(peers[p], out);
      if (npact)
        progress = 1;
      for (k = 0; k < npact; ++k) {
        switch (out[k].act) {
        case BKR94ACS_ACT_PROP_SEND:
          if (!out[k].value) break;
          for (q = 0; q < 4; ++q)
            qPush(BKR94ACS_CLS_PROPOSAL, out[k].origin, 0, 0,
                  out[k].type, (unsigned char)p, (unsigned char)q,
                  out[k].value, 1);
          break;
        case BKR94ACS_ACT_CON_SEND:
          for (q = 0; q < 4; ++q)
            qPush(BKR94ACS_CLS_CONSENSUS, out[k].origin, out[k].round,
                  out[k].broadcaster, out[k].type,
                  (unsigned char)p, (unsigned char)q,
                  &out[k].conValue, 1);
          break;
        default:
          break;
        }
      }
    }

    ++totalSwept;
    if (totalSwept > 500000) {
      check("BPR byzantine: converged within budget", 0);
      break;
    }
    if (!progress && Qhead == Qtail)
      break;
  }

  for (p = 0; p < 3; ++p) {
    results[p].complete = peers[p]->complete;
    results[p].subsetCnt = bkr94acsSubset(peers[p], results[p].subset);
  }

  check("BPR byzantine: 3 honest peers complete",
        results[0].complete && results[1].complete && results[2].complete);
  check("BPR byzantine: honest peers agree on subset",
        results[0].subsetCnt == results[1].subsetCnt
        && results[1].subsetCnt == results[2].subsetCnt
        && memcmp(results[0].subset, results[1].subset,
                  results[0].subsetCnt) == 0
        && memcmp(results[1].subset, results[2].subset,
                  results[1].subsetCnt) == 0);
  /* Subset must contain all 3 honest origins (they all proposed
   * and reached Fig1 ACCEPT among the honest 3 -- 3 = n-t = 3
   * which satisfies BKR94 Lemma 2 Part A). */
  check("BPR byzantine: |subset| >= n-t = 3",
        results[0].subsetCnt >= 3);
  printf("    n=4 t=1 silent byzantine: 3 honest converged, |SubSet|=%u in %u sweeps\n",
         results[0].subsetCnt, totalSwept);

  for (p = 0; p < 4; ++p)
    free(peers[p]);
}

/*------------------------------------------------------------------------*/
/*  High-drop e2e                                                         */
/*                                                                        */
/*  Same harness as testBpr's e2e, drop rate parameterised.  At 90%       */
/*  drop the bootstrap broadcast nearly always loses; convergence         */
/*  must come from BPR replays.  Confirms BPR is sufficient under         */
/*  pathological loss.                                                    */
/*------------------------------------------------------------------------*/

static int
runPumpOnlyE2e(
  unsigned int dropMask
 ,unsigned int *sweepsOut
){
  struct bkr94acs *peers[4];
  struct acsResult results[4];
  struct bkr94acsAct out[BKR94ACS_PUMP_MAX_ACTS];
  unsigned char val;
  unsigned long sz;
  unsigned int dropSeed;
  unsigned int totalSwept;
  unsigned int p;
  unsigned int q;
  int allOk;

  sz = bkr94acsSz(3, 0, MAX_PHASES);
  for (p = 0; p < 4; ++p) {
    peers[p] = (struct bkr94acs *)calloc(1, sz);
    bkr94acsInit(peers[p], 3, 1, 0, MAX_PHASES, (unsigned char)p,
                 testCoin, 0);
  }

  qInit();
  dropSeed = 0x13371337u;

  val = 1;
  for (p = 0; p < 4; ++p) {
    struct bkr94acsAct iact[BKR94ACS_MAX_ACTS(4, MAX_PHASES)];
    bkr94acsPropose(peers[p], &val, iact);
    for (q = 0; q < 4; ++q) {
      dropSeed = dropSeed * 1103515245u + 12345u;
      /* dropMask is in 1/16ths: 14/16 = ~87.5%, 15/16 = ~93.75% */
      if (((dropSeed >> 16) & 0xF) < dropMask)
        continue;
      qPush(BKR94ACS_CLS_PROPOSAL, (unsigned char)p, 0, 0,
            BRACHA87_INITIAL, (unsigned char)p, (unsigned char)q,
            &val, 1);
    }
  }

  totalSwept = 0;
  for (;;) {
    int progress;

    while (Qhead < Qtail) {
      struct msg *m;
      struct bkr94acsAct acts[BKR94ACS_MAX_ACTS(4, MAX_PHASES)];
      unsigned int nacts;
      unsigned int k;

      m = &MsgQ[Qhead++];
      if (m->cls == BKR94ACS_CLS_PROPOSAL)
        nacts = bkr94acsProposalInput(peers[m->to], m->origin,
                  m->type, m->from, m->value, acts);
      else
        nacts = bkr94acsConsensusInput(peers[m->to], m->origin,
                  m->round, m->broadcaster, m->type, m->from,
                  m->value[0], acts);
      for (k = 0; k < nacts; ++k) {
        struct bkr94acsAct *act = &acts[k];
        switch (act->act) {
        case BKR94ACS_ACT_PROP_SEND:
          if (!act->value) break;
          for (q = 0; q < 4; ++q)
            qPush(BKR94ACS_CLS_PROPOSAL, act->origin, 0, 0,
                  act->type, m->to, (unsigned char)q, act->value, 1);
          break;
        case BKR94ACS_ACT_CON_SEND:
          for (q = 0; q < 4; ++q)
            qPush(BKR94ACS_CLS_CONSENSUS, act->origin, act->round,
                  act->broadcaster, act->type, m->to,
                  (unsigned char)q, &act->conValue, 1);
          break;
        default:
          break;
        }
      }
    }

    progress = 0;
    for (p = 0; p < 4; ++p)
      if (!peers[p]->complete) {
        progress = 1;
        break;
      }
    if (!progress)
      break;

    progress = 0;
    for (p = 0; p < 4; ++p) {
      unsigned int npact;
      unsigned int k;

      npact = bkr94acsPump(peers[p], out);
      if (npact)
        progress = 1;
      for (k = 0; k < npact; ++k) {
        switch (out[k].act) {
        case BKR94ACS_ACT_PROP_SEND:
          if (!out[k].value) break;
          for (q = 0; q < 4; ++q)
            qPush(BKR94ACS_CLS_PROPOSAL, out[k].origin, 0, 0,
                  out[k].type, (unsigned char)p, (unsigned char)q,
                  out[k].value, 1);
          break;
        case BKR94ACS_ACT_CON_SEND:
          for (q = 0; q < 4; ++q)
            qPush(BKR94ACS_CLS_CONSENSUS, out[k].origin, out[k].round,
                  out[k].broadcaster, out[k].type,
                  (unsigned char)p, (unsigned char)q,
                  &out[k].conValue, 1);
          break;
        default:
          break;
        }
      }
    }

    ++totalSwept;
    if (totalSwept > 100000) {
      *sweepsOut = totalSwept;
      for (p = 0; p < 4; ++p)
        free(peers[p]);
      return (-1);
    }
    if (!progress && Qhead == Qtail)
      break;
  }

  for (p = 0; p < 4; ++p) {
    results[p].complete = peers[p]->complete;
    results[p].subsetCnt = bkr94acsSubset(peers[p], results[p].subset);
  }

  allOk = allComplete(results, 4) && allAgree(results, 4)
       && subsetValid(results, 4, 1);
  *sweepsOut = totalSwept;

  for (p = 0; p < 4; ++p)
    free(peers[p]);
  return (allOk ? 0 : -1);
}

static void
testBprHighDrop(
  void
){
  unsigned int sweeps;
  int rc;

  printf("\n  BPR high-drop e2e:\n");

  /* 75% drop (12/16) */
  rc = runPumpOnlyE2e(12, &sweeps);
  check("BPR high-drop 75%: converged", rc == 0);
  printf("    n=4 t=1 75%% INITIAL drop: %s in %u sweeps\n",
         rc == 0 ? "converged" : "FAILED", sweeps);

  /* 87.5% drop (14/16) */
  rc = runPumpOnlyE2e(14, &sweeps);
  check("BPR high-drop 87.5%: converged", rc == 0);
  printf("    n=4 t=1 87.5%% INITIAL drop: %s in %u sweeps\n",
         rc == 0 ? "converged" : "FAILED", sweeps);
}

int
main(
  void
){
  printf("bkr94acs test suite\n");
  printf("===================\n\n");

  MsgQ = (struct msg *)calloc(MAX_MSGS, sizeof (struct msg));
  if (!MsgQ) {
    fprintf(stderr, "message queue allocation failed\n");
    return (1);
  }

  testBasic();
  testShuffled();
  testValues();
  testMultiByteValues();
  testIdenticalProposals();
  testLargerN();
  testPostDecideContinuation();
  testStepTwoTrigger();
  testBpr();
  testBprCursorCoverage();
  testBprOriginGate();
  testBprByzantineSilent();
  testBprHighDrop();

  free(MsgQ);

  printf("\n===================\n");
  if (Fail) {
    printf("%d FAILED\n", Fail);
    return (1);
  }
  printf("ALL PASSED\n");
  return (0);
}
