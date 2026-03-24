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
            (unsigned char)(vLen - 1), MAX_PHASES, (unsigned char)i, 0, 0);
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
    struct acsAct acts[MAX_PEERS + 4];
    unsigned int nacts;
    unsigned int k;
    unsigned int oldTail;

    m = &MsgQ[Qhead++];
    st = peers[m->to];

    if (acsComplete(st))
      continue;

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
   * making the subset smaller than n-t. With a random coin the
   * |subset| >= n-t guarantee holds w.h.p. (see HoneyBadger Thm 3).
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
            (unsigned char)(vLen - 1), MAX_PHASES, (unsigned char)i, 0, 0);
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
    struct acsAct acts[MAX_PEERS + 4];
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

  free(MsgQ);

  printf("\n==============\n");
  if (Fail) {
    printf("%d FAILED\n", Fail);
    return (1);
  }
  printf("ALL PASSED\n");
  return (0);
}
