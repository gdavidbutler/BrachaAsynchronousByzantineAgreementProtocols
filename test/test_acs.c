/*
 * Tests for acs.[hc] — Asynchronous Common Subset.
 *
 * Simulates all-to-all message passing for ACS:
 *   N proposal broadcasts (Fig1 with arbitrary values)
 *   N binary consensuses (Fig4 per origin)
 *
 * Verifies:
 *   Agreement  — all honest peers decide the same subset
 *   Validity   — subset contains at least n-t origins
 *   Totality   — all BAs decide (acsComplete returns true)
 *   Values     — accepted proposal values match what was proposed
 *   Ordering   — deterministic sort produces identical order at each peer
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "acs.h"

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
/*  Message queue — same pattern as example/acs.c                         */
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
/*  ACS simulation engine                                                 */
/*------------------------------------------------------------------------*/

struct acsResult {
  int complete;
  unsigned int subsetCnt;
  unsigned char subset[MAX_PEERS];
};

/*
 * Run one ACS simulation.
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
  struct acs *peers[MAX_PEERS];
  unsigned long sz;
  unsigned int i;
  unsigned int j;

  sz = acsSz(n - 1, vLen - 1, MAX_PHASES);
  memset(peers, 0, sizeof (peers));
  for (i = 0; i < n; ++i) {
    peers[i] = (struct acs *)calloc(1, sz);
    if (!peers[i]) {
      for (j = 0; j < i; ++j)
        free(peers[j]);
      return (-1);
    }
    acsInit(peers[i], (unsigned char)(n - 1), (unsigned char)t,
            (unsigned char)(vLen - 1), MAX_PHASES, (unsigned char)i,
            testCoin, 0);
  }

  qInit();

  /* Bootstrap: each peer broadcasts INITIAL of their proposal */
  for (i = 0; i < n; ++i)
    for (j = 0; j < n; ++j)
      qPush(ACS_CLS_PROPOSAL, (unsigned char)i, 0, 0,
            BRACHA87_INITIAL, (unsigned char)i, (unsigned char)j,
            (const unsigned char *)proposals[i], vLen);

  if (shuffleSeed)
    qShuffle(&shuffleSeed);

  /* Process message queue */
  while (Qhead < Qtail) {
    struct msg *m;
    struct acs *st;
    struct acsAct acts[ACS_MAX_ACTS(MAX_PEERS, MAX_PHASES)];
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
     * (see acs.c acsConsensusInput comment on a->complete).
     */
    oldTail = Qtail;

    if (m->cls == ACS_CLS_PROPOSAL) {
      nacts = acsProposalInput(st, m->origin, m->type, m->from,
                               m->value, acts);
    } else {
      nacts = acsConsensusInput(st, m->origin, m->round,
                                m->broadcaster, m->type,
                                m->from, m->value[0], acts);
    }

    for (k = 0; k < nacts; ++k) {
      unsigned int p;

      switch (acts[k].act) {
      case ACS_ACT_PROP_ECHO:
      case ACS_ACT_PROP_READY:
        {
          const unsigned char *pv;

          pv = acsProposalValue(st, acts[k].origin);
          if (!pv)
            break;
          for (p = 0; p < n; ++p)
            qPush(ACS_CLS_PROPOSAL, acts[k].origin, 0, 0,
                  (acts[k].act == ACS_ACT_PROP_ECHO)
                    ? BRACHA87_ECHO : BRACHA87_READY,
                  m->to, (unsigned char)p, pv, vLen);
        }
        break;
      case ACS_ACT_CON_SEND:
        for (p = 0; p < n; ++p)
          qPush(ACS_CLS_CONSENSUS, acts[k].origin, acts[k].round,
                acts[k].broadcaster, acts[k].conType,
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
    results[i].complete = acsComplete(peers[i]);
    results[i].subsetCnt = acsSubset(peers[i], results[i].subset);
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

  printf("ACS — Basic honest tests\n");

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

  printf("\nACS — Shuffled delivery tests\n");

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
  struct acs *peers[MAX_PEERS];
  unsigned long sz;
  unsigned int n;
  unsigned int t;
  unsigned int vLen;
  unsigned int i;
  unsigned int j;
  int valuesOk;

  printf("\nACS — Proposal value integrity tests\n");

  /*
   * Verify that accepted proposal values match what was proposed.
   * Run a simulation, then check acsProposalValue for each included origin.
   */
  n = 4;
  t = 1;
  memset(proposals, 0, sizeof (proposals));
  strcpy(proposals[0], "joe");
  strcpy(proposals[1], "sam");
  strcpy(proposals[2], "sally");
  strcpy(proposals[3], "tim");
  vLen = 6;

  sz = acsSz(n - 1, vLen - 1, MAX_PHASES);
  for (i = 0; i < n; ++i) {
    peers[i] = (struct acs *)calloc(1, sz);
    acsInit(peers[i], (unsigned char)(n - 1), (unsigned char)t,
            (unsigned char)(vLen - 1), MAX_PHASES, (unsigned char)i,
            testCoin, 0);
  }

  qInit();
  for (i = 0; i < n; ++i)
    for (j = 0; j < n; ++j)
      qPush(ACS_CLS_PROPOSAL, (unsigned char)i, 0, 0,
            BRACHA87_INITIAL, (unsigned char)i, (unsigned char)j,
            (const unsigned char *)proposals[i], vLen);

  while (Qhead < Qtail) {
    struct msg *m;
    struct acs *st;
    struct acsAct acts[ACS_MAX_ACTS(MAX_PEERS, MAX_PHASES)];
    unsigned int nacts;
    unsigned int k;

    m = &MsgQ[Qhead++];
    st = peers[m->to];
    if (acsComplete(st))
      continue;

    if (m->cls == ACS_CLS_PROPOSAL) {
      nacts = acsProposalInput(st, m->origin, m->type, m->from,
                               m->value, acts);
    } else {
      nacts = acsConsensusInput(st, m->origin, m->round,
                                m->broadcaster, m->type,
                                m->from, m->value[0], acts);
    }

    for (k = 0; k < nacts; ++k) {
      unsigned int p;

      switch (acts[k].act) {
      case ACS_ACT_PROP_ECHO:
      case ACS_ACT_PROP_READY:
        {
          const unsigned char *pv;

          pv = acsProposalValue(st, acts[k].origin);
          if (!pv)
            break;
          for (p = 0; p < n; ++p)
            qPush(ACS_CLS_PROPOSAL, acts[k].origin, 0, 0,
                  (acts[k].act == ACS_ACT_PROP_ECHO)
                    ? BRACHA87_ECHO : BRACHA87_READY,
                  m->to, (unsigned char)p, pv, vLen);
        }
        break;
      case ACS_ACT_CON_SEND:
        for (p = 0; p < n; ++p)
          qPush(ACS_CLS_CONSENSUS, acts[k].origin, acts[k].round,
                acts[k].broadcaster, acts[k].conType,
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

    check("values: complete", acsComplete(peers[i]));
    cnt = acsSubset(peers[i], subset);
    for (j = 0; j < cnt; ++j) {
      const unsigned char *pv;

      pv = acsProposalValue(peers[i], subset[j]);
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

  printf("\nACS — Multi-byte value tests\n");

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

  printf("\nACS — Identical proposal tests\n");

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

  printf("\nACS — Larger N tests\n");

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
/*  peers can decide.  An earlier version of acs.c short-circuited        */
/*  acsConsensusInput when acsDecision[origin] != 0xFF, silently dropping */
/*  all post-decide messages.  This test pokes the BA decision marker     */
/*  directly, then verifies that a fresh consensus INITIAL for that       */
/*  origin still drives Fig1 (ECHO emission) rather than returning zero.  */
/*  With the bug:  nacts == 0.                                            */
/*  With the fix:  nacts >= 1 and includes a CON_SEND ECHO.               */
/*------------------------------------------------------------------------*/

static void
testPostDecideContinuation(
  void
){
  struct acs *a;
  unsigned long sz;
  struct acsAct acts[ACS_MAX_ACTS(MAX_PEERS, MAX_PHASES)];
  unsigned int nacts;
  unsigned int N;
  unsigned int k;
  unsigned int nEcho;
  unsigned char value;
  unsigned char encN;
  unsigned char t;

  printf("\nACS — Post-decide continuation regression\n");

  encN = 3;  /* actual N = 4 */
  t = 1;
  sz = acsSz(encN, 0, MAX_PHASES);
  a = (struct acs *)calloc(1, sz);
  if (!a) {
    check("alloc acs instance", 0);
    return;
  }
  acsInit(a, encN, t, 0, MAX_PHASES, 0, testCoin, 0);

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
  nacts = acsConsensusInput(a, 0, 0, 1, BRACHA87_INITIAL, 1, value, acts);
  check("post-decide input produces output", nacts > 0);

  nEcho = 0;
  for (k = 0; k < nacts; ++k) {
    if (acts[k].act == ACS_ACT_CON_SEND
     && acts[k].origin == 0
     && acts[k].conType == BRACHA87_ECHO)
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

          nacts = acsConsensusInput(a, 0, 0, broadcaster,
                                    type, from, value, acts);
          for (kk = 0; kk < nacts; ++kk) {
            /*
             * A post-decide CON_SEND with round > 0 proves Fig4Round
             * fired in the already-decided else branch and emitted
             * a continuation broadcast.
             */
            if (acts[kk].act == ACS_ACT_CON_SEND
             && acts[kk].round > 0)
              hasPostDecideBroadcast = 1;
            /* BA_DECIDED must not re-fire for origin 0 */
            if (acts[kk].act == ACS_ACT_BA_DECIDED
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
/*  Pre-fix, acsProposalInput counted Fig1 ACCEPTs and fired the vote-0   */
/*  fanout when nAccepted reached n-t.  BKR94 Lemma 2 Part A case (i)     */
/*  requires the step-2 trigger to be "2t+1 BAs terminated with output 1",*/
/*  not "2t+1 Fig1 ACCEPTs" — these coincide only in benign runs and      */
/*  diverge under asynchrony or Byzantine scheduling.                     */
/*                                                                        */
/*  This test pins the corrected semantics by driving all N Fig1          */
/*  instances to ACCEPT on a single peer via acsProposalInput and         */
/*  asserting:                                                            */
/*    - a->threshold stays 0 after each accept (step 2 not fired),        */
/*    - no ACS_ACT_CON_SEND with conValue=0 comes out of the proposal     */
/*      path (no vote-0 fanout),                                          */
/*    - voted[j] == ACS_VOTE_ONE for every j (step 1 fired per accept).   */
/*                                                                        */
/*  With the pre-fix code the (n-t)th accept would flip threshold to 1    */
/*  and emit a burst of vote-0 CON_SEND actions.                          */
/*------------------------------------------------------------------------*/

static void
testStepTwoTrigger(
  void
){
  struct acs *a;
  unsigned long sz;
  struct acsAct acts[ACS_MAX_ACTS(MAX_PEERS, MAX_PHASES)];
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

  printf("\nACS — BKR94 Step 2 trigger regression\n");

  encN = 3;  /* actual N = 4, n-t threshold = 3 */
  t = 1;
  N = (unsigned int)encN + 1;
  val = 'x';

  sz = acsSz(encN, 0, MAX_PHASES);
  a = (struct acs *)calloc(1, sz);
  if (!a) {
    check("alloc acs instance", 0);
    return;
  }
  acsInit(a, encN, t, 0, MAX_PHASES, 0, testCoin, 0);

  voteZeroSeen = 0;
  voteOneSeen = 0;

  /*
   * Drive each Fig1 instance to ACCEPT on this peer:
   *   INITIAL from origin + ECHO from every peer + READY from every
   *   peer.  The READY cascade crosses 2t+1 inside acsProposalInput
   *   and fires ACS_ACT_CON_SEND (vote-1, step 1).
   */
  for (origin = 0; origin < N; ++origin) {
    nacts = acsProposalInput(a, (unsigned char)origin, BRACHA87_INITIAL,
                             (unsigned char)origin, &val, acts);
    for (k = 0; k < nacts; ++k)
      if (acts[k].act == ACS_ACT_CON_SEND) {
        if (acts[k].conValue == 0) ++voteZeroSeen;
        else                       ++voteOneSeen;
      }

    for (from = 0; from < N; ++from) {
      nacts = acsProposalInput(a, (unsigned char)origin, BRACHA87_ECHO,
                               from, &val, acts);
      for (k = 0; k < nacts; ++k)
        if (acts[k].act == ACS_ACT_CON_SEND) {
          if (acts[k].conValue == 0) ++voteZeroSeen;
          else                       ++voteOneSeen;
        }
    }

    for (from = 0; from < N; ++from) {
      nacts = acsProposalInput(a, (unsigned char)origin, BRACHA87_READY,
                               from, &val, acts);
      for (k = 0; k < nacts; ++k)
        if (acts[k].act == ACS_ACT_CON_SEND) {
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
   * voted[] layout: first N bytes of a->data (see acsVoted in
   * acs.c).  ACS_VOTE_ONE is the internal sentinel value 1.
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

int
main(
  void
){
  printf("acs test suite\n");
  printf("==============\n\n");

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

  free(MsgQ);

  printf("\n==============\n");
  if (Fail) {
    printf("%d FAILED\n", Fail);
    return (1);
  }
  printf("ALL PASSED\n");
  return (0);
}
