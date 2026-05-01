/*
 * Tests for bracha87.[hc] — Bracha 1987 Figures 1, 3, 4.
 *
 * Simulates all-to-all message passing for Figure 1 (reliable broadcast)
 * and round-by-round processing for Figure 4 (consensus).
 * Labeled output, sequential simulation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bracha87.h"


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

/*************************************************************************/
/*  Figure 1 simulation                                                  */
/*************************************************************************/

#define VLEN     4   /* actual value length; API vLen = VLEN - 1 */
#define MAX_N   16
#define MAX_MSGS 8192

struct fig1Msg {
  unsigned char type;   /* BRACHA87_INITIAL/ECHO/READY */
  unsigned char from;
  unsigned char to;
  unsigned char value[VLEN];
};

static struct fig1Msg MsgQ[MAX_MSGS];
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
  unsigned char type
 ,unsigned char from
 ,unsigned char to
 ,const unsigned char *value
){
  if (Qtail >= MAX_MSGS) {
    fprintf(stderr, "qPush overflow\n");
    return;
  }
  MsgQ[Qtail].type = type;
  MsgQ[Qtail].from = from;
  MsgQ[Qtail].to = to;
  memcpy(MsgQ[Qtail].value, value, VLEN);
  ++Qtail;
}

/*
 * Simulate a Figure 1 broadcast.
 *
 *   n:      total peers
 *   t:      max Byzantine
 *   origin: broadcasting peer
 *   value:  broadcast value (VLEN bytes)
 *   silent: bitmask of peers whose OUTBOUND messages are dropped
 *
 * Returns number of peers that accepted.
 */
static unsigned int
simFig1(
  unsigned char n
 ,unsigned char t
 ,unsigned char origin
 ,const unsigned char *value
 ,unsigned int silent
){
  struct bracha87Fig1 *inst[MAX_N];
  unsigned long sz;
  unsigned int accepted;
  unsigned int i;

  sz = bracha87Fig1Sz(n - 1, VLEN - 1);
  for (i = 0; i < n; ++i) {
    inst[i] = (struct bracha87Fig1 *)calloc(1, sz);
    if (!inst[i]) {
      fprintf(stderr, "simFig1 OoR\n");
      return (0);
    }
    bracha87Fig1Init(inst[i], n - 1, t, VLEN - 1);
  }

  /* Origin sends INITIAL to all peers (including self) */
  qInit();
  for (i = 0; i < n; ++i)
    qPush(BRACHA87_INITIAL, origin, (unsigned char)i, value);

  /* Process message queue */
  while (Qhead < Qtail) {
    struct fig1Msg *m = &MsgQ[Qhead++];
    unsigned char out[3];
    unsigned int nout;
    unsigned int j;
    const unsigned char *cv;

    nout = bracha87Fig1Input(inst[m->to], m->type, m->from, m->value, out);

    /* For each output action, enqueue messages */
    for (j = 0; j < nout; ++j) {
      unsigned int k;

      if (out[j] == BRACHA87_ACCEPT)
        continue; /* no message to send */

      /* Silenced peers don't send */
      if (silent & (1u << m->to))
        continue;

      cv = bracha87Fig1Value(inst[m->to]);
      if (!cv)
        continue;

      for (k = 0; k < n; ++k) {
        if (out[j] == BRACHA87_ECHO_ALL)
          qPush(BRACHA87_ECHO, m->to, (unsigned char)k, cv);
        else if (out[j] == BRACHA87_READY_ALL)
          qPush(BRACHA87_READY, m->to, (unsigned char)k, cv);
      }
    }
  }

  accepted = 0;
  for (i = 0; i < n; ++i) {
    if ((inst[i]->flags & BRACHA87_F1_ACCEPTED))
      ++accepted;
    free(inst[i]);
  }
  return (accepted);
}

/*
 * Simulate a Figure 1 broadcast with equivocating origin.
 * Origin sends value1 to peers [0..split-1], value2 to peers [split..n-1].
 * Returns number of peers that accepted, and which value via *acceptedVal.
 */
static unsigned int
simFig1Equivoc(
  unsigned char n
 ,unsigned char t
 ,unsigned char origin
 ,const unsigned char *value1
 ,const unsigned char *value2
 ,unsigned char split
 ,unsigned char *acceptedVal
){
  struct bracha87Fig1 *inst[MAX_N];
  unsigned long sz;
  unsigned int accepted;
  unsigned int i;

  sz = bracha87Fig1Sz(n - 1, VLEN - 1);
  for (i = 0; i < n; ++i) {
    inst[i] = (struct bracha87Fig1 *)calloc(1, sz);
    if (!inst[i]) {
      fprintf(stderr, "simFig1Equivoc OoR\n");
      return (0);
    }
    bracha87Fig1Init(inst[i], n - 1, t, VLEN - 1);
  }

  /* Equivocating origin: different values to different peers */
  qInit();
  for (i = 0; i < n; ++i) {
    if (i < split)
      qPush(BRACHA87_INITIAL, origin, (unsigned char)i, value1);
    else
      qPush(BRACHA87_INITIAL, origin, (unsigned char)i, value2);
  }

  while (Qhead < Qtail) {
    struct fig1Msg *m = &MsgQ[Qhead++];
    unsigned char out[3];
    unsigned int nout;
    unsigned int j;
    const unsigned char *cv;

    nout = bracha87Fig1Input(inst[m->to], m->type, m->from, m->value, out);
    for (j = 0; j < nout; ++j) {
      unsigned int k;

      if (out[j] == BRACHA87_ACCEPT)
        continue;

      cv = bracha87Fig1Value(inst[m->to]);
      if (!cv)
        continue;

      for (k = 0; k < n; ++k) {
        if (out[j] == BRACHA87_ECHO_ALL)
          qPush(BRACHA87_ECHO, m->to, (unsigned char)k, cv);
        else if (out[j] == BRACHA87_READY_ALL)
          qPush(BRACHA87_READY, m->to, (unsigned char)k, cv);
      }
    }
  }

  accepted = 0;
  memset(acceptedVal, 0, VLEN);
  for (i = 0; i < n; ++i) {
    if ((inst[i]->flags & BRACHA87_F1_ACCEPTED)) {
      const unsigned char *cv = bracha87Fig1Value(inst[i]);
      if (cv && !accepted)
        memcpy(acceptedVal, cv, VLEN);
      /* Lemma 2: all accepted values must agree */
      if (cv && accepted)
        check("Fig1 equivoc: Lemma 2 (all accept same value)",
              !memcmp(acceptedVal, cv, VLEN));
      ++accepted;
    }
    free(inst[i]);
  }
  return (accepted);
}

/*
 * Shuffle unprocessed portion of message queue (Fisher-Yates).
 * Simple LCG PRNG seeded by caller.
 */
static void
qShuffle(
  unsigned int *seed
){
  unsigned int i;
  unsigned int n;

  n = Qtail - Qhead;
  if (n < 2)
    return;
  for (i = n - 1; i > 0; --i) {
    unsigned int j;
    struct fig1Msg tmp;

    *seed = *seed * 1103515245u + 12345u;
    j = ((*seed >> 16) & 0x7FFF) % (i + 1);
    tmp = MsgQ[Qhead + i];
    MsgQ[Qhead + i] = MsgQ[Qhead + j];
    MsgQ[Qhead + j] = tmp;
  }
}

/*
 * Like simFig1 but shuffles message queue after each batch of enqueues.
 */
static unsigned int
simFig1Shuffled(
  unsigned char n
 ,unsigned char t
 ,unsigned char origin
 ,const unsigned char *value
 ,unsigned int silent
 ,unsigned int seed
){
  struct bracha87Fig1 *inst[MAX_N];
  unsigned long sz;
  unsigned int accepted;
  unsigned int i;

  sz = bracha87Fig1Sz(n - 1, VLEN - 1);
  for (i = 0; i < n; ++i) {
    inst[i] = (struct bracha87Fig1 *)calloc(1, sz);
    if (!inst[i]) {
      fprintf(stderr, "simFig1Shuffled OoR\n");
      return (0);
    }
    bracha87Fig1Init(inst[i], n - 1, t, VLEN - 1);
  }

  qInit();
  for (i = 0; i < n; ++i)
    qPush(BRACHA87_INITIAL, origin, (unsigned char)i, value);
  qShuffle(&seed);

  while (Qhead < Qtail) {
    struct fig1Msg *m = &MsgQ[Qhead++];
    unsigned char out[3];
    unsigned int nout;
    unsigned int j;
    const unsigned char *cv;
    unsigned int oldTail;

    oldTail = Qtail;
    nout = bracha87Fig1Input(inst[m->to], m->type, m->from, m->value, out);

    for (j = 0; j < nout; ++j) {
      unsigned int k;

      if (out[j] == BRACHA87_ACCEPT)
        continue;
      if (silent & (1u << m->to))
        continue;

      cv = bracha87Fig1Value(inst[m->to]);
      if (!cv)
        continue;

      for (k = 0; k < n; ++k) {
        if (out[j] == BRACHA87_ECHO_ALL)
          qPush(BRACHA87_ECHO, m->to, (unsigned char)k, cv);
        else if (out[j] == BRACHA87_READY_ALL)
          qPush(BRACHA87_READY, m->to, (unsigned char)k, cv);
      }
    }

    /* Shuffle newly enqueued messages into remaining queue */
    if (Qtail > oldTail)
      qShuffle(&seed);
  }

  accepted = 0;
  for (i = 0; i < n; ++i) {
    if ((inst[i]->flags & BRACHA87_F1_ACCEPTED))
      ++accepted;
    free(inst[i]);
  }
  return (accepted);
}

/*************************************************************************/
/*  Figure 4 simulation                                                  */
/*************************************************************************/

static unsigned char CoinVal;

static unsigned char
testCoin(
  void *closure
 ,unsigned char phase
){
  (void)closure;
  (void)phase;
  return (CoinVal);
}

/*
 * Simulate Figure 4 consensus with all-honest peers.
 * Each peer sees all n messages per round.
 *
 * Returns number of peers that decided.
 * All decisions are verified to agree (Theorem 2).
 */
static unsigned int
simFig4(
  unsigned char n
 ,unsigned char t
 ,const unsigned char *initVals
 ,unsigned char *decidedVal
 ,unsigned char maxPhases
){
  struct bracha87Fig4 *inst[MAX_N];
  unsigned long sz;
  unsigned int k;
  unsigned char vals[MAX_N];
  unsigned char senders[MAX_N];
  unsigned int decided;
  unsigned int i;
  unsigned int act;
  int anyDecided;

  sz = bracha87Fig4Sz(n - 1, maxPhases);
  for (i = 0; i < n; ++i) {
    inst[i] = (struct bracha87Fig4 *)calloc(1, sz);
    if (!inst[i]) {
      fprintf(stderr, "simFig4 OoR\n");
      return (0);
    }
    bracha87Fig4Init(inst[i], n - 1, t, maxPhases, initVals[i], testCoin, 0);
    senders[i] = (unsigned char)i;
  }

  /* Initial values for round 0 */
  for (i = 0; i < n; ++i)
    vals[i] = initVals[i];

  anyDecided = 0;
  for (k = 0; k < (unsigned int)maxPhases * 3; ++k) {
    /* All peers process this round with the collected values */
    for (i = 0; i < n; ++i) {
      if (inst[i]->decided)
        continue;
      act = bracha87Fig4Round(inst[i], (unsigned char)k, n, senders, vals);
      if (act & BRACHA87_DECIDE)
        anyDecided = 1;
    }

    if (anyDecided)
      break;

    /* Collect values for next round */
    for (i = 0; i < n; ++i)
      vals[i] = inst[i]->value;
  }

  decided = 0;
  *decidedVal = 0xFF;
  for (i = 0; i < n; ++i) {
    if (inst[i]->decided) {
      if (*decidedVal == 0xFF)
        *decidedVal = inst[i]->decision;
      /* Theorem 2: all decisions agree */
      check("Fig4: Theorem 2 (all decide same value)",
            inst[i]->decision == *decidedVal);
      ++decided;
    }
    free(inst[i]);
  }
  return (decided);
}

/*************************************************************************/
/*  Figure 1 — unit-level rule tests                                     */
/*************************************************************************/

/*
 * Test each of the 6 rules individually on a single Fig1 instance.
 * n=4, t=1. Thresholds: echo (n+t)/2+1=3, ready_amplify t+1=2, accept 2t+1=3.
 */
static void
testFig1Rules(
  void
){
  struct bracha87Fig1 *b;
  unsigned long sz;
  unsigned char out[3];
  unsigned int nout;
  unsigned char val_A[VLEN];
  unsigned char val_B[VLEN];

  memcpy(val_A, "AAAA", VLEN);
  memcpy(val_B, "BBBB", VLEN);
  sz = bracha87Fig1Sz(3, VLEN - 1);

  printf("\n  Rule-by-rule tests (n=4, t=1):\n");

  /*
   * Rule 1: INITIAL from origin, !echoed -> echo all
   */
  b = (struct bracha87Fig1 *)calloc(1, sz);
  bracha87Fig1Init(b, 3, 1, VLEN - 1);
  nout = bracha87Fig1Input(b, BRACHA87_INITIAL, 0, val_A, out);
  printf("    Rule 1 (INITIAL)       : nout=%u out[0]=%u\n", nout, nout ? out[0] : 0);
  check("Rule 1: INITIAL -> ECHO_ALL", nout >= 1 && out[0] == BRACHA87_ECHO_ALL);
  check("Rule 1: echoed set", (b->flags & BRACHA87_F1_ECHOED));
  check("Rule 1: value correct", !memcmp(bracha87Fig1Value(b), val_A, VLEN));

  /* Rule 1: second INITIAL ignored */
  nout = bracha87Fig1Input(b, BRACHA87_INITIAL, 1, val_B, out);
  check("Rule 1: second INITIAL ignored", nout == 0);
  free(b);

  /*
   * Rule 2: !echoed, echo_count[v] > (n+t)/2 -> echo all (threshold=3 for n=4,t=1)
   * No INITIAL received. Feed echoes from peers.
   */
  b = (struct bracha87Fig1 *)calloc(1, sz);
  bracha87Fig1Init(b, 3, 1, VLEN - 1);

  /* First echo: count=1 < 3, no action */
  nout = bracha87Fig1Input(b, BRACHA87_ECHO, 0, val_A, out);
  check("Rule 2: 1 echo, no action", nout == 0);
  check("Rule 2: not echoed yet", !(b->flags & BRACHA87_F1_ECHOED));

  /* Second echo: count=2 < 3, no action */
  nout = bracha87Fig1Input(b, BRACHA87_ECHO, 1, val_A, out);
  check("Rule 2: 2 echoes, no action", nout == 0);
  check("Rule 2: still not echoed", !(b->flags & BRACHA87_F1_ECHOED));

  /* Third echo: count=3 >= 3, rule 2 fires */
  nout = bracha87Fig1Input(b, BRACHA87_ECHO, 2, val_A, out);
  printf("    Rule 2 (echo quorum)   : nout=%u out[0]=%u\n", nout, nout ? out[0] : 0);
  check("Rule 2: 3 echoes -> ECHO_ALL", nout >= 1 && out[0] == BRACHA87_ECHO_ALL);
  check("Rule 2: echoed set", (b->flags & BRACHA87_F1_ECHOED));
  check("Rule 2: value correct", !memcmp(bracha87Fig1Value(b), val_A, VLEN));
  free(b);

  /*
   * Rule 3: !echoed, ready_count[v] >= t+1=2 -> echo all
   * No INITIAL or echoes. Feed readys from peers.
   */
  b = (struct bracha87Fig1 *)calloc(1, sz);
  bracha87Fig1Init(b, 3, 1, VLEN - 1);

  nout = bracha87Fig1Input(b, BRACHA87_READY, 0, val_A, out);
  check("Rule 3: 1 ready, no action", nout == 0);

  nout = bracha87Fig1Input(b, BRACHA87_READY, 1, val_A, out);
  printf("    Rule 3 (ready amplify) : nout=%u out[0]=%u\n", nout, nout ? out[0] : 0);
  check("Rule 3: 2 readys -> ECHO_ALL", nout >= 1 && out[0] == BRACHA87_ECHO_ALL);
  check("Rule 3: echoed set", (b->flags & BRACHA87_F1_ECHOED));
  free(b);

  /*
   * Rule 4: echoed && !rdSent && echo_count[v] > (n+t)/2 -> ready all
   * First INITIAL (sets echoed), then feed echoes.
   */
  b = (struct bracha87Fig1 *)calloc(1, sz);
  bracha87Fig1Init(b, 3, 1, VLEN - 1);

  bracha87Fig1Input(b, BRACHA87_INITIAL, 0, val_A, out);
  check("Rule 4 setup: echoed", (b->flags & BRACHA87_F1_ECHOED));

  /* Feed echoes for same value (need echo_count >= 3) */
  nout = bracha87Fig1Input(b, BRACHA87_ECHO, 1, val_A, out);
  check("Rule 4: 1 echo, no ready", nout == 0);

  nout = bracha87Fig1Input(b, BRACHA87_ECHO, 2, val_A, out);
  check("Rule 4: 2 echoes, no ready", nout == 0);

  nout = bracha87Fig1Input(b, BRACHA87_ECHO, 3, val_A, out);
  printf("    Rule 4 (echo->ready)   : nout=%u", nout);
  { unsigned int k; for (k = 0; k < nout; ++k) printf(" out[%u]=%u", k, out[k]); }
  printf("\n");
  /* At 3 echoes, rule 4 fires (ready) */
  {
    int hasReady = 0;
    unsigned int k;
    for (k = 0; k < nout; ++k)
      if (out[k] == BRACHA87_READY_ALL) hasReady = 1;
    check("Rule 4: echo threshold -> READY_ALL", hasReady);
  }
  check("Rule 4: rdSent set", (b->flags & BRACHA87_F1_RDSENT));
  free(b);

  /*
   * Rule 5: echoed && !rdSent && ready_count[v] >= t+1 -> ready all
   * INITIAL (sets echoed), then readys.
   */
  b = (struct bracha87Fig1 *)calloc(1, sz);
  bracha87Fig1Init(b, 3, 1, VLEN - 1);

  bracha87Fig1Input(b, BRACHA87_INITIAL, 0, val_A, out);

  nout = bracha87Fig1Input(b, BRACHA87_READY, 1, val_A, out);
  check("Rule 5: 1 ready, no action", nout == 0);

  nout = bracha87Fig1Input(b, BRACHA87_READY, 2, val_A, out);
  printf("    Rule 5 (ready->ready)  : nout=%u", nout);
  { unsigned int k; for (k = 0; k < nout; ++k) printf(" out[%u]=%u", k, out[k]); }
  printf("\n");
  {
    int hasReady = 0;
    unsigned int k;
    for (k = 0; k < nout; ++k)
      if (out[k] == BRACHA87_READY_ALL) hasReady = 1;
    check("Rule 5: ready threshold -> READY_ALL", hasReady);
  }
  check("Rule 5: rdSent set", (b->flags & BRACHA87_F1_RDSENT));
  free(b);

  /*
   * Rule 6: rdSent && ready_count[v] >= 2t+1=3 -> accept
   * INITIAL, enough echoes for ready, then readys to accept.
   */
  b = (struct bracha87Fig1 *)calloc(1, sz);
  bracha87Fig1Init(b, 3, 1, VLEN - 1);

  bracha87Fig1Input(b, BRACHA87_INITIAL, 0, val_A, out);
  bracha87Fig1Input(b, BRACHA87_ECHO, 1, val_A, out);
  bracha87Fig1Input(b, BRACHA87_ECHO, 2, val_A, out);
  bracha87Fig1Input(b, BRACHA87_ECHO, 3, val_A, out); /* triggers ready */
  check("Rule 6 setup: rdSent", (b->flags & BRACHA87_F1_RDSENT));

  nout = bracha87Fig1Input(b, BRACHA87_READY, 0, val_A, out);
  check("Rule 6: 1 ready, no accept", nout == 0 || out[0] != BRACHA87_ACCEPT);

  nout = bracha87Fig1Input(b, BRACHA87_READY, 1, val_A, out);
  check("Rule 6: 2 readys, no accept", nout == 0 || out[0] != BRACHA87_ACCEPT);

  nout = bracha87Fig1Input(b, BRACHA87_READY, 2, val_A, out);
  printf("    Rule 6 (accept)        : nout=%u out[0]=%u\n", nout, nout ? out[0] : 0);
  check("Rule 6: 3 readys -> ACCEPT", nout == 1 && out[0] == BRACHA87_ACCEPT);
  check("Rule 6: accepted set", (b->flags & BRACHA87_F1_ACCEPTED));

  /* After accept, all messages ignored */
  nout = bracha87Fig1Input(b, BRACHA87_READY, 3, val_A, out);
  check("Rule 6: post-accept ignored", nout == 0);
  free(b);
}

/*
 * Test cascade: rule 3 -> rule 5 -> rule 6.
 * Feed enough readys to trigger echo (rule 3), which sets echoed,
 * then rule 5 fires (ready_count >= t+1 with echoed), then
 * if ready_count >= 2t+1, rule 6 fires.
 */
static void
testFig1Cascade(
  void
){
  struct bracha87Fig1 *b;
  unsigned long sz;
  unsigned char out[3];
  unsigned int nout;
  unsigned char val_A[VLEN];

  memcpy(val_A, "AAAA", VLEN);
  sz = bracha87Fig1Sz(3, VLEN - 1);

  printf("\n  Cascade tests:\n");

  /*
   * Rule 3 -> Rule 5 cascade.
   * n=4, t=1. Ready threshold=t+1=2.
   * 2 readys: rule 3 fires (echo), rule 5 fires (ready).
   */
  b = (struct bracha87Fig1 *)calloc(1, sz);
  bracha87Fig1Init(b, 3, 1, VLEN - 1);

  bracha87Fig1Input(b, BRACHA87_READY, 0, val_A, out);

  nout = bracha87Fig1Input(b, BRACHA87_READY, 1, val_A, out);
  printf("    3->5 cascade           : nout=%u", nout);
  { unsigned int k; for (k = 0; k < nout; ++k) printf(" out[%u]=%u", k, out[k]); }
  printf("\n");
  check("Cascade 3->5: echoed", (b->flags & BRACHA87_F1_ECHOED));
  check("Cascade 3->5: rdSent", (b->flags & BRACHA87_F1_RDSENT));
  {
    int hasEcho = 0, hasReady = 0;
    unsigned int k;
    for (k = 0; k < nout; ++k) {
      if (out[k] == BRACHA87_ECHO_ALL) hasEcho = 1;
      if (out[k] == BRACHA87_READY_ALL) hasReady = 1;
    }
    check("Cascade 3->5: ECHO_ALL + READY_ALL", hasEcho && hasReady);
  }
  free(b);

  /*
   * Rule 3 -> Rule 5 -> Rule 6 cascade.
   * n=4, t=1. 3 readys: rule 3 fires (2 readys), rule 5 fires,
   * then with 3 readys, rule 6 fires.
   * But rule 3 fires at the 2nd ready, not the 3rd.
   * So: 2 readys -> rule 3+5, then 3rd ready -> rule 6.
   */
  b = (struct bracha87Fig1 *)calloc(1, sz);
  bracha87Fig1Init(b, 3, 1, VLEN - 1);

  bracha87Fig1Input(b, BRACHA87_READY, 0, val_A, out);
  bracha87Fig1Input(b, BRACHA87_READY, 1, val_A, out); /* cascade 3->5 */

  nout = bracha87Fig1Input(b, BRACHA87_READY, 2, val_A, out);
  printf("    3->5->6 cascade        : nout=%u", nout);
  { unsigned int k; for (k = 0; k < nout; ++k) printf(" out[%u]=%u", k, out[k]); }
  printf("\n");
  check("Cascade 3->5->6: ACCEPT", nout == 1 && out[0] == BRACHA87_ACCEPT);
  check("Cascade 3->5->6: accepted", (b->flags & BRACHA87_F1_ACCEPTED));
  free(b);
}

/*
 * Test deduplication.
 */
static void
testFig1Dedup(
  void
){
  struct bracha87Fig1 *b;
  unsigned long sz;
  unsigned char out[3];
  unsigned int nout;
  unsigned char val_A[VLEN];

  memcpy(val_A, "AAAA", VLEN);
  sz = bracha87Fig1Sz(3, VLEN - 1);

  printf("\n  Deduplication tests:\n");

  b = (struct bracha87Fig1 *)calloc(1, sz);
  bracha87Fig1Init(b, 3, 1, VLEN - 1);

  bracha87Fig1Input(b, BRACHA87_ECHO, 0, val_A, out);
  nout = bracha87Fig1Input(b, BRACHA87_ECHO, 0, val_A, out);
  printf("    Echo dedup             : nout=%u\n", nout);
  check("Dedup: second echo from same sender ignored", nout == 0);

  bracha87Fig1Input(b, BRACHA87_READY, 0, val_A, out);
  nout = bracha87Fig1Input(b, BRACHA87_READY, 0, val_A, out);
  printf("    Ready dedup            : nout=%u\n", nout);
  check("Dedup: second ready from same sender ignored", nout == 0);
  free(b);
}

/*
 * Test edge cases: from >= n, bad type.
 */
static void
testFig1EdgeCases(
  void
){
  struct bracha87Fig1 *b;
  unsigned long sz;
  unsigned char out[3];
  unsigned int nout;
  unsigned char val_A[VLEN];

  memcpy(val_A, "AAAA", VLEN);
  sz = bracha87Fig1Sz(3, VLEN - 1);

  printf("\n  Edge case tests:\n");

  b = (struct bracha87Fig1 *)calloc(1, sz);
  bracha87Fig1Init(b, 3, 1, VLEN - 1);

  nout = bracha87Fig1Input(b, BRACHA87_INITIAL, 4, val_A, out);
  printf("    from >= n              : nout=%u\n", nout);
  check("Edge: from >= n rejected", nout == 0);

  nout = bracha87Fig1Input(b, BRACHA87_INITIAL, 255, val_A, out);
  check("Edge: from=255 rejected", nout == 0);

  nout = bracha87Fig1Input(b, 99, 0, val_A, out);
  printf("    bad type               : nout=%u\n", nout);
  check("Edge: bad type rejected", nout == 0);

  nout = bracha87Fig1Input(0, BRACHA87_INITIAL, 0, val_A, out);
  check("Edge: null instance rejected", nout == 0);

  nout = bracha87Fig1Input(b, BRACHA87_INITIAL, 0, 0, out);
  check("Edge: null value rejected", nout == 0);

  nout = bracha87Fig1Input(b, BRACHA87_INITIAL, 0, val_A, 0);
  check("Edge: null out rejected", nout == 0);
  free(b);
}

/*
 * Test threshold boundaries: just below and at threshold.
 * n=7, t=2: echo threshold=(7+2)/2+1=5, ready_amplify=t+1=3, accept=2t+1=5.
 */
static void
testFig1Thresholds(
  void
){
  struct bracha87Fig1 *b;
  unsigned long sz;
  unsigned char out[3];
  unsigned int nout;
  unsigned char val_A[VLEN];
  unsigned int i;

  memcpy(val_A, "AAAA", VLEN);
  sz = bracha87Fig1Sz(6, VLEN - 1);

  printf("\n  Threshold boundary tests (n=7, t=2):\n");

  /*
   * Echo threshold: (n+t)/2+1 = 5. Need 5 echoes.
   */
  b = (struct bracha87Fig1 *)calloc(1, sz);
  bracha87Fig1Init(b, 6, 2, VLEN - 1);

  for (i = 0; i < 4; ++i)
    bracha87Fig1Input(b, BRACHA87_ECHO, (unsigned char)i, val_A, out);
  check("Threshold: 4 echoes, not echoed", !(b->flags & BRACHA87_F1_ECHOED));

  nout = bracha87Fig1Input(b, BRACHA87_ECHO, 4, val_A, out);
  printf("    Echo threshold (5)     : echoed=%u\n", !!(b->flags & BRACHA87_F1_ECHOED));
  check("Threshold: 5th echo -> echoed", (b->flags & BRACHA87_F1_ECHOED));
  check("Threshold: 5th echo -> ECHO_ALL", nout >= 1 && out[0] == BRACHA87_ECHO_ALL);
  free(b);

  /*
   * Ready amplification: t+1 = 3. Need 3 readys.
   */
  b = (struct bracha87Fig1 *)calloc(1, sz);
  bracha87Fig1Init(b, 6, 2, VLEN - 1);

  for (i = 0; i < 2; ++i)
    bracha87Fig1Input(b, BRACHA87_READY, (unsigned char)i, val_A, out);
  check("Threshold: 2 readys, not echoed", !(b->flags & BRACHA87_F1_ECHOED));

  nout = bracha87Fig1Input(b, BRACHA87_READY, 2, val_A, out);
  printf("    Ready amplify (3)      : echoed=%u\n", !!(b->flags & BRACHA87_F1_ECHOED));
  check("Threshold: 3rd ready -> echoed (rule 3)", (b->flags & BRACHA87_F1_ECHOED));
  free(b);

  /*
   * Accept threshold: 2t+1 = 5.
   * Set up: INITIAL + echoes to get rdSent, then feed readys.
   */
  b = (struct bracha87Fig1 *)calloc(1, sz);
  bracha87Fig1Init(b, 6, 2, VLEN - 1);

  bracha87Fig1Input(b, BRACHA87_INITIAL, 0, val_A, out);
  for (i = 1; i < 6; ++i)
    bracha87Fig1Input(b, BRACHA87_ECHO, (unsigned char)i, val_A, out);
  check("Threshold: setup rdSent", (b->flags & BRACHA87_F1_RDSENT));

  for (i = 0; i < 4; ++i)
    bracha87Fig1Input(b, BRACHA87_READY, (unsigned char)i, val_A, out);
  check("Threshold: 4 readys, not accepted", !(b->flags & BRACHA87_F1_ACCEPTED));

  nout = bracha87Fig1Input(b, BRACHA87_READY, 4, val_A, out);
  printf("    Accept threshold (5)   : accepted=%u\n", !!(b->flags & BRACHA87_F1_ACCEPTED));
  check("Threshold: 5th ready -> accepted", (b->flags & BRACHA87_F1_ACCEPTED));
  check("Threshold: ACCEPT action", nout == 1 && out[0] == BRACHA87_ACCEPT);
  free(b);
}

/*
 * Test liveness: echoed value A, but majority echoes value B.
 * Rule 4 should fire for B (ready B), not blocked by committed A.
 *
 * n=4, t=1. Echo threshold=(n+t)/2+1=3.
 * Peer 0 gets INITIAL(A). Peers 1,2,3 echo B.
 * Peer 0 should eventually ready B because echo_count[B] >= 3.
 */
static void
testFig1Liveness(
  void
){
  struct bracha87Fig1 *b;
  unsigned long sz;
  unsigned char out[3];
  unsigned int nout;
  unsigned char val_A[VLEN];
  unsigned char val_B[VLEN];

  memcpy(val_A, "AAAA", VLEN);
  memcpy(val_B, "BBBB", VLEN);
  sz = bracha87Fig1Sz(3, VLEN - 1);

  printf("\n  Liveness tests (value switch):\n");

  /*
   * Echo A, then receive 3 echoes for B. Rule 4 should fire for B.
   */
  b = (struct bracha87Fig1 *)calloc(1, sz);
  bracha87Fig1Init(b, 3, 1, VLEN - 1);

  bracha87Fig1Input(b, BRACHA87_INITIAL, 0, val_A, out);
  check("Liveness: echoed A", (b->flags & BRACHA87_F1_ECHOED) && !memcmp(bracha87Fig1Value(b), val_A, VLEN));

  bracha87Fig1Input(b, BRACHA87_ECHO, 1, val_B, out);
  check("Liveness: 1 echo B, no ready yet", !(b->flags & BRACHA87_F1_RDSENT));

  bracha87Fig1Input(b, BRACHA87_ECHO, 2, val_B, out);
  check("Liveness: 2 echoes B, no ready yet", !(b->flags & BRACHA87_F1_RDSENT));

  nout = bracha87Fig1Input(b, BRACHA87_ECHO, 3, val_B, out);
  printf("    Echoed A, 3 echoes B   : nout=%u rdSent=%u\n", nout, !!(b->flags & BRACHA87_F1_RDSENT));
  check("Liveness: 3 echoes B -> ready B (rule 4)", (b->flags & BRACHA87_F1_RDSENT));
  check("Liveness: value switched to B",
        !memcmp(bracha87Fig1Value(b), val_B, VLEN));
  {
    int hasReady = 0;
    unsigned int k;
    for (k = 0; k < nout; ++k)
      if (out[k] == BRACHA87_READY_ALL) hasReady = 1;
    check("Liveness: READY_ALL output", hasReady);
  }
  free(b);

  /*
   * Echo A, then receive 2 readys for B. Rule 5 should fire for B.
   */
  b = (struct bracha87Fig1 *)calloc(1, sz);
  bracha87Fig1Init(b, 3, 1, VLEN - 1);

  bracha87Fig1Input(b, BRACHA87_INITIAL, 0, val_A, out);

  bracha87Fig1Input(b, BRACHA87_READY, 1, val_B, out);
  nout = bracha87Fig1Input(b, BRACHA87_READY, 2, val_B, out);
  printf("    Echoed A, 2 readys B   : nout=%u rdSent=%u\n", nout, !!(b->flags & BRACHA87_F1_RDSENT));
  check("Liveness: 2 readys B -> ready B (rule 5)", (b->flags & BRACHA87_F1_RDSENT));
  check("Liveness: value switched to B (rule 5)",
        !memcmp(bracha87Fig1Value(b), val_B, VLEN));
  free(b);

  /*
   * rdSent for A, then receive 3 readys for B. Rule 6 should accept B.
   */
  b = (struct bracha87Fig1 *)calloc(1, sz);
  bracha87Fig1Init(b, 3, 1, VLEN - 1);

  bracha87Fig1Input(b, BRACHA87_INITIAL, 0, val_A, out);
  bracha87Fig1Input(b, BRACHA87_ECHO, 1, val_A, out);
  bracha87Fig1Input(b, BRACHA87_ECHO, 2, val_A, out);
  bracha87Fig1Input(b, BRACHA87_ECHO, 3, val_A, out); /* rule 4 -> rdSent */
  check("Liveness setup: rdSent for A", (b->flags & BRACHA87_F1_RDSENT));

  bracha87Fig1Input(b, BRACHA87_READY, 0, val_B, out);
  bracha87Fig1Input(b, BRACHA87_READY, 1, val_B, out);
  nout = bracha87Fig1Input(b, BRACHA87_READY, 2, val_B, out);
  printf("    rdSent A, 3 readys B   : nout=%u accepted=%u\n", nout, !!(b->flags & BRACHA87_F1_ACCEPTED));
  check("Liveness: 3 readys B -> accept B (rule 6)", (b->flags & BRACHA87_F1_ACCEPTED));
  check("Liveness: accepted value is B",
        !memcmp(bracha87Fig1Value(b), val_B, VLEN));
  free(b);
}

/*
 * Test asymmetric equivocation: origin sends A to 1 peer, B to rest.
 * With n=4 t=1: origin sends A to peer 0, B to peers 1,2,3.
 * The majority (B) has 3 echoes, well above threshold.
 * All honest peers should accept the SAME value.
 *
 * This test specifically exercises the liveness fix:
 * peer 0 echoed A but must accept B when B's counts cross thresholds.
 */
static void
testFig1AsymEquivoc(
  void
){
  unsigned char val1[VLEN], val2[VLEN], aval[VLEN];
  unsigned int a;

  memcpy(val1, "AAAA", VLEN);
  memcpy(val2, "BBBB", VLEN);

  printf("\n  Asymmetric equivocation tests:\n");

  /* n=4 t=1: origin sends A to peer 0 only, B to peers 1,2,3 */
  a = simFig1Equivoc(4, 1, 3, val1, val2, 1, aval);
  printf("    n=4 t=1 split 1:3      : accept %u/4", a);
  if (a > 0) printf(" value=%c%c%c%c", aval[0], aval[1], aval[2], aval[3]);
  printf("\n");
  check("Asym equivoc n=4: all accept", a == 4);
  check("Asym equivoc n=4: majority value wins",
        !memcmp(aval, val2, VLEN));

  /* n=7 t=2: origin sends A to peer 0 only, B to peers 1-6 */
  a = simFig1Equivoc(7, 2, 6, val1, val2, 1, aval);
  printf("    n=7 t=2 split 1:6      : accept %u/7", a);
  if (a > 0) printf(" value=%c%c%c%c", aval[0], aval[1], aval[2], aval[3]);
  printf("\n");
  check("Asym equivoc n=7: all accept", a == 7);
  check("Asym equivoc n=7: majority value wins",
        !memcmp(aval, val2, VLEN));

  /* n=10 t=3: origin sends A to peer 0 only, B to peers 1-9 */
  a = simFig1Equivoc(10, 3, 9, val1, val2, 1, aval);
  printf("    n=10 t=3 split 1:9     : accept %u/10", a);
  if (a > 0) printf(" value=%c%c%c%c", aval[0], aval[1], aval[2], aval[3]);
  printf("\n");
  check("Asym equivoc n=10: all accept", a == 10);
  check("Asym equivoc n=10: majority value wins",
        !memcmp(aval, val2, VLEN));

  /* n=7 t=2: origin sends A to 2 peers, B to 5 peers */
  a = simFig1Equivoc(7, 2, 6, val1, val2, 2, aval);
  printf("    n=7 t=2 split 2:5      : accept %u/7", a);
  if (a > 0) printf(" value=%c%c%c%c", aval[0], aval[1], aval[2], aval[3]);
  printf("\n");
  check("Asym equivoc n=7 2:5: all accept same (Lemma 2)", a > 0);
}

/*
 * Test that shuffled message delivery doesn't break Fig1.
 * The protocol is asynchronous — message order must not matter.
 */
static void
testFig1Shuffled(
  void
){
  unsigned char val[VLEN];
  unsigned int a;
  unsigned int seed;

  memcpy(val, "TEST", VLEN);

  printf("\n  Shuffled delivery tests:\n");

  /* n=4 t=1, multiple seeds */
  for (seed = 1; seed <= 10; ++seed) {
    a = simFig1Shuffled(4, 1, 0, val, 0, seed);
    if (seed == 1)
      printf("    n=4 t=1 seed 1-10      : ");
    check("Shuffled n=4 t=1 all accept", a == 4);
  }
  printf("all accepted\n");

  /* n=7 t=2, multiple seeds */
  for (seed = 1; seed <= 10; ++seed) {
    a = simFig1Shuffled(7, 2, 0, val, 0, seed);
    check("Shuffled n=7 t=2 all accept", a == 7);
  }
  printf("    n=7 t=2 seed 1-10      : all accepted\n");

  /* n=10 t=3, multiple seeds */
  for (seed = 1; seed <= 10; ++seed) {
    a = simFig1Shuffled(10, 3, 0, val, 0, seed);
    check("Shuffled n=10 t=3 all accept", a == 10);
  }
  printf("    n=10 t=3 seed 1-10     : all accepted\n");

  /* n=4 t=1, 1 silent peer, shuffled */
  for (seed = 1; seed <= 10; ++seed) {
    a = simFig1Shuffled(4, 1, 0, val, 1u << 3, seed);
    check("Shuffled n=4 1-silent all accept", a == 4);
  }
  printf("    n=4 t=1 1-silent shuf  : all accepted\n");

  /* n=7 t=2, 2 silent peers, shuffled */
  for (seed = 1; seed <= 10; ++seed) {
    a = simFig1Shuffled(7, 2, 0, val, (1u << 5) | (1u << 6), seed);
    check("Shuffled n=7 2-silent all accept", a == 7);
  }
  printf("    n=7 t=2 2-silent shuf  : all accepted\n");
}

/*************************************************************************/
/*  Figure 3 — multi-round tests                                         */
/*************************************************************************/

/*
 * Simple N function: majority vote.
 */
static int
testNfn(
  void *closure
 ,unsigned char k
 ,unsigned int n_msgs
 ,const unsigned char *senders
 ,const unsigned char *values
 ,unsigned char *result
){
  unsigned int cnt[2];
  unsigned int i;

  (void)closure;
  (void)k;
  (void)senders;

  cnt[0] = cnt[1] = 0;
  for (i = 0; i < n_msgs; ++i)
    if (values[i] <= 1)
      ++cnt[values[i]];

  *result = (cnt[1] > cnt[0]) ? 1 : 0;
  return (0);
}

/*************************************************************************/
/*  Figure 2 — round accumulation tests                                  */
/*************************************************************************/

static void
testFig2(
  void
){
  struct bracha87Fig2 *f2;
  unsigned long sz;
  unsigned int rc;
  unsigned char senders[4];
  unsigned char values[4];
  unsigned int cnt;

  printf("\nFigure 2 — Abstract protocol round\n");

  /* n=4, t=1, n-t=3, maxRounds=3 */
  sz = bracha87Fig2Sz(3, 3);
  f2 = (struct bracha87Fig2 *)calloc(1, sz);
  bracha87Fig2Init(f2, 3, 1, 3);

  /* Round 0: accumulate toward n-t=3 */
  rc = bracha87Fig2Receive(f2, 0, 0, 10);
  printf("  Receive(0,0,10)=%u\n", rc);
  check("Fig2 round 0 msg 0", rc == 0);

  rc = bracha87Fig2Receive(f2, 0, 1, 20);
  check("Fig2 round 0 msg 1", rc == 0);

  rc = bracha87Fig2Receive(f2, 0, 2, 30);
  printf("  Receive(0,2,30)=%u (n-t reached)\n", rc);
  check("Fig2 round 0 complete", rc == BRACHA87_ROUND_COMPLETE);

  /* Fourth message: already complete, no re-trigger */
  rc = bracha87Fig2Receive(f2, 0, 3, 40);
  check("Fig2 round 0 post-complete", rc == 0);

  /* RecvCount */
  printf("  RecvCount(0)=%u\n", bracha87Fig2RecvCount(f2, 0));
  check("Fig2 RecvCount", bracha87Fig2RecvCount(f2, 0) == 4);

  /* GetReceived */
  cnt = bracha87Fig2GetReceived(f2, 0, senders, values);
  printf("  GetReceived(0): cnt=%u\n", cnt);
  check("Fig2 GetReceived count", cnt == 4);
  check("Fig2 GetReceived s[0]=0 v[0]=10",
        cnt >= 1 && senders[0] == 0 && values[0] == 10);
  check("Fig2 GetReceived s[1]=1 v[1]=20",
        cnt >= 2 && senders[1] == 1 && values[1] == 20);

  /* Dedup */
  rc = bracha87Fig2Receive(f2, 0, 0, 99);
  check("Fig2 dedup", rc == 0);
  check("Fig2 dedup count unchanged", bracha87Fig2RecvCount(f2, 0) == 4);

  /* Edge cases: round >= maxRounds rejected (0-based) */
  rc = bracha87Fig2Receive(f2, 3, 0, 0);
  check("Fig2 round >= maxRounds rejected", rc == 0);
  rc = bracha87Fig2Receive(f2, 4, 0, 0);
  check("Fig2 round > max rejected", rc == 0);
  rc = bracha87Fig2Receive(f2, 0, 4, 0);
  check("Fig2 sender >= n rejected", rc == 0);
  check("Fig2 RecvCount round >= maxRounds", bracha87Fig2RecvCount(f2, 3) == 0);
  check("Fig2 RecvCount round > max", bracha87Fig2RecvCount(f2, 99) == 0);

  /* Round 1: only 2 messages, not complete */
  bracha87Fig2Receive(f2, 1, 0, 0);
  bracha87Fig2Receive(f2, 1, 1, 1);
  check("Fig2 round 1 incomplete", bracha87Fig2RecvCount(f2, 1) == 2);

  free(f2);
}

/*************************************************************************/
/*  Figure 3 — VALID set tests                                           */
/*************************************************************************/

static void
testFig3(
  void
){
  struct bracha87Fig3 *f3;
  unsigned long sz;
  unsigned int vc;
  unsigned int a;
  unsigned char senders[4];
  unsigned char values[4];
  unsigned int cnt;

  printf("\nFigure 3 — VALID sets\n");

  /*
   * Basic VALID^0 tests
   */
  sz = bracha87Fig3Sz(3, 3);
  f3 = (struct bracha87Fig3 *)calloc(1, sz);
  bracha87Fig3Init(f3, 3, 1, 3, testNfn, 0);

  /* VALID^0: value in {0, 1} */
  a = bracha87Fig3Accept(f3, 0, 0, 0, &vc);
  printf("  VALID^0(0,0)=%u vc=%u\n", a, vc);
  check("Fig3 VALID^0 v=0", a == BRACHA87_VALIDATED);

  a = bracha87Fig3Accept(f3, 0, 1, 1, &vc);
  printf("  VALID^0(1,1)=%u vc=%u\n", a, vc);
  check("Fig3 VALID^0 v=1", a == BRACHA87_VALIDATED);

  a = bracha87Fig3Accept(f3, 0, 2, 0, &vc);
  printf("  VALID^0(2,0)=%u vc=%u\n", a, vc);
  check("Fig3 VALID^0 vc=3", vc == 3);

  /* VALID^0: value=2 should be rejected */
  a = bracha87Fig3Accept(f3, 0, 3, 2, &vc);
  printf("  VALID^0(3,2)=%u vc=%u (invalid value)\n", a, vc);
  check("Fig3 VALID^0 v=2 rejected", a == 0);

  /* Dedup: same sender twice */
  a = bracha87Fig3Accept(f3, 0, 0, 0, &vc);
  printf("  VALID^0(0,0) dup =%u vc=%u\n", a, vc);
  check("Fig3 VALID^0 dedup", a == 0);

  printf("  ValidCount(0)=%u\n", bracha87Fig3ValidCount(f3, 0));
  check("Fig3 ValidCount", bracha87Fig3ValidCount(f3, 0) == 3);

  /*
   * GetValid test
   */
  cnt = bracha87Fig3GetValid(f3, 0, senders, values);
  printf("  GetValid(0): cnt=%u", cnt);
  { unsigned int k; for (k = 0; k < cnt; ++k) printf(" s[%u]=%u,v[%u]=%u", k, senders[k], k, values[k]); }
  printf("\n");
  check("Fig3 GetValid count", cnt == 3);
  check("Fig3 GetValid sender[0]=0", cnt >= 1 && senders[0] == 0);
  check("Fig3 GetValid sender[1]=1", cnt >= 2 && senders[1] == 1);
  check("Fig3 GetValid sender[2]=2", cnt >= 3 && senders[2] == 2);

  /*
   * VALID^1: requires n-t=3 validated messages from round 0,
   * value = N(0, set) = majority of {0, 1, 0} = 0.
   */
  a = bracha87Fig3Accept(f3, 1, 0, 0, &vc);
  printf("  VALID^1(0,0)=%u vc=%u (N(0,{0,1,0})=0)\n", a, vc);
  check("Fig3 VALID^1 v=0 valid", a == BRACHA87_VALIDATED);

  /* VALID^1: value=1 should be rejected (N returns 0) */
  a = bracha87Fig3Accept(f3, 1, 1, 1, &vc);
  printf("  VALID^1(1,1)=%u vc=%u (rejected, N=0 not 1)\n", a, vc);
  check("Fig3 VALID^1 v=1 rejected", a == 0);

  /* VALID^1: another sender with correct value */
  a = bracha87Fig3Accept(f3, 1, 2, 0, &vc);
  printf("  VALID^1(2,0)=%u vc=%u\n", a, vc);
  check("Fig3 VALID^1 second correct", a == BRACHA87_VALIDATED);

  /*
   * Edge cases: round >= maxRounds rejected (0-based)
   */
  a = bracha87Fig3Accept(f3, 3, 0, 0, &vc);
  check("Fig3 round >= maxRounds rejected", a == 0);

  a = bracha87Fig3Accept(f3, 4, 0, 0, &vc);
  check("Fig3 round > maxRounds rejected", a == 0);

  a = bracha87Fig3Accept(f3, 0, 4, 0, &vc);
  check("Fig3 sender >= n rejected", a == 0);

  check("Fig3 ValidCount round >= maxRounds", bracha87Fig3ValidCount(f3, 3) == 0);
  check("Fig3 ValidCount round > max", bracha87Fig3ValidCount(f3, 99) == 0);

  free(f3);
}

/*
 * Test Fig3 deeper recursion: VALID^2 requiring 0->1->2 chain.
 * n=4, t=1, n-t=3. Uses testNfn (majority) as N function.
 */
static void
testFig3Deep(
  void
){
  struct bracha87Fig3 *f3;
  unsigned long sz;
  unsigned int vc;
  unsigned int a;

  printf("\n  VALID^2 deep recursion tests:\n");

  sz = bracha87Fig3Sz(3, 4);
  f3 = (struct bracha87Fig3 *)calloc(1, sz);
  bracha87Fig3Init(f3, 3, 1, 4, testNfn, 0);

  /*
   * Round 0: peers 0,1,2 send {0, 1, 0}. All valid (binary).
   * N(0, {0,1,0}) = majority = 0.
   */
  a = bracha87Fig3Accept(f3, 0, 0, 0, &vc);
  check("Deep VALID^0 peer 0", a == BRACHA87_VALIDATED && vc == 1);
  a = bracha87Fig3Accept(f3, 0, 1, 1, &vc);
  check("Deep VALID^0 peer 1", a == BRACHA87_VALIDATED && vc == 2);
  a = bracha87Fig3Accept(f3, 0, 2, 0, &vc);
  check("Deep VALID^0 peer 2", a == BRACHA87_VALIDATED && vc == 3);

  /*
   * Round 1: peers 0,1,2 send value 0 (= N(0,{0,1,0})).
   * N(1, {0,0,0}) = majority = 0.
   */
  a = bracha87Fig3Accept(f3, 1, 0, 0, &vc);
  check("Deep VALID^1 peer 0", a == BRACHA87_VALIDATED && vc == 1);
  a = bracha87Fig3Accept(f3, 1, 1, 0, &vc);
  check("Deep VALID^1 peer 1", a == BRACHA87_VALIDATED && vc == 2);
  a = bracha87Fig3Accept(f3, 1, 2, 0, &vc);
  check("Deep VALID^1 peer 2", a == BRACHA87_VALIDATED && vc == 3);

  /*
   * Round 2: peers 0,1,2 send value 0 (= N(1,{0,0,0})).
   * Should be validated as VALID^2.
   */
  a = bracha87Fig3Accept(f3, 2, 0, 0, &vc);
  printf("  VALID^2(0,0)=%u vc=%u\n", a, vc);
  check("Deep VALID^2 peer 0", a == BRACHA87_VALIDATED && vc == 1);
  a = bracha87Fig3Accept(f3, 2, 1, 0, &vc);
  check("Deep VALID^2 peer 1", a == BRACHA87_VALIDATED && vc == 2);
  a = bracha87Fig3Accept(f3, 2, 2, 0, &vc);
  check("Deep VALID^2 peer 2", a == BRACHA87_VALIDATED && vc == 3);
  printf("  VALID^2 validCount=%u\n", bracha87Fig3ValidCount(f3, 2));
  check("Deep VALID^2 validCount", bracha87Fig3ValidCount(f3, 2) == 3);

  /* Wrong value at round 2 should be rejected */
  a = bracha87Fig3Accept(f3, 2, 3, 1, &vc);
  printf("  VALID^2(3,1)=%u (wrong value rejected)\n", a);
  check("Deep VALID^2 wrong value rejected", a == 0);

  free(f3);

  /*
   * Test that incomplete VALID^{k-1} blocks VALID^k.
   * Only 2 validated at round 0 (< n-t=3), so round 1 should reject.
   */
  f3 = (struct bracha87Fig3 *)calloc(1, sz);
  bracha87Fig3Init(f3, 3, 1, 4, testNfn, 0);

  bracha87Fig3Accept(f3, 0, 0, 0, &vc);
  bracha87Fig3Accept(f3, 0, 1, 0, &vc);
  /* Only 2 validated at round 0 */
  check("Deep incomplete: round 0 vc=2", vc == 2);

  a = bracha87Fig3Accept(f3, 1, 0, 0, &vc);
  printf("  Incomplete VALID^0 blocks VALID^1: %u\n", a);
  check("Deep incomplete: round 1 rejected", a == 0);

  free(f3);
}

/*
 * Test Fig3 re-evaluation: completing round k auto-validates
 * stored round k+1 messages (Fig 2 round coordination).
 */
static void
testFig3Reeval(
  void
){
  struct bracha87Fig3 *f3;
  unsigned long sz;
  unsigned int vc;
  unsigned int a;

  printf("\n  Re-evaluation tests (Fig 2 round coordination):\n");

  /* n=4, t=1, n-t=3, maxRounds=4 */
  sz = bracha87Fig3Sz(3, 4);
  f3 = (struct bracha87Fig3 *)calloc(1, sz);
  bracha87Fig3Init(f3, 3, 1, 4, testNfn, 0);

  /*
   * Submit round 1 messages BEFORE round 0 completes.
   * They should be stored but not validated.
   */
  a = bracha87Fig3Accept(f3, 1, 0, 0, &vc);
  check("Reeval: round 1 msg 0 stored not valid", a == 0);
  a = bracha87Fig3Accept(f3, 1, 1, 0, &vc);
  check("Reeval: round 1 msg 1 stored not valid", a == 0);
  a = bracha87Fig3Accept(f3, 1, 2, 0, &vc);
  check("Reeval: round 1 msg 2 stored not valid", a == 0);
  check("Reeval: round 1 vc=0", bracha87Fig3ValidCount(f3, 1) == 0);
  check("Reeval: round 1 not complete", !bracha87Fig3RoundComplete(f3, 1));

  /* Complete round 0: {0,0,0}, N=majority=0 */
  a = bracha87Fig3Accept(f3, 0, 0, 0, &vc);
  check("Reeval: round 0 msg 0", a == BRACHA87_VALIDATED);
  a = bracha87Fig3Accept(f3, 0, 1, 0, &vc);
  check("Reeval: round 0 msg 1", a == BRACHA87_VALIDATED);
  a = bracha87Fig3Accept(f3, 0, 2, 0, &vc);
  check("Reeval: round 0 msg 2", a == BRACHA87_VALIDATED);
  check("Reeval: round 0 complete", bracha87Fig3RoundComplete(f3, 0));

  /* Round 1 stored messages should now be validated by re-evaluation.
   * N(0, {0,0,0}) = 0, all stored values are 0 -> valid. */
  printf("    Round 1 after re-eval: vc=%u complete=%d\n",
    bracha87Fig3ValidCount(f3, 1),
    bracha87Fig3RoundComplete(f3, 1));
  check("Reeval: round 1 auto-validated",
        bracha87Fig3ValidCount(f3, 1) == 3);
  check("Reeval: round 1 complete",
        bracha87Fig3RoundComplete(f3, 1));

  free(f3);

  /*
   * Cascade: pre-load rounds 1 and 2, then complete round 0.
   * Round 0 -> cascades to 1 -> cascades to 2.
   */
  printf("    Cascade re-evaluation:\n");
  f3 = (struct bracha87Fig3 *)calloc(1, sz);
  bracha87Fig3Init(f3, 3, 1, 4, testNfn, 0);

  /* Pre-load rounds 1 and 2 (value=0 matches majority throughout) */
  bracha87Fig3Accept(f3, 1, 0, 0, &vc);
  bracha87Fig3Accept(f3, 1, 1, 0, &vc);
  bracha87Fig3Accept(f3, 1, 2, 0, &vc);
  bracha87Fig3Accept(f3, 2, 0, 0, &vc);
  bracha87Fig3Accept(f3, 2, 1, 0, &vc);
  bracha87Fig3Accept(f3, 2, 2, 0, &vc);
  check("Cascade: round 1 not yet valid",
        bracha87Fig3ValidCount(f3, 1) == 0);
  check("Cascade: round 2 not yet valid",
        bracha87Fig3ValidCount(f3, 2) == 0);

  /* Complete round 0 -> cascade through 1 and 2 */
  bracha87Fig3Accept(f3, 0, 0, 0, &vc);
  bracha87Fig3Accept(f3, 0, 1, 0, &vc);
  bracha87Fig3Accept(f3, 0, 2, 0, &vc);
  printf("    Round 0: complete=%d\n",
    bracha87Fig3RoundComplete(f3, 0));
  printf("    Round 1: vc=%u complete=%d\n",
    bracha87Fig3ValidCount(f3, 1),
    bracha87Fig3RoundComplete(f3, 1));
  printf("    Round 2: vc=%u complete=%d\n",
    bracha87Fig3ValidCount(f3, 2),
    bracha87Fig3RoundComplete(f3, 2));
  check("Cascade: round 0 complete",
        bracha87Fig3RoundComplete(f3, 0));
  check("Cascade: round 1 cascaded",
        bracha87Fig3ValidCount(f3, 1) == 3);
  check("Cascade: round 1 complete",
        bracha87Fig3RoundComplete(f3, 1));
  check("Cascade: round 2 cascaded",
        bracha87Fig3ValidCount(f3, 2) == 3);
  check("Cascade: round 2 complete",
        bracha87Fig3RoundComplete(f3, 2));

  free(f3);

  /*
   * Partial cascade then individual arrival.
   * Pre-load round 1 with only 2 msgs, round 2 with 3 msgs.
   * Complete round 0 -> cascade partially fills round 1 (2 < n-t=3).
   * Then a 3rd round-1 message arrives individually -> round 1 completes
   * -> cascades to round 2.
   */
  printf("    Partial cascade + individual arrival:\n");
  f3 = (struct bracha87Fig3 *)calloc(1, sz);
  bracha87Fig3Init(f3, 3, 1, 4, testNfn, 0);

  bracha87Fig3Accept(f3, 1, 0, 0, &vc);
  bracha87Fig3Accept(f3, 1, 1, 0, &vc);
  /* only 2 for round 1 */
  bracha87Fig3Accept(f3, 2, 0, 0, &vc);
  bracha87Fig3Accept(f3, 2, 1, 0, &vc);
  bracha87Fig3Accept(f3, 2, 2, 0, &vc);

  /* Complete round 0 -> cascade stops at round 1 (only 2 valid) */
  bracha87Fig3Accept(f3, 0, 0, 0, &vc);
  bracha87Fig3Accept(f3, 0, 1, 0, &vc);
  bracha87Fig3Accept(f3, 0, 2, 0, &vc);
  check("Partial: round 0 complete",
        bracha87Fig3RoundComplete(f3, 0));
  check("Partial: round 1 vc=2",
        bracha87Fig3ValidCount(f3, 1) == 2);
  check("Partial: round 1 not complete",
        !bracha87Fig3RoundComplete(f3, 1));
  check("Partial: round 2 vc=0 (blocked)",
        bracha87Fig3ValidCount(f3, 2) == 0);

  /* 3rd round-1 message arrives -> completes round 1 -> cascades to 2 */
  a = bracha87Fig3Accept(f3, 1, 2, 0, &vc);
  printf("    Round 1 after 3rd msg: vc=%u complete=%d\n",
    bracha87Fig3ValidCount(f3, 1),
    bracha87Fig3RoundComplete(f3, 1));
  printf("    Round 2 cascaded:      vc=%u complete=%d\n",
    bracha87Fig3ValidCount(f3, 2),
    bracha87Fig3RoundComplete(f3, 2));
  check("Partial: msg validated", a == BRACHA87_VALIDATED);
  check("Partial: round 1 complete",
        bracha87Fig3RoundComplete(f3, 1));
  check("Partial: round 2 cascaded to complete",
        bracha87Fig3ValidCount(f3, 2) == 3);
  check("Partial: round 2 complete",
        bracha87Fig3RoundComplete(f3, 2));

  free(f3);

  /*
   * Arrived dedup blocks re-submission.
   * Sender 0 broadcasts value=1 for round 1 (wrong — N=0).
   * After re-evaluation it stays invalid.
   * Re-submitting sender 0 with the correct value must be blocked
   * (paper: each process broadcasts once per round).
   */
  printf("    Arrived dedup blocks re-submission:\n");
  f3 = (struct bracha87Fig3 *)calloc(1, sz);
  bracha87Fig3Init(f3, 3, 1, 4, testNfn, 0);

  bracha87Fig3Accept(f3, 1, 0, 1, &vc);  /* wrong value */
  bracha87Fig3Accept(f3, 0, 0, 0, &vc);
  bracha87Fig3Accept(f3, 0, 1, 0, &vc);
  bracha87Fig3Accept(f3, 0, 2, 0, &vc);  /* round 0 complete */

  /* sender 0's round 1 msg stays invalid (value=1, N=0) */
  check("Dedup: round 1 vc=0", bracha87Fig3ValidCount(f3, 1) == 0);

  /* Try to re-submit sender 0 with correct value */
  a = bracha87Fig3Accept(f3, 1, 0, 0, &vc);
  printf("    Re-submit sender 0: a=%u vc=%u\n", a, vc);
  check("Dedup: re-submit blocked", a == 0);
  check("Dedup: vc still 0", bracha87Fig3ValidCount(f3, 1) == 0);

  free(f3);

  /*
   * Invalid values must NOT auto-validate on re-evaluation.
   * Round 0 = {0,0,0} -> N=0. Round 1 message with value=1 stays invalid.
   */
  printf("    Invalid value not re-validated:\n");
  f3 = (struct bracha87Fig3 *)calloc(1, sz);
  bracha87Fig3Init(f3, 3, 1, 4, testNfn, 0);

  /* Pre-load round 1: two correct (0), one wrong (1) */
  bracha87Fig3Accept(f3, 1, 0, 0, &vc);
  bracha87Fig3Accept(f3, 1, 1, 0, &vc);
  bracha87Fig3Accept(f3, 1, 2, 1, &vc);  /* wrong value */

  /* Complete round 0 */
  bracha87Fig3Accept(f3, 0, 0, 0, &vc);
  bracha87Fig3Accept(f3, 0, 1, 0, &vc);
  bracha87Fig3Accept(f3, 0, 2, 0, &vc);

  printf("    Round 1: vc=%u (expect 2, not 3)\n",
    bracha87Fig3ValidCount(f3, 1));
  check("Reeval: wrong value stays invalid",
        bracha87Fig3ValidCount(f3, 1) == 2);
  check("Reeval: round 1 not complete (2 < 3)",
        !bracha87Fig3RoundComplete(f3, 1));

  free(f3);
}

/*
 * Test re-cascade when VALID^{k-1} grows past n-t.
 *
 * Round k-1 first reaches n-t with one composition (giving N result A).
 * A round-k message arrives carrying value B != A — stored, rejected.
 * More round-(k-1) messages arrive after first crossing; the new
 * full-set composition gives N result B.  The stored round-k B
 * message must be re-evaluated and validated.
 *
 * Catches the pre-fix bug where the forward cascade fired only on the
 * first crossing of n-t.
 */
static void
testFig3RecascadeOnGrowth(
  void
){
  struct bracha87Fig3 *f3;
  unsigned long sz;
  unsigned int vc;
  unsigned int a;

  printf("\n  Re-cascade on VALID growth past n-t:\n");

  /*
   * n=9 (enc 8), t=1, nt=8.
   *   Round 0 first crossing: 4 zeros + 4 ones -> testNfn tie-break -> 0.
   *   Submit round 1 sender 0 value=1 BEFORE the 9th round-0 arrives;
   *     rejected (N=0 != 1).
   *   Round 0 grows: 9th message (value=1) -> cnt[0]=4, cnt[1]=5 -> N=1.
   *   Stored round 1 v=1 must now re-validate.
   */
  sz = bracha87Fig3Sz(8, 4);
  f3 = (struct bracha87Fig3 *)calloc(1, sz);
  bracha87Fig3Init(f3, 8, 1, 4, testNfn, 0);

  /* Round 0: 4 zeros */
  bracha87Fig3Accept(f3, 0, 0, 0, &vc);
  bracha87Fig3Accept(f3, 0, 1, 0, &vc);
  bracha87Fig3Accept(f3, 0, 2, 0, &vc);
  bracha87Fig3Accept(f3, 0, 3, 0, &vc);
  /* Round 0: 4 ones -> nt=8 reached, N=0 */
  bracha87Fig3Accept(f3, 0, 4, 1, &vc);
  bracha87Fig3Accept(f3, 0, 5, 1, &vc);
  bracha87Fig3Accept(f3, 0, 6, 1, &vc);
  bracha87Fig3Accept(f3, 0, 7, 1, &vc);
  check("Recascade: round 0 complete at n-t",
        bracha87Fig3RoundComplete(f3, 0));

  /* Round 1 v=1 stored, rejected (N(8 msgs, 4:4) -> 0) */
  a = bracha87Fig3Accept(f3, 1, 0, 1, &vc);
  printf("    round 1 v=1 first eval: a=%u vc=%u (expect rejected)\n", a, vc);
  check("Recascade: round 1 v=1 initially rejected", a == 0);
  check("Recascade: round 1 vc=0 before growth",
        bracha87Fig3ValidCount(f3, 1) == 0);

  /* 9th round-0 message (value=1) -> N(9 msgs, 4:5) -> 1 */
  a = bracha87Fig3Accept(f3, 0, 8, 1, &vc);
  check("Recascade: round 0 9th msg validated", a == BRACHA87_VALIDATED);
  check("Recascade: round 0 vc=9", vc == 9);

  /* Stored round 1 v=1 must now be re-evaluated against new N result */
  printf("    round 1 vc after growth: %u (expect 1)\n",
         bracha87Fig3ValidCount(f3, 1));
  check("Recascade: stored round 1 v=1 re-validated on growth",
        bracha87Fig3ValidCount(f3, 1) == 1);

  free(f3);

  /*
   * Multi-step cascade on growth: round k grows -> unlocks stored at
   * k+1 -> unlocks stored at k+2.  Pre-load v=1 messages for rounds 1
   * and 2; they sit rejected until round 0 grows past 4:4 to favor 1.
   */
  printf("    Multi-round re-cascade on growth:\n");
  sz = bracha87Fig3Sz(8, 4);
  f3 = (struct bracha87Fig3 *)calloc(1, sz);
  bracha87Fig3Init(f3, 8, 1, 4, testNfn, 0);

  /* Pre-load round 1 with v=1 (will be rejected at first), and
   * round 2 with v=1 (cannot be evaluated until round 1 has n-t). */
  bracha87Fig3Accept(f3, 1, 0, 1, &vc);
  bracha87Fig3Accept(f3, 2, 0, 1, &vc);

  /* Round 0: 4 zeros + 4 ones -> N=0, round 0 complete; round 1 v=1
   * rejected on first cascade; round 2 stays unreachable. */
  bracha87Fig3Accept(f3, 0, 0, 0, &vc);
  bracha87Fig3Accept(f3, 0, 1, 0, &vc);
  bracha87Fig3Accept(f3, 0, 2, 0, &vc);
  bracha87Fig3Accept(f3, 0, 3, 0, &vc);
  bracha87Fig3Accept(f3, 0, 4, 1, &vc);
  bracha87Fig3Accept(f3, 0, 5, 1, &vc);
  bracha87Fig3Accept(f3, 0, 6, 1, &vc);
  bracha87Fig3Accept(f3, 0, 7, 1, &vc);
  check("Multi: round 1 vc=0 after first crossing",
        bracha87Fig3ValidCount(f3, 1) == 0);
  check("Multi: round 2 vc=0 (round 1 not at n-t)",
        bracha87Fig3ValidCount(f3, 2) == 0);

  /* Add 5 more ones to round 0 -> cnt[0]=4, cnt[1]=9 -> N=1 in full set;
   * but the stored round-1 v=1 needs only one re-eval pass.  We then
   * still need round 1 to reach n-t before round 2 can validate. */
  bracha87Fig3Accept(f3, 0, 8, 1, &vc);
  /* Now also add more round 1 v=1 messages so it reaches n-t. */
  bracha87Fig3Accept(f3, 1, 1, 1, &vc);
  bracha87Fig3Accept(f3, 1, 2, 1, &vc);
  bracha87Fig3Accept(f3, 1, 3, 1, &vc);
  bracha87Fig3Accept(f3, 1, 4, 1, &vc);
  bracha87Fig3Accept(f3, 1, 5, 1, &vc);
  bracha87Fig3Accept(f3, 1, 6, 1, &vc);
  bracha87Fig3Accept(f3, 1, 7, 1, &vc);  /* round 1 vc reaches 8 */
  printf("    round 1 vc=%u round 2 vc=%u\n",
         bracha87Fig3ValidCount(f3, 1),
         bracha87Fig3ValidCount(f3, 2));
  check("Multi: round 1 reached n-t after re-cascade",
        bracha87Fig3ValidCount(f3, 1) >= 8);
  check("Multi: round 2 v=1 unlocked",
        bracha87Fig3ValidCount(f3, 2) == 1);

  free(f3);
}

/*************************************************************************/
/*  Figure 4 — step-by-step tests                                        */
/*************************************************************************/

/*
 * Test Fig4 step-by-step to verify D_FLAG handling.
 */
static void
testFig4Steps(
  void
){
  struct bracha87Fig4 *b;
  unsigned long sz;
  unsigned char vals[MAX_N];
  unsigned char senders[MAX_N];
  unsigned int act;
  unsigned int i;

  printf("\n  Step-by-step tests:\n");

  /* n=4, t=1, all start 0 */
  sz = bracha87Fig4Sz(3, 10);
  b = (struct bracha87Fig4 *)calloc(1, sz);
  CoinVal = 0;
  bracha87Fig4Init(b, 3, 1, 10, 0, testCoin, 0);

  for (i = 0; i < 4; ++i) {
    senders[i] = (unsigned char)i;
    vals[i] = 0;
  }

  /* Round 0 (step 1): majority of {0,0,0,0} = 0 */
  act = bracha87Fig4Round(b, 0, 4, senders, vals);
  printf("    Round 0 (step 1)       : act=%u value=%u\n", act, b->value);
  check("Step 1: BROADCAST", act == BRACHA87_BROADCAST);
  check("Step 1: majority 0", (b->value & (unsigned char)~BRACHA87_D_FLAG) == 0);

  /* Round 1 (step 2): all send 0, >n/2 agree -> (d,0) */
  for (i = 0; i < 4; ++i) vals[i] = 0;
  act = bracha87Fig4Round(b, 1, 4, senders, vals);
  printf("    Round 1 (step 2)       : act=%u value=0x%02x\n", act, b->value);
  check("Step 2: BROADCAST", act == BRACHA87_BROADCAST);
  check("Step 2: D_FLAG set", (b->value & BRACHA87_D_FLAG) != 0);
  check("Step 2: value=0 with D", (b->value & (unsigned char)~BRACHA87_D_FLAG) == 0);

  /* Round 2 (step 3): all send (d,0), dc[0]=4 > 2t=2 -> decide 0 */
  for (i = 0; i < 4; ++i) vals[i] = 0 | BRACHA87_D_FLAG;
  act = bracha87Fig4Round(b, 2, 4, senders, vals);
  printf("    Round 2 (step 3)       : act=%u decided=%u decision=%u\n",
         act, b->decided, b->decision);
  check("Step 3: DECIDE", (act & BRACHA87_DECIDE) != 0);
  check("Step 3: BROADCAST (continue)", (act & BRACHA87_BROADCAST) != 0);
  check("Step 3: decided=0", b->decision == 0);

  free(b);

  /*
   * Step 3: split input forcing coin flip.
   * n=4, t=1. Need dc[dmax] <= t=1 to hit coin.
   * Send 2 plain 0, 2 plain 1 (no d flags). dc[0]=dc[1]=0 -> coin.
   */
  b = (struct bracha87Fig4 *)calloc(1, sz);
  CoinVal = 1;
  bracha87Fig4Init(b, 3, 1, 10, 0, testCoin, 0);

  /* Skip to step 3 by processing rounds 0 and 1 */
  for (i = 0; i < 4; ++i) vals[i] = 0;
  bracha87Fig4Round(b, 0, 4, senders, vals);
  bracha87Fig4Round(b, 1, 4, senders, vals);

  /* Round 2: no d flags -> coin */
  vals[0] = 0; vals[1] = 0; vals[2] = 1; vals[3] = 1;
  act = bracha87Fig4Round(b, 2, 4, senders, vals);
  printf("    Step 3 coin flip       : act=%u value=%u (coin=%u)\n",
         act, b->value, CoinVal);
  check("Step 3 coin: BROADCAST (next phase)", act == BRACHA87_BROADCAST);
  check("Step 3 coin: value = coin", b->value == CoinVal);
  check("Step 3 coin: not decided", b->decided == 0);
  free(b);

  /*
   * Step 3: adopt path. dc[dmax] > t but <= 2t.
   * n=4, t=1. Need dc[v] > 1 but <= 2. So dc[v]=2.
   * But 2 > 2t=2 is false (> not >=), so dc=2 doesn't decide.
   * Actually dc > 2t means dc > 2 for t=1. So dc=2 doesn't decide.
   * dc=2 > t=1 -> adopt.
   */
  b = (struct bracha87Fig4 *)calloc(1, sz);
  CoinVal = 0;
  bracha87Fig4Init(b, 3, 1, 10, 0, testCoin, 0);

  for (i = 0; i < 4; ++i) vals[i] = 0;
  bracha87Fig4Round(b, 0, 4, senders, vals);
  bracha87Fig4Round(b, 1, 4, senders, vals);

  /* Round 2: 2 d-flagged 1, 2 plain 0 */
  vals[0] = 1 | BRACHA87_D_FLAG;
  vals[1] = 1 | BRACHA87_D_FLAG;
  vals[2] = 0;
  vals[3] = 0;
  act = bracha87Fig4Round(b, 2, 4, senders, vals);
  printf("    Step 3 adopt           : act=%u value=%u\n", act, b->value);
  check("Step 3 adopt: BROADCAST (next phase)", act == BRACHA87_BROADCAST);
  check("Step 3 adopt: adopted value 1", b->value == 1);
  check("Step 3 adopt: not decided", b->decided == 0);
  free(b);
}

/*
 * Test Fig4 step 3 boundary: dc == t exactly should coin, not adopt.
 *
 * n=4, t=1: decide > 2t=2, adopt > t=1, coin otherwise.
 * n=7, t=2: decide > 2t=4, adopt > t=2, coin otherwise.
 */
static void
testFig4Step3Boundary(
  void
){
  struct bracha87Fig4 *b;
  unsigned long sz;
  unsigned char vals[MAX_N];
  unsigned char senders[MAX_N];
  unsigned int act;
  unsigned int i;

  printf("\n  Step 3 boundary tests:\n");

  /*
   * n=4, t=1: dc[dmax] == t == 1 -> should coin, not adopt.
   * 1 d-flagged 0, 3 plain 1. dc[0]=1 == t=1. Not > t. -> coin.
   */
  sz = bracha87Fig4Sz(3, 10);
  b = (struct bracha87Fig4 *)calloc(1, sz);
  CoinVal = 1;
  bracha87Fig4Init(b, 3, 1, 10, 0, testCoin, 0);
  for (i = 0; i < 4; ++i) senders[i] = (unsigned char)i;

  for (i = 0; i < 4; ++i) vals[i] = 0;
  bracha87Fig4Round(b, 0, 4, senders, vals);
  bracha87Fig4Round(b, 1, 4, senders, vals);

  vals[0] = 0 | BRACHA87_D_FLAG;
  vals[1] = 1;
  vals[2] = 1;
  vals[3] = 1;
  act = bracha87Fig4Round(b, 2, 4, senders, vals);
  printf("    n=4 dc==t(1)           : act=%u val=%u coin=%u\n",
         act, b->value, CoinVal);
  check("Boundary dc==t n=4: coin not adopt", b->value == CoinVal);
  check("Boundary dc==t n=4: BROADCAST", act == BRACHA87_BROADCAST);
  check("Boundary dc==t n=4: not decided", b->decided == 0);
  free(b);

  /*
   * n=4, t=1: dc[dmax] == t+1 == 2 -> should adopt (> t but not > 2t).
   * 2 d-flagged 1, 2 plain 0. dc[1]=2 > t=1 but dc[1]=2 not > 2t=2.
   */
  b = (struct bracha87Fig4 *)calloc(1, sz);
  CoinVal = 0;
  bracha87Fig4Init(b, 3, 1, 10, 0, testCoin, 0);

  for (i = 0; i < 4; ++i) vals[i] = 0;
  bracha87Fig4Round(b, 0, 4, senders, vals);
  bracha87Fig4Round(b, 1, 4, senders, vals);

  vals[0] = 1 | BRACHA87_D_FLAG;
  vals[1] = 1 | BRACHA87_D_FLAG;
  vals[2] = 0;
  vals[3] = 0;
  act = bracha87Fig4Round(b, 2, 4, senders, vals);
  printf("    n=4 dc==t+1(2)         : act=%u val=%u\n", act, b->value);
  check("Boundary dc==t+1 n=4: adopt value 1", b->value == 1);
  check("Boundary dc==t+1 n=4: not decided", b->decided == 0);
  free(b);

  /*
   * n=4, t=1: dc[dmax] == 2t+1 == 3 -> should decide (> 2t=2).
   * 3 d-flagged 0, 1 plain 1. dc[0]=3 > 2t=2. Decide 0.
   */
  b = (struct bracha87Fig4 *)calloc(1, sz);
  CoinVal = 1;
  bracha87Fig4Init(b, 3, 1, 10, 0, testCoin, 0);

  for (i = 0; i < 4; ++i) vals[i] = 0;
  bracha87Fig4Round(b, 0, 4, senders, vals);
  bracha87Fig4Round(b, 1, 4, senders, vals);

  vals[0] = 0 | BRACHA87_D_FLAG;
  vals[1] = 0 | BRACHA87_D_FLAG;
  vals[2] = 0 | BRACHA87_D_FLAG;
  vals[3] = 1;
  act = bracha87Fig4Round(b, 2, 4, senders, vals);
  printf("    n=4 dc==2t+1(3)        : act=%u decided=%u dec=%u\n",
         act, b->decided, b->decision);
  check("Boundary dc==2t+1 n=4: DECIDE", (act & BRACHA87_DECIDE) != 0);
  check("Boundary dc==2t+1 n=4: decision=0", b->decision == 0);
  free(b);

  /*
   * n=7, t=2: dc == t == 2 -> should coin.
   * 2 d-flagged 0, 5 plain 1. dc[0]=2 == t=2. Not > t. -> coin.
   */
  sz = bracha87Fig4Sz(6, 10);
  b = (struct bracha87Fig4 *)calloc(1, sz);
  CoinVal = 0;
  bracha87Fig4Init(b, 6, 2, 10, 0, testCoin, 0);
  for (i = 0; i < 7; ++i) senders[i] = (unsigned char)i;

  for (i = 0; i < 7; ++i) vals[i] = 0;
  bracha87Fig4Round(b, 0, 7, senders, vals);
  bracha87Fig4Round(b, 1, 7, senders, vals);

  vals[0] = 0 | BRACHA87_D_FLAG;
  vals[1] = 0 | BRACHA87_D_FLAG;
  for (i = 2; i < 7; ++i) vals[i] = 1;
  act = bracha87Fig4Round(b, 2, 7, senders, vals);
  printf("    n=7 dc==t(2)           : act=%u val=%u coin=%u\n",
         act, b->value, CoinVal);
  check("Boundary dc==t n=7: coin", b->value == CoinVal);
  check("Boundary dc==t n=7: not decided", b->decided == 0);
  free(b);

  /*
   * n=7, t=2: dc == t+1 == 3 -> should adopt (> t=2 but not > 2t=4).
   */
  b = (struct bracha87Fig4 *)calloc(1, sz);
  CoinVal = 1;
  bracha87Fig4Init(b, 6, 2, 10, 0, testCoin, 0);

  for (i = 0; i < 7; ++i) vals[i] = 0;
  bracha87Fig4Round(b, 0, 7, senders, vals);
  bracha87Fig4Round(b, 1, 7, senders, vals);

  vals[0] = 0 | BRACHA87_D_FLAG;
  vals[1] = 0 | BRACHA87_D_FLAG;
  vals[2] = 0 | BRACHA87_D_FLAG;
  for (i = 3; i < 7; ++i) vals[i] = 1;
  act = bracha87Fig4Round(b, 2, 7, senders, vals);
  printf("    n=7 dc==t+1(3)         : act=%u val=%u\n", act, b->value);
  check("Boundary dc==t+1 n=7: adopt 0", b->value == 0);
  check("Boundary dc==t+1 n=7: not decided", b->decided == 0);
  free(b);

  /*
   * n=7, t=2: dc == 2t+1 == 5 -> should decide (> 2t=4).
   */
  b = (struct bracha87Fig4 *)calloc(1, sz);
  CoinVal = 1;
  bracha87Fig4Init(b, 6, 2, 10, 0, testCoin, 0);

  for (i = 0; i < 7; ++i) vals[i] = 0;
  bracha87Fig4Round(b, 0, 7, senders, vals);
  bracha87Fig4Round(b, 1, 7, senders, vals);

  for (i = 0; i < 5; ++i) vals[i] = 1 | BRACHA87_D_FLAG;
  vals[5] = 0;
  vals[6] = 0;
  act = bracha87Fig4Round(b, 2, 7, senders, vals);
  printf("    n=7 dc==2t+1(5)        : act=%u decided=%u dec=%u\n",
         act, b->decided, b->decision);
  check("Boundary dc==2t+1 n=7: DECIDE", (act & BRACHA87_DECIDE) != 0);
  check("Boundary dc==2t+1 n=7: decision=1", b->decision == 1);
  free(b);
}

/*
 * Test Fig4: decided process continues participating (paper requirement).
 * After deciding, subsequent rounds return BRACHA87_BROADCAST with the
 * decision value unchanged, advancing through phases.
 */
static void
testFig4PostDecide(
  void
){
  struct bracha87Fig4 *b;
  unsigned long sz;
  unsigned char vals[MAX_N];
  unsigned char senders[MAX_N];
  unsigned int act;
  unsigned int i;

  printf("\n  Post-decide tests:\n");

  sz = bracha87Fig4Sz(3, 10);
  b = (struct bracha87Fig4 *)calloc(1, sz);
  CoinVal = 0;
  bracha87Fig4Init(b, 3, 1, 10, 0, testCoin, 0);

  for (i = 0; i < 4; ++i) {
    senders[i] = (unsigned char)i;
    vals[i] = 0;
  }

  /* Drive to decision at phase 0 step 3 */
  bracha87Fig4Round(b, 0, 4, senders, vals);
  bracha87Fig4Round(b, 1, 4, senders, vals);
  for (i = 0; i < 4; ++i) vals[i] = 0 | BRACHA87_D_FLAG;
  act = bracha87Fig4Round(b, 2, 4, senders, vals);
  check("Post-decide: decided", b->decided == 1);
  check("Post-decide: DECIDE|BROADCAST",
        act == (BRACHA87_DECIDE | BRACHA87_BROADCAST));
  check("Post-decide: advanced to phase 1", b->phase == 1);

  /* Subsequent round: process continues, value frozen at decision */
  for (i = 0; i < 4; ++i) vals[i] = 0;
  act = bracha87Fig4Round(b, 3, 4, senders, vals);
  printf("    Post-decide round 3    : act=%u value=%u\n", act, b->value);
  check("Post-decide: BROADCAST", act == BRACHA87_BROADCAST);
  check("Post-decide: value frozen", b->value == 0);
  check("Post-decide: decision unchanged", b->decision == 0);

  /* Another round: still continues */
  act = bracha87Fig4Round(b, 4, 4, senders, vals);
  check("Post-decide round 4: BROADCAST", act == BRACHA87_BROADCAST);
  check("Post-decide: decision still unchanged", b->decision == 0);
  free(b);
}

/*
 * Test Fig4 post-decide value preservation under adversarial inputs.
 *
 * Bracha post-decide-continuation (pitfall #1) requires a decided
 * process to keep broadcasting its decision value — not whatever
 * majority/(d, majority) the next phase's validated set would suggest.
 * The .dtc-faithful Fig4 dispatch zeroes setMajority and setDMajority
 * when have_decided=yes; this test exercises that explicitly by
 * feeding inputs whose majority disagrees with the decision and
 * checking that b->value remains the decision through every sub-round
 * of the next phase.
 *
 * If sub=0 set value to majority post-decide, b->value would drift to
 * the adversarial majority and the decided peer's continuation
 * broadcast would carry the wrong value, potentially stranding peers
 * still trying to decide.
 */
static void
testFig4PostDecideAdversarial(
  void
){
  struct bracha87Fig4 *b;
  unsigned long sz;
  unsigned char vals[MAX_N];
  unsigned char senders[MAX_N];
  unsigned int i;
  unsigned int act;

  printf("\n  Post-decide value preservation under adversarial majority:\n");

  sz = bracha87Fig4Sz(3, 10);
  for (i = 0; i < 4; ++i) senders[i] = (unsigned char)i;

  /* Decide 0; then feed phase 1 inputs whose majority is 1. */
  b = (struct bracha87Fig4 *)calloc(1, sz);
  CoinVal = 0;
  bracha87Fig4Init(b, 3, 1, 10, 0, testCoin, 0);

  for (i = 0; i < 4; ++i) vals[i] = 0;
  bracha87Fig4Round(b, 0, 4, senders, vals);
  bracha87Fig4Round(b, 1, 4, senders, vals);
  for (i = 0; i < 4; ++i) vals[i] = 0 | BRACHA87_D_FLAG;
  act = bracha87Fig4Round(b, 2, 4, senders, vals);
  check("Decide 0: decided", b->decided == 1 && b->decision == 0);
  check("Decide 0: DECIDE|BROADCAST",
        act == (BRACHA87_DECIDE | BRACHA87_BROADCAST));

  /* Phase 1 sub=0: 4 plain 1s. Buggy: b->value = majority = 1.
   * Paper-faithful: b->value preserved at decision = 0. */
  for (i = 0; i < 4; ++i) vals[i] = 1;
  act = bracha87Fig4Round(b, 3, 4, senders, vals);
  printf("    decide=0, sub=0 maj=1  : act=%u value=%u\n", act, b->value);
  check("Adversarial sub=0: BROADCAST", act == BRACHA87_BROADCAST);
  check("Adversarial sub=0: value=decision (not majority)", b->value == 0);

  /* Phase 1 sub=1: 4 plain 1s. cnt[1]*2 > B_N(=4), so n2Half fires.
   * Buggy: b->value = 1 | D_FLAG = 0x81. Paper-faithful: 0. */
  for (i = 0; i < 4; ++i) vals[i] = 1;
  act = bracha87Fig4Round(b, 4, 4, senders, vals);
  printf("    decide=0, sub=1 (d,1)? : act=%u value=0x%02x\n", act, b->value);
  check("Adversarial sub=1: BROADCAST", act == BRACHA87_BROADCAST);
  check("Adversarial sub=1: value=decision (no D_FLAG drift)",
        b->value == 0);

  /* Phase 1 sub=2: 4 d-flagged 1s. Original C explicitly assigns
   * b->value = b->decision here; new code does too. Either way 0. */
  for (i = 0; i < 4; ++i) vals[i] = 1 | BRACHA87_D_FLAG;
  act = bracha87Fig4Round(b, 5, 4, senders, vals);
  check("Adversarial sub=2: BROADCAST", act == BRACHA87_BROADCAST);
  check("Adversarial sub=2: value=decision", b->value == 0);
  check("Adversarial: decision unchanged", b->decision == 0);

  free(b);

  /* Mirror: decide 1, then feed adversarial 0-majority. */
  b = (struct bracha87Fig4 *)calloc(1, sz);
  bracha87Fig4Init(b, 3, 1, 10, 1, testCoin, 0);

  for (i = 0; i < 4; ++i) vals[i] = 1;
  bracha87Fig4Round(b, 0, 4, senders, vals);
  bracha87Fig4Round(b, 1, 4, senders, vals);
  for (i = 0; i < 4; ++i) vals[i] = 1 | BRACHA87_D_FLAG;
  bracha87Fig4Round(b, 2, 4, senders, vals);
  check("Mirror decide 1: decided", b->decided == 1 && b->decision == 1);

  for (i = 0; i < 4; ++i) vals[i] = 0;
  bracha87Fig4Round(b, 3, 4, senders, vals);
  printf("    decide=1, sub=0 maj=0  : value=%u\n", b->value);
  check("Mirror sub=0: value=decision", b->value == 1);

  bracha87Fig4Round(b, 4, 4, senders, vals);
  printf("    decide=1, sub=1 (d,0)? : value=0x%02x\n", b->value);
  check("Mirror sub=1: value=decision", b->value == 1);

  for (i = 0; i < 4; ++i) vals[i] = 0 | BRACHA87_D_FLAG;
  bracha87Fig4Round(b, 5, 4, senders, vals);
  check("Mirror sub=2: value=decision", b->value == 1);
  check("Mirror: decision unchanged", b->decision == 1);
  free(b);
}

/*
 * Test Fig4 edge cases.
 */
static void
testFig4EdgeCases(
  void
){
  struct bracha87Fig4 *b;
  unsigned long sz;
  unsigned char vals[MAX_N];
  unsigned char senders[MAX_N];
  unsigned int act;
  unsigned int i;

  printf("\n  Figure 4 edge case tests:\n");

  sz = bracha87Fig4Sz(3, 10);
  b = (struct bracha87Fig4 *)calloc(1, sz);
  CoinVal = 0;
  bracha87Fig4Init(b, 3, 1, 10, 0, testCoin, 0);

  for (i = 0; i < 4; ++i) senders[i] = (unsigned char)i;

  act = bracha87Fig4Round(0, 0, 4, senders, vals);
  check("Fig4 null instance", act == 0);

  act = bracha87Fig4Round(b, 0, 0, senders, vals);
  check("Fig4 n_msgs=0", act == 0);

  free(b);
}

/*
 * Test fig4Nfn subset majority threshold for even / odd n-t.
 *
 * Under N's tie-break-to-0 (matching Fig4Round case 0 state
 * transition), an even-nt subset that ties produces 0.  VALID^k
 * is existential over n-t subsets of VALID^{k-1}, so value 0 is
 * legitimate whenever some subset ties — i.e. cnt[0] >= (nt+1)/2
 * (uniform formula; equals nt/2 for even nt, nt/2+1 for odd).
 * Value 1 is legitimate iff cnt[1] >= nt/2+1 (strict majority).
 * Both reachable => permissive; only one => exact.
 */
static void
testFig4SubsetMajority(
  void
){
  struct bracha87Fig4 *b;
  struct bracha87Fig3 *f3;
  unsigned long sz;
  unsigned int vc;
  unsigned int a;

  printf("\n  Subset majority tests (even n-t):\n");

  /*
   * n=5, t=1, n-t=4 (even), cnt[0]=2, cnt[1]=3, n_msgs=5.
   *   Result 0 reachable: cnt[0]=2 >= (nt+1)/2 = 2.  Yes — subset
   *     {0,0,1,1} ties, tie-break to 0.
   *   Result 1 reachable: cnt[1]=3 >= nt/2+1 = 3.  Yes — subset
   *     {0,1,1,1} has strict majority 1.
   *   Both reachable => permissive; both 0 and 1 accepted.
   */
  sz = bracha87Fig4Sz(4, 10);
  b = (struct bracha87Fig4 *)calloc(1, sz);
  CoinVal = 0;
  bracha87Fig4Init(b, 4, 1, 10, 0, testCoin, 0);
  f3 = &b->fig3;

  /* Round 0: 2 zeros, 3 ones (all 5 peers) */
  bracha87Fig3Accept(f3, 0, 0, 0, &vc);  /* vc=1 */
  bracha87Fig3Accept(f3, 0, 1, 0, &vc);  /* vc=2 */
  bracha87Fig3Accept(f3, 0, 2, 1, &vc);  /* vc=3 */
  bracha87Fig3Accept(f3, 0, 3, 1, &vc);  /* vc=4, round complete */
  check("Subset: round 0 complete at 4", bracha87Fig3RoundComplete(f3, 0));
  bracha87Fig3Accept(f3, 0, 4, 1, &vc);  /* vc=5, extra validated */
  check("Subset: round 0 vc=5", vc == 5);

  /* Round 1: value=0 accepted (tie subset produces 0) */
  a = bracha87Fig3Accept(f3, 1, 0, 0, &vc);
  printf("    n=5 t=1 round 1 v=0   : a=%u vc=%u (expect valid)\n", a, vc);
  check("Subset n=5: value 0 accepted", a == BRACHA87_VALIDATED);

  /* Round 1: value=1 accepted (strict majority subset produces 1) */
  a = bracha87Fig3Accept(f3, 1, 1, 1, &vc);
  printf("    n=5 t=1 round 1 v=1   : a=%u vc=%u (expect valid)\n", a, vc);
  check("Subset n=5: value 1 accepted", a == BRACHA87_VALIDATED);

  free(b);

  /*
   * n=4, t=1, n-t=3 (odd). Regression baseline.
   * Round 1: 4 messages — sender 0 sends 0; senders 1,2,3 send 1.
   * Majority is 1. Value 0 rejected, value 1 accepted.
   */
  sz = bracha87Fig4Sz(3, 10);
  b = (struct bracha87Fig4 *)calloc(1, sz);
  CoinVal = 0;
  bracha87Fig4Init(b, 3, 1, 10, 0, testCoin, 0);
  f3 = &b->fig3;

  bracha87Fig3Accept(f3, 0, 0, 0, &vc);
  bracha87Fig3Accept(f3, 0, 1, 1, &vc);
  bracha87Fig3Accept(f3, 0, 2, 1, &vc);  /* vc=3, round complete */
  check("Subset: n=4 round 0 complete at 3", bracha87Fig3RoundComplete(f3, 0));
  bracha87Fig3Accept(f3, 0, 3, 1, &vc);  /* vc=4, extra */

  a = bracha87Fig3Accept(f3, 1, 0, 0, &vc);
  printf("    n=4 t=1 round 1 v=0   : a=%u vc=%u (expect rejected)\n", a, vc);
  check("Subset n=4: value 0 rejected", a == 0);

  a = bracha87Fig3Accept(f3, 1, 1, 1, &vc);
  printf("    n=4 t=1 round 1 v=1   : a=%u vc=%u (expect valid)\n", a, vc);
  check("Subset n=4: value 1 accepted", a == BRACHA87_VALIDATED);

  free(b);

  /*
   * n=8, t=2, n-t=6 (even), cnt[0]=3, cnt[1]=5, n_msgs=8.
   *   Result 0 reachable: cnt[0]=3 >= (nt+1)/2 = 3.  Yes — subset
   *     {0,0,0,1,1,1} ties, tie-break to 0.
   *   Result 1 reachable: cnt[1]=5 >= nt/2+1 = 4.  Yes.
   *   Both reachable => permissive; both 0 and 1 accepted.
   */
  sz = bracha87Fig4Sz(7, 10);
  b = (struct bracha87Fig4 *)calloc(1, sz);
  CoinVal = 0;
  bracha87Fig4Init(b, 7, 2, 10, 0, testCoin, 0);
  f3 = &b->fig3;

  bracha87Fig3Accept(f3, 0, 0, 0, &vc);
  bracha87Fig3Accept(f3, 0, 1, 0, &vc);
  bracha87Fig3Accept(f3, 0, 2, 0, &vc);
  bracha87Fig3Accept(f3, 0, 3, 1, &vc);
  bracha87Fig3Accept(f3, 0, 4, 1, &vc);
  bracha87Fig3Accept(f3, 0, 5, 1, &vc);  /* vc=6, round complete */
  check("Subset: n=8 round 0 complete at 6", bracha87Fig3RoundComplete(f3, 0));
  bracha87Fig3Accept(f3, 0, 6, 1, &vc);
  bracha87Fig3Accept(f3, 0, 7, 1, &vc);  /* vc=8 */

  a = bracha87Fig3Accept(f3, 1, 0, 0, &vc);
  printf("    n=8 t=2 round 1 v=0   : a=%u vc=%u (expect valid)\n", a, vc);
  check("Subset n=8: value 0 accepted", a == BRACHA87_VALIDATED);

  a = bracha87Fig3Accept(f3, 1, 1, 1, &vc);
  printf("    n=8 t=2 round 1 v=1   : a=%u vc=%u (expect valid)\n", a, vc);
  check("Subset n=8: value 1 accepted", a == BRACHA87_VALIDATED);

  free(b);

  /*
   * n=5, t=1, n-t=4 (even), cnt[0]=3, cnt[1]=2, n_msgs=5.
   *   Result 0 reachable: cnt[0]=3 >= (nt+1)/2 = 2.  Yes.
   *   Result 1 reachable: cnt[1]=2 >= nt/2+1 = 3.  No (strict majority
   *     of 1 needs at least 3 ones; only 2 available).
   *   Only 0 reachable => exact with result=0; value 1 rejected.
   */
  sz = bracha87Fig4Sz(4, 10);
  b = (struct bracha87Fig4 *)calloc(1, sz);
  CoinVal = 0;
  bracha87Fig4Init(b, 4, 1, 10, 0, testCoin, 0);
  f3 = &b->fig3;

  bracha87Fig3Accept(f3, 0, 0, 0, &vc);
  bracha87Fig3Accept(f3, 0, 1, 0, &vc);
  bracha87Fig3Accept(f3, 0, 2, 0, &vc);
  bracha87Fig3Accept(f3, 0, 3, 1, &vc);  /* round complete */
  bracha87Fig3Accept(f3, 0, 4, 1, &vc);  /* extra */

  /* cnt[0]=3, cnt[1]=2. Exact majority = 0. */
  a = bracha87Fig3Accept(f3, 1, 0, 0, &vc);
  printf("    n=5 t=1 3:2 v=0       : a=%u vc=%u (expect valid)\n", a, vc);
  check("Subset n=5 3:2: value 0 accepted", a == BRACHA87_VALIDATED);

  a = bracha87Fig3Accept(f3, 1, 1, 1, &vc);
  printf("    n=5 t=1 3:2 v=1       : a=%u vc=%u (expect rejected)\n", a, vc);
  check("Subset n=5 3:2: value 1 rejected", a == 0);

  free(b);
}

/*
 * Just-below-threshold boundary tests for fig4Nfn case 0.
 * Uses n=9 (enc 8), t=1, nt=8 (even).
 *
 *   cnt[0]=3, cnt[1]=6, n_msgs=9: cnt[0] < (nt+1)/2=4 so 0 unreachable
 *     in any subset.  Exact, value 0 rejected, value 1 accepted.
 *   cnt[0]=4, cnt[1]=4, n_msgs=8 (=nt): n_msgs not > nt so the
 *     permissive branch is not even consulted.  Exact via tie-break
 *     to 0; value 0 accepted, value 1 rejected.
 *   cnt[0]=2, cnt[1]=7, n_msgs=9: only result 1 reachable.
 */
static void
testFig4SubsetMajorityBoundary(
  void
){
  struct bracha87Fig4 *b;
  struct bracha87Fig3 *f3;
  unsigned long sz;
  unsigned int vc;
  unsigned int a;

  printf("\n  Subset majority boundary (just-below threshold):\n");

  /* Case A: nt=8, cnt[0]=3, cnt[1]=6, n_msgs=9 > nt.
   * Permissive check: cnt[0]=3 < (nt+1)/2=4 so 0 not reachable.
   *                   cnt[1]=6 >= nt/2+1=5  so 1 reachable.
   * Not permissive -> exact, result=1.  v=0 rejected, v=1 accepted. */
  sz = bracha87Fig4Sz(8, 10);
  b = (struct bracha87Fig4 *)calloc(1, sz);
  CoinVal = 0;
  bracha87Fig4Init(b, 8, 1, 10, 0, testCoin, 0);
  f3 = &b->fig3;

  bracha87Fig3Accept(f3, 0, 0, 0, &vc);
  bracha87Fig3Accept(f3, 0, 1, 0, &vc);
  bracha87Fig3Accept(f3, 0, 2, 0, &vc);
  bracha87Fig3Accept(f3, 0, 3, 1, &vc);
  bracha87Fig3Accept(f3, 0, 4, 1, &vc);
  bracha87Fig3Accept(f3, 0, 5, 1, &vc);
  bracha87Fig3Accept(f3, 0, 6, 1, &vc);
  bracha87Fig3Accept(f3, 0, 7, 1, &vc);
  bracha87Fig3Accept(f3, 0, 8, 1, &vc);  /* 9 msgs > nt=8 */

  a = bracha87Fig3Accept(f3, 1, 0, 0, &vc);
  check("Boundary 3:6@nt=8: v=0 rejected", a == 0);
  a = bracha87Fig3Accept(f3, 1, 1, 1, &vc);
  check("Boundary 3:6@nt=8: v=1 accepted", a == BRACHA87_VALIDATED);
  free(b);

  /* Case B: nt=8, cnt[0]=4, cnt[1]=4, n_msgs=8 (=nt).
   * Permissive branch not consulted.  Exact via tie-break to 0.
   * v=0 accepted, v=1 rejected. */
  b = (struct bracha87Fig4 *)calloc(1, sz);
  bracha87Fig4Init(b, 8, 1, 10, 0, testCoin, 0);
  f3 = &b->fig3;

  bracha87Fig3Accept(f3, 0, 0, 0, &vc);
  bracha87Fig3Accept(f3, 0, 1, 0, &vc);
  bracha87Fig3Accept(f3, 0, 2, 0, &vc);
  bracha87Fig3Accept(f3, 0, 3, 0, &vc);
  bracha87Fig3Accept(f3, 0, 4, 1, &vc);
  bracha87Fig3Accept(f3, 0, 5, 1, &vc);
  bracha87Fig3Accept(f3, 0, 6, 1, &vc);
  bracha87Fig3Accept(f3, 0, 7, 1, &vc);  /* exactly nt=8 */

  a = bracha87Fig3Accept(f3, 1, 0, 0, &vc);
  check("Boundary 4:4@nt=8: v=0 accepted (tie->0)", a == BRACHA87_VALIDATED);
  a = bracha87Fig3Accept(f3, 1, 1, 1, &vc);
  check("Boundary 4:4@nt=8: v=1 rejected", a == 0);
  free(b);

  /* Case C: nt=8, cnt[0]=2, cnt[1]=7, n_msgs=9 > nt.
   * Permissive check: cnt[0]=2 < 4 so 0 unreachable.  Exact, result=1.
   * v=0 rejected, v=1 accepted. */
  b = (struct bracha87Fig4 *)calloc(1, sz);
  bracha87Fig4Init(b, 8, 1, 10, 0, testCoin, 0);
  f3 = &b->fig3;

  bracha87Fig3Accept(f3, 0, 0, 0, &vc);
  bracha87Fig3Accept(f3, 0, 1, 0, &vc);
  bracha87Fig3Accept(f3, 0, 2, 1, &vc);
  bracha87Fig3Accept(f3, 0, 3, 1, &vc);
  bracha87Fig3Accept(f3, 0, 4, 1, &vc);
  bracha87Fig3Accept(f3, 0, 5, 1, &vc);
  bracha87Fig3Accept(f3, 0, 6, 1, &vc);
  bracha87Fig3Accept(f3, 0, 7, 1, &vc);
  bracha87Fig3Accept(f3, 0, 8, 1, &vc);

  a = bracha87Fig3Accept(f3, 1, 0, 0, &vc);
  check("Boundary 2:7@nt=8: v=0 rejected", a == 0);
  a = bracha87Fig3Accept(f3, 1, 1, 1, &vc);
  check("Boundary 2:7@nt=8: v=1 accepted", a == BRACHA87_VALIDATED);
  free(b);
}

/*
 * Test that D_FLAG injection is rejected when fig4Nfn denies it.
 *
 * Round 3i+1 broadcasts (validated via fig4Nfn case 0) carry the
 * step-1 majority — never D_FLAG.  An attacker injecting (q, 1,
 * v|D_FLAG) must be rejected.  Same for case 1 fall-through (no
 * majority anywhere) and case 2 permissive (round 3(i+1) outputs
 * are always plain binary).
 */
static void
testFig4DflagInjection(
  void
){
  struct bracha87Fig4 *b;
  struct bracha87Fig3 *f3;
  unsigned long sz;
  unsigned int vc;
  unsigned int a;

  printf("\n  D_FLAG injection rejection:\n");

  /* Case 0 EXACT: nt=3 (odd), all-0 round 0. Round 1 v=0|D_FLAG rejected. */
  sz = bracha87Fig4Sz(3, 10);
  b = (struct bracha87Fig4 *)calloc(1, sz);
  CoinVal = 0;
  bracha87Fig4Init(b, 3, 1, 10, 0, testCoin, 0);
  f3 = &b->fig3;

  bracha87Fig3Accept(f3, 0, 0, 0, &vc);
  bracha87Fig3Accept(f3, 0, 1, 0, &vc);
  bracha87Fig3Accept(f3, 0, 2, 0, &vc);
  a = bracha87Fig3Accept(f3, 1, 0, 0 | BRACHA87_D_FLAG, &vc);
  check("Case 0 exact: 0|D_FLAG rejected", a == 0);
  a = bracha87Fig3Accept(f3, 1, 1, 0, &vc);
  check("Case 0 exact: plain 0 accepted", a == BRACHA87_VALIDATED);
  free(b);

  /* Case 0 PERMISSIVE: nt=4 (even), cnt[0]=2 cnt[1]=3 n_msgs=5.
   * fig4Nfn case 0 returns permissive with *result=0 (no D_FLAG).
   * Round 1 v=0|D_FLAG and v=1|D_FLAG both rejected; plain 0 and 1 OK. */
  sz = bracha87Fig4Sz(4, 10);
  b = (struct bracha87Fig4 *)calloc(1, sz);
  bracha87Fig4Init(b, 4, 1, 10, 0, testCoin, 0);
  f3 = &b->fig3;

  bracha87Fig3Accept(f3, 0, 0, 0, &vc);
  bracha87Fig3Accept(f3, 0, 1, 0, &vc);
  bracha87Fig3Accept(f3, 0, 2, 1, &vc);
  bracha87Fig3Accept(f3, 0, 3, 1, &vc);
  bracha87Fig3Accept(f3, 0, 4, 1, &vc);  /* 5 msgs > nt=4 */
  a = bracha87Fig3Accept(f3, 1, 0, 0 | BRACHA87_D_FLAG, &vc);
  check("Case 0 permissive: 0|D_FLAG rejected", a == 0);
  a = bracha87Fig3Accept(f3, 1, 1, 1 | BRACHA87_D_FLAG, &vc);
  check("Case 0 permissive: 1|D_FLAG rejected", a == 0);
  a = bracha87Fig3Accept(f3, 1, 2, 0, &vc);
  check("Case 0 permissive: plain 0 accepted", a == BRACHA87_VALIDATED);
  a = bracha87Fig3Accept(f3, 1, 3, 1, &vc);
  check("Case 0 permissive: plain 1 accepted", a == BRACHA87_VALIDATED);
  free(b);

  /*
   * Case 1 PERMISSIVE — first branch (cnt[v]*2 > N, D_FLAG legitimate).
   * To exercise this we need round 1 (= step-1 broadcasts) to validate
   * with a strict majority of one value, and then submit round 2
   * messages.  fig4Nfn case 1 should return permissive with
   * *result = v|D_FLAG.  Round 2 v|D_FLAG accepted, v with no flag
   * also accepted (process-keeps-own-value path).
   *
   * n=5, t=1, nt=4.  Round 0: all-0.  Round 1 setup needs 5 msgs
   * with cnt[0]*2 > 5 (i.e. cnt[0] >= 3) AND n_msgs > nt for permissive.
   */
  sz = bracha87Fig4Sz(4, 10);
  b = (struct bracha87Fig4 *)calloc(1, sz);
  bracha87Fig4Init(b, 4, 1, 10, 0, testCoin, 0);
  f3 = &b->fig3;

  /* Round 0: 5 zeros (full set).  Validates round 1 v=0 exact. */
  bracha87Fig3Accept(f3, 0, 0, 0, &vc);
  bracha87Fig3Accept(f3, 0, 1, 0, &vc);
  bracha87Fig3Accept(f3, 0, 2, 0, &vc);
  bracha87Fig3Accept(f3, 0, 3, 0, &vc);
  bracha87Fig3Accept(f3, 0, 4, 0, &vc);
  /* Round 1: 4 v=0 (validates), then 1 more v=0 (extra). */
  bracha87Fig3Accept(f3, 1, 0, 0, &vc);
  bracha87Fig3Accept(f3, 1, 1, 0, &vc);
  bracha87Fig3Accept(f3, 1, 2, 0, &vc);
  bracha87Fig3Accept(f3, 1, 3, 0, &vc);  /* round 1 vc=4 (= nt) */
  bracha87Fig3Accept(f3, 1, 4, 0, &vc);  /* round 1 vc=5 */
  /* fig4Nfn case 1 with cnt[0]=5, N=5, n_msgs=5, nt=4:
   * cnt[0]*2=10 > 5 -> *result = 0|D_FLAG.
   * Permissive check: (5 - 1) * 2 = 8 > 5, so worst-case subset still
   * has majority -> NOT permissive, exact 0|D_FLAG.
   * Round 2 v=0|D_FLAG accepted, v=0 (no D) rejected, v=1 rejected. */
  a = bracha87Fig3Accept(f3, 2, 0, 0 | BRACHA87_D_FLAG, &vc);
  check("Case 1 exact: 0|D_FLAG accepted", a == BRACHA87_VALIDATED);
  a = bracha87Fig3Accept(f3, 2, 1, 0, &vc);
  check("Case 1 exact: plain 0 rejected", a == 0);
  a = bracha87Fig3Accept(f3, 2, 2, 1, &vc);
  check("Case 1 exact: plain 1 rejected", a == 0);
  free(b);

  /*
   * Case 1 FALL-THROUGH (no full-set majority).
   * n=5, t=1, nt=4.  Round 1: 2 zeros, 2 ones (no majority).
   * cnt[v]*2 = 4 not > 5 for either.  Permissive fall-through with
   * *result = 0 (no D_FLAG).  Round 2 v|D_FLAG must be rejected.
   */
  b = (struct bracha87Fig4 *)calloc(1, sz);
  bracha87Fig4Init(b, 4, 1, 10, 0, testCoin, 0);
  f3 = &b->fig3;

  /* Round 0: 5 zeros so round 1 v=0 validates exact. */
  bracha87Fig3Accept(f3, 0, 0, 0, &vc);
  bracha87Fig3Accept(f3, 0, 1, 0, &vc);
  bracha87Fig3Accept(f3, 0, 2, 0, &vc);
  bracha87Fig3Accept(f3, 0, 3, 0, &vc);
  bracha87Fig3Accept(f3, 0, 4, 0, &vc);
  /* Round 1: only validate 0s (Ns of round 1 says "round 1 must = 0"
   * because round 0 is all-0; so round 1 v=1 messages are rejected).
   * To get cnt[0]=cnt[1] in VALID^1 we'd need to swap N — but for
   * testing the rejection we just need the case 1 fall-through path
   * to fire.  Construct it differently: use round 0 mixed so that
   * round 1 N is permissive (case 0), accepting both 0 and 1. */
  free(b);

  /* Reset: round 0 mixed permissive so round 1 accepts both values. */
  b = (struct bracha87Fig4 *)calloc(1, sz);
  bracha87Fig4Init(b, 4, 1, 10, 0, testCoin, 0);
  f3 = &b->fig3;

  /* Round 0: cnt[0]=2 cnt[1]=3 n_msgs=5 -> permissive (case 0 fix).
   * Round 1 v=0 and v=1 both accepted. */
  bracha87Fig3Accept(f3, 0, 0, 0, &vc);
  bracha87Fig3Accept(f3, 0, 1, 0, &vc);
  bracha87Fig3Accept(f3, 0, 2, 1, &vc);
  bracha87Fig3Accept(f3, 0, 3, 1, &vc);
  bracha87Fig3Accept(f3, 0, 4, 1, &vc);
  /* Build a 4:0 round 1 (no majority lost; just need round 1 nt=4). */
  bracha87Fig3Accept(f3, 1, 0, 0, &vc);
  bracha87Fig3Accept(f3, 1, 1, 0, &vc);
  bracha87Fig3Accept(f3, 1, 2, 1, &vc);
  bracha87Fig3Accept(f3, 1, 3, 1, &vc);  /* round 1 vc=4, cnt[0]=2 cnt[1]=2 */
  /* fig4Nfn case 1 with N=5, cnt[0]=2 cnt[1]=2: neither *2 > 5.
   * Falls through to permissive, *result = 0 (no D_FLAG legitimate).
   * Round 2 v|D_FLAG must be rejected. */
  a = bracha87Fig3Accept(f3, 2, 0, 0 | BRACHA87_D_FLAG, &vc);
  check("Case 1 fall-through: 0|D_FLAG rejected", a == 0);
  a = bracha87Fig3Accept(f3, 2, 1, 1 | BRACHA87_D_FLAG, &vc);
  check("Case 1 fall-through: 1|D_FLAG rejected", a == 0);
  a = bracha87Fig3Accept(f3, 2, 2, 0, &vc);
  check("Case 1 fall-through: plain 0 accepted", a == BRACHA87_VALIDATED);
  a = bracha87Fig3Accept(f3, 2, 3, 1, &vc);
  check("Case 1 fall-through: plain 1 accepted", a == BRACHA87_VALIDATED);
  free(b);
}

/*
 * Smoke-test that bracha87Fig4Init clamps maxPhases > BRACHA87_MAX_PHASES
 * to BRACHA87_MAX_PHASES (85).  Without the clamp, maxPhases * 3 wraps in
 * unsigned char and silently corrupts the embedded Fig3 size to 2 rounds.
 */
static void
testFig4MaxPhasesClamp(
  void
){
  struct bracha87Fig4 *b;
  unsigned long sz;
  unsigned long sizeClamp;
  unsigned char senders[1];
  unsigned char values[1];
  unsigned int act;

  printf("\n  maxPhases clamp:\n");

  /* Sz called with out-of-range value should equal Sz at the cap. */
  sz = bracha87Fig4Sz(3, 100);
  sizeClamp = bracha87Fig4Sz(3, BRACHA87_MAX_PHASES);
  printf("    Sz(100)=%lu Sz(85)=%lu\n", sz, sizeClamp);
  check("maxPhases: Sz clamps at cap", sz == sizeClamp);

  /* Init with out-of-range value should not crash and should produce
   * a Fig4 with the clamped capacity (round indices 0..254 valid). */
  b = (struct bracha87Fig4 *)calloc(1, sz);
  bracha87Fig4Init(b, 3, 1, 100, 0, testCoin, 0);

  check("maxPhases: Init clamps maxPhases field",
        b->maxPhases == BRACHA87_MAX_PHASES);

  /* Smoke: round 0 with all-0 majority validates and advances. */
  CoinVal = 0;
  senders[0] = 0;
  values[0] = 0;
  /* Skip Fig3 wiring; just confirm Fig4Round operates on a non-corrupt
   * embedded Fig3 (would dereference past end if size was 2 rounds). */
  act = bracha87Fig4Round(b, 0, 0, senders, values);
  check("maxPhases: Fig4Round(0 msgs) returns 0 cleanly", act == 0);

  free(b);
}

/*
 * Test Fig4: multi-phase convergence where coin drives all to same value.
 */
static void
testFig4MultiPhase(
  void
){
  unsigned char inits[MAX_N];
  unsigned char dval;
  unsigned int a;
  unsigned int i;

  printf("\n  Multi-phase tests:\n");

  /* n=4 t=1, 2:2 split, coin=0 then coin=1 */
  for (i = 0; i < MAX_N; ++i)
    inits[i] = (i < 2) ? 0 : 1;

  CoinVal = 0;
  a = simFig4(4, 1, inits, &dval, 20);
  printf("    n=4 t=1 split coin=0   : decide %u (%u/%u)\n",
         (unsigned)dval, a, 4);
  check("Multi-phase split coin=0: all decide", a == 4);

  CoinVal = 1;
  a = simFig4(4, 1, inits, &dval, 20);
  printf("    n=4 t=1 split coin=1   : decide %u (%u/%u)\n",
         (unsigned)dval, a, 4);
  check("Multi-phase split coin=1: all decide", a == 4);

  /* n=7 t=2, 3:4 split */
  for (i = 0; i < MAX_N; ++i)
    inits[i] = (i < 3) ? 0 : 1;

  CoinVal = 0;
  a = simFig4(7, 2, inits, &dval, 20);
  printf("    n=7 t=2 split 3:4      : decide %u (%u/%u)\n",
         (unsigned)dval, a, 7);
  check("Multi-phase n=7 split: all decide", a == 7);

  /* n=10 t=3, 5:5 split */
  for (i = 0; i < MAX_N; ++i)
    inits[i] = (i < 5) ? 0 : 1;

  CoinVal = 1;
  a = simFig4(10, 3, inits, &dval, 20);
  printf("    n=10 t=3 split 5:5     : decide %u (%u/%u)\n",
         (unsigned)dval, a, 10);
  check("Multi-phase n=10 split: all decide", a == 10);
}

/*
 * Simulate Figure 4 consensus with t Byzantine peers.
 * Byzantine peers (indices 0..t-1) always broadcast byzVal.
 * In step 3, Byzantine peers send byzVal | D_FLAG.
 * Only honest peers (t..n-1) are tracked for decisions.
 */
static unsigned int
simFig4Byz(
  unsigned char n
 ,unsigned char t
 ,const unsigned char *initVals
 ,unsigned char *decidedVal
 ,unsigned char maxPhases
 ,unsigned char byzVal
){
  struct bracha87Fig4 *inst[MAX_N];
  unsigned long sz;
  unsigned int k;
  unsigned char vals[MAX_N];
  unsigned char senders[MAX_N];
  unsigned int decided;
  unsigned int i;
  unsigned int act;
  unsigned int sub;
  int anyDecided;

  sz = bracha87Fig4Sz(n - 1, maxPhases);
  /* Only allocate Fig4 for honest peers */
  for (i = 0; i < (unsigned int)t; ++i)
    inst[i] = 0;
  for (i = (unsigned int)t; i < n; ++i) {
    inst[i] = (struct bracha87Fig4 *)calloc(1, sz);
    if (!inst[i]) {
      fprintf(stderr, "simFig4Byz OoR\n");
      return (0);
    }
    bracha87Fig4Init(inst[i], n - 1, t, maxPhases, initVals[i], testCoin, 0);
  }
  for (i = 0; i < n; ++i)
    senders[i] = (unsigned char)i;

  /* Initial values: Byzantine + honest */
  for (i = 0; i < (unsigned int)t; ++i)
    vals[i] = byzVal;
  for (i = (unsigned int)t; i < n; ++i)
    vals[i] = initVals[i];

  anyDecided = 0;
  for (k = 0; k < (unsigned int)maxPhases * 3; ++k) {
    sub = k % 3;

    /* Feed round to honest peers */
    for (i = (unsigned int)t; i < n; ++i) {
      if (inst[i]->decided)
        continue;
      act = bracha87Fig4Round(inst[i], (unsigned char)k, n, senders, vals);
      if (act & BRACHA87_DECIDE)
        anyDecided = 1;
    }

    if (anyDecided)
      break;

    /* Collect values for next round */
    for (i = 0; i < (unsigned int)t; ++i) {
      /* Byzantine: in step 2 broadcasts (sub==1), add D_FLAG for step 3 */
      if (sub == 1)
        vals[i] = byzVal | BRACHA87_D_FLAG;
      else
        vals[i] = byzVal;
    }
    for (i = (unsigned int)t; i < n; ++i)
      vals[i] = inst[i]->value;
  }

  decided = 0;
  *decidedVal = 0xFF;
  for (i = (unsigned int)t; i < n; ++i) {
    if (inst[i]->decided) {
      if (*decidedVal == 0xFF)
        *decidedVal = inst[i]->decision;
      check("Fig4Byz: Theorem 2 (honest agree)",
            inst[i]->decision == *decidedVal);
      ++decided;
    }
    free(inst[i]);
  }
  return (decided);
}

static void
testFig4Byzantine(
  void
){
  unsigned char inits[MAX_N];
  unsigned char dval;
  unsigned int a;
  unsigned int i;
  unsigned int honestN;

  printf("\n  Byzantine peer tests:\n");

  /*
   * n=4, t=1: 1 Byzantine always sends 1, 3 honest start 0.
   * Honest should decide 0 (3 honest > 2t+1=3 for accept).
   */
  for (i = 0; i < MAX_N; ++i) inits[i] = 0;
  CoinVal = 0;
  honestN = 4 - 1;
  a = simFig4Byz(4, 1, inits, &dval, 20, 1);
  printf("    n=4 t=1 byz=1 hon=0   : decide %u (%u/%u)\n",
         (unsigned)dval, a, honestN);
  check("Byz n=4: honest decide", a == honestN);
  check("Byz n=4: honest decide 0", dval == 0);

  /*
   * n=4, t=1: 1 Byzantine always sends 0, 3 honest start 1.
   */
  for (i = 0; i < MAX_N; ++i) inits[i] = 1;
  CoinVal = 1;
  a = simFig4Byz(4, 1, inits, &dval, 20, 0);
  printf("    n=4 t=1 byz=0 hon=1   : decide %u (%u/%u)\n",
         (unsigned)dval, a, honestN);
  check("Byz n=4 reverse: honest decide 1", a == honestN && dval == 1);

  /*
   * n=7, t=2: 2 Byzantine always send 1, 5 honest start 0.
   */
  for (i = 0; i < MAX_N; ++i) inits[i] = 0;
  CoinVal = 0;
  honestN = 7 - 2;
  a = simFig4Byz(7, 2, inits, &dval, 20, 1);
  printf("    n=7 t=2 byz=1 hon=0   : decide %u (%u/%u)\n",
         (unsigned)dval, a, honestN);
  check("Byz n=7: honest decide", a == honestN);
  check("Byz n=7: honest decide 0", dval == 0);

  /*
   * n=10, t=3: 3 Byzantine always send 1, 7 honest start 0.
   */
  for (i = 0; i < MAX_N; ++i) inits[i] = 0;
  CoinVal = 0;
  honestN = 10 - 3;
  a = simFig4Byz(10, 3, inits, &dval, 20, 1);
  printf("    n=10 t=3 byz=1 hon=0  : decide %u (%u/%u)\n",
         (unsigned)dval, a, honestN);
  check("Byz n=10: honest decide", a == honestN);
  check("Byz n=10: honest decide 0", dval == 0);

  /*
   * n=7, t=2: honest split (3 start 0, 2 start 1), Byzantine send 1.
   * Honest should still converge.
   */
  for (i = 0; i < MAX_N; ++i) inits[i] = 0;
  inits[5] = 1;
  inits[6] = 1;
  CoinVal = 0;
  honestN = 5;
  a = simFig4Byz(7, 2, inits, &dval, 20, 1);
  printf("    n=7 t=2 hon split byz=1: decide %u (%u/%u)\n",
         (unsigned)dval, a, honestN);
  check("Byz n=7 split: honest converge", a == honestN);
}

/****************************************************************************/
/*  Deterministic composed simulation: Fig1 -> Fig3 -> Fig4                 */
/*                                                                          */
/*  Single-threaded pipeline with message queue. Each process has Fig1      */
/*  instances per (origin, round), and a Fig4 (embedding Fig3).             */
/*  Messages flow: Fig1 echo/ready/accept -> Fig3 validate ->               */
/*  Fig4 round complete -> new Fig1 broadcasts.                             */
/*  Asserts the paper's lemmas inline.                                      */
/****************************************************************************/

#define MAX_BMSGS 32768

struct bMsgQ {
  unsigned char round;
  unsigned char type;
  unsigned char from;
  unsigned char to;
  unsigned char origin;
  unsigned char value;
};

static struct bMsgQ BMsgQ[MAX_BMSGS];
static unsigned int BQhead;
static unsigned int BQtail;

static void
bqInit(
  void
){
  BQhead = BQtail = 0;
}

static void
bqPush(
  unsigned char round
 ,unsigned char type
 ,unsigned char from
 ,unsigned char to
 ,unsigned char origin
 ,unsigned char value
){
  if (BQtail >= MAX_BMSGS)
    return;
  BMsgQ[BQtail].round = round;
  BMsgQ[BQtail].type = type;
  BMsgQ[BQtail].from = from;
  BMsgQ[BQtail].to = to;
  BMsgQ[BQtail].origin = origin;
  BMsgQ[BQtail].value = value;
  ++BQtail;
}

static void
bqShuffle(
  unsigned int *seed
){
  unsigned int i;
  unsigned int n;

  n = BQtail - BQhead;
  if (n < 2)
    return;
  for (i = n - 1; i > 0; --i) {
    unsigned int j;
    struct bMsgQ tmp;

    *seed = *seed * 1103515245u + 12345u;
    j = ((*seed >> 16) & 0x7FFF) % (i + 1);
    tmp = BMsgQ[BQhead + i];
    BMsgQ[BQhead + i] = BMsgQ[BQhead + j];
    BMsgQ[BQhead + j] = tmp;
  }
}

/*
 * Per-process state for composed simulation.
 */
struct composedState {
  struct bracha87Fig1 **fig1;  /* maxRounds * n instances */
  struct bracha87Fig4 *fig4;
  struct bracha87Fig3 *fig3;   /* &fig4->fig3 */
  unsigned char nextRound;
};

/*
 * Composed simulation. Returns number of honest peers that decided.
 *
 * byzantineMask: bitmask of Byzantine peers (their messages are sent
 *   but they don't process incoming). Byzantine peers always send
 *   their initVal, optionally with equivocation via byzSplit.
 *
 * byzSplit: if non-zero, Byzantine peer 0 sends value 0 to peers
 *   [0..byzSplit-1] and value 1 to [byzSplit..n-1] for round 1.
 *
 * shuffleSeed: if non-zero, shuffle queue after each batch of enqueues.
 *
 * Inline assertions:
 *   - Step 1 broadcasts never carry D_FLAG
 *   - Lemma 2: all accepted values agree per (origin, round)
 *   - Theorem 2: all decisions agree
 */
static unsigned int
simComposed(
  unsigned char n
 ,unsigned char t
 ,const unsigned char *initVals
 ,unsigned char *decisions
 ,unsigned char maxPhases
 ,unsigned int byzantineMask
 ,unsigned char byzSplit
 ,unsigned int shuffleSeed
){
  struct composedState states[MAX_N];
  unsigned int maxRounds;
  unsigned long f1sz;
  unsigned int decided;
  unsigned int i;
  unsigned int r;
  unsigned char firstDec;
  int decAgree;

  maxRounds = (unsigned int)maxPhases * 3;
  f1sz = bracha87Fig1Sz(n - 1, 0);

  /* Allocate per-process state */
  memset(states, 0, sizeof (states));
  for (i = 0; i < n; ++i) {
    unsigned int j;

    decisions[i] = 0xFF;
    if (byzantineMask & (1u << i))
      continue;

    states[i].fig1 = (struct bracha87Fig1 **)calloc(
      maxRounds * n, sizeof (struct bracha87Fig1 *));
    states[i].fig4 = (struct bracha87Fig4 *)calloc(
      1, bracha87Fig4Sz(n - 1, maxPhases));
    if (!states[i].fig1 || !states[i].fig4)
      goto cleanup;

    for (j = 0; j < maxRounds * n; ++j) {
      states[i].fig1[j] = (struct bracha87Fig1 *)calloc(1, f1sz);
      if (!states[i].fig1[j])
        goto cleanup;
      bracha87Fig1Init(states[i].fig1[j], n - 1, t, 0);
    }
    bracha87Fig4Init(states[i].fig4, n - 1, t, maxPhases,
                     initVals[i], testCoin, 0);
    states[i].fig3 = &states[i].fig4->fig3;
    states[i].nextRound = 0;
  }

  /* Bootstrap: each process broadcasts INITIAL for round 0 */
  bqInit();
  for (i = 0; i < n; ++i) {
    unsigned int k;

    for (k = 0; k < n; ++k) {
      unsigned char v;

      if ((byzantineMask & (1u << i)) && byzSplit && i == 0)
        v = (k < byzSplit) ? 0 : 1;
      else
        v = initVals[i];
      bqPush(0, BRACHA87_INITIAL, (unsigned char)i,
             (unsigned char)k, (unsigned char)i, v);
    }
  }
  if (shuffleSeed)
    bqShuffle(&shuffleSeed);

  /* Process message queue */
  while (BQhead < BQtail) {
    struct bMsgQ *m;
    struct composedState *st;
    struct bracha87Fig1 *f1;
    unsigned char out[3];
    unsigned int nout;
    unsigned int k;
    unsigned int oldTail;
    const unsigned char *cv;

    m = &BMsgQ[BQhead++];

    /* Skip if target is Byzantine */
    if (byzantineMask & (1u << m->to))
      continue;
    if (m->round >= maxRounds
     || m->origin >= n || m->from >= n)
      continue;

    st = &states[m->to];
    f1 = st->fig1[(unsigned int)m->round * n + m->origin];
    oldTail = BQtail;
    nout = bracha87Fig1Input(f1, m->type, m->from, &m->value, out);

    for (k = 0; k < nout; ++k) {
      if (out[k] == BRACHA87_ACCEPT) {
        unsigned int vc;

        cv = bracha87Fig1Value(f1);
        if (!cv)
          continue;

        /* Lemma 2 inline: check accepted value consistency */
        /* (checked at end across all processes) */

        bracha87Fig3Accept(st->fig3, m->round,
                           m->origin, cv[0], &vc);

        /* Process completed rounds (incl. cascade) */
        while (st->nextRound < maxRounds
            && bracha87Fig3RoundComplete(st->fig3,
                                         st->nextRound)) {
          unsigned char rsnd[MAX_N];
          unsigned char rval[MAX_N];
          unsigned int rcnt;
          unsigned int act;

          rcnt = bracha87Fig3GetValid(st->fig3,
                   st->nextRound, rsnd, rval);
          act = bracha87Fig4Round(st->fig4, st->nextRound,
                                  rcnt, rsnd, rval);
          ++st->nextRound;

          if (act & BRACHA87_DECIDE) {
            decisions[m->to] = st->fig4->decision;
          }
          if ((act & BRACHA87_BROADCAST)
           && st->nextRound < maxRounds) {
            unsigned int j;

            /* Assert step 1 broadcasts have no D_FLAG */
            if (st->nextRound % 3 == 0) {
              check("Composed: step 1 no D_FLAG",
                    (st->fig4->value & BRACHA87_D_FLAG) == 0);
            }

            for (j = 0; j < n; ++j)
              bqPush(st->nextRound, BRACHA87_INITIAL,
                     m->to, (unsigned char)j,
                     m->to, st->fig4->value);
          }
        }
        continue;
      }

      /* ECHO_ALL or READY_ALL */
      if (!(byzantineMask & (1u << m->to))) {
        unsigned int j;

        cv = bracha87Fig1Value(f1);
        if (!cv)
          continue;
        for (j = 0; j < n; ++j)
          bqPush(m->round,
                 (out[k] == BRACHA87_ECHO_ALL)
                   ? BRACHA87_ECHO : BRACHA87_READY,
                 m->to, (unsigned char)j, m->origin, cv[0]);
      }
    }

    if (shuffleSeed && BQtail > oldTail)
      bqShuffle(&shuffleSeed);
  }

  /* Post-simulation assertions */

  /* Lemma 1: all honest readys agree per (origin, round) */
  for (r = 0; r < maxRounds; ++r) {
    unsigned int orig;

    for (orig = 0; orig < n; ++orig) {
      unsigned char rdVal;
      int rdSeen;
      const unsigned char *rv;

      rdVal = 0;
      rdSeen = 0;
      for (i = 0; i < n; ++i) {
        struct bracha87Fig1 *fi;

        if (byzantineMask & (1u << i))
          continue;
        fi = states[i].fig1[r * n + orig];
        if ((fi->flags & BRACHA87_F1_RDSENT)) {
          rv = bracha87Fig1Value(fi);
          if (rv) {
            if (!rdSeen) {
              rdVal = rv[0];
              rdSeen = 1;
            } else {
              check("Lemma 1: honest readys agree",
                    rv[0] == rdVal);
            }
          }
        }
      }
    }
  }

  /* Lemma 2: all honest accepts agree per (origin, round) */
  for (r = 0; r < maxRounds; ++r) {
    unsigned int orig;

    for (orig = 0; orig < n; ++orig) {
      unsigned char acVal;
      int acSeen;
      const unsigned char *av;

      acVal = 0;
      acSeen = 0;
      for (i = 0; i < n; ++i) {
        struct bracha87Fig1 *fi;

        if (byzantineMask & (1u << i))
          continue;
        fi = states[i].fig1[r * n + orig];
        if ((fi->flags & BRACHA87_F1_ACCEPTED)) {
          av = bracha87Fig1Value(fi);
          if (av) {
            if (!acSeen) {
              acVal = av[0];
              acSeen = 1;
            } else {
              check("Lemma 2: honest accepts agree",
                    av[0] == acVal);
            }
          }
        }
      }
    }
  }

  /* Theorem 2: all honest decisions agree */
  decided = 0;
  firstDec = 0xFF;
  decAgree = 1;
  for (i = 0; i < n; ++i) {
    if (byzantineMask & (1u << i))
      continue;
    if (decisions[i] != 0xFF) {
      if (firstDec == 0xFF)
        firstDec = decisions[i];
      else if (decisions[i] != firstDec)
        decAgree = 0;
      ++decided;
    }
  }
  if (decided > 1)
    check("Theorem 2: honest decisions agree", decAgree);

cleanup:
  for (i = 0; i < n; ++i) {
    if (states[i].fig1) {
      unsigned int j;

      for (j = 0; j < maxRounds * n; ++j)
        free(states[i].fig1[j]);
      free(states[i].fig1);
    }
    free(states[i].fig4);
  }

  return (decided);
}

/*
 * Composed simulation: all honest, various configurations.
 */
static void
testComposed(
  void
){
  unsigned char inits[MAX_N];
  unsigned char decs[MAX_N];
  unsigned int a;
  unsigned int i;

  printf("\nComposed simulation (Fig1+Fig3+Fig4)\n");

  /* n=4 t=1, all start 0 */
  for (i = 0; i < MAX_N; ++i) inits[i] = 0;
  CoinVal = 0;
  a = simComposed(4, 1, inits, decs, 10, 0, 0, 0);
  printf("  n=4  t=1 all-0    : decide %u (%u/4)\n",
         (unsigned)decs[0], a);
  check("Composed n=4 all-0: all decide", a == 4);
  check("Composed n=4 all-0: decide 0", decs[0] == 0);

  /* n=4 t=1, all start 1 */
  for (i = 0; i < MAX_N; ++i) inits[i] = 1;
  CoinVal = 0;
  a = simComposed(4, 1, inits, decs, 10, 0, 0, 0);
  printf("  n=4  t=1 all-1    : decide %u (%u/4)\n",
         (unsigned)decs[0], a);
  check("Composed n=4 all-1: all decide", a == 4);
  check("Composed n=4 all-1: decide 1", decs[0] == 1);

  /* n=7 t=2, all start 0 */
  for (i = 0; i < MAX_N; ++i) inits[i] = 0;
  CoinVal = 0;
  a = simComposed(7, 2, inits, decs, 10, 0, 0, 0);
  printf("  n=7  t=2 all-0    : decide %u (%u/7)\n",
         (unsigned)decs[0], a);
  check("Composed n=7 all-0: all decide", a == 7);
  check("Composed n=7 all-0: decide 0", decs[0] == 0);

  /* n=10 t=3, all start 0 */
  for (i = 0; i < MAX_N; ++i) inits[i] = 0;
  CoinVal = 0;
  a = simComposed(10, 3, inits, decs, 10, 0, 0, 0);
  printf("  n=10 t=3 all-0    : decide %u (%u/10)\n",
         (unsigned)decs[0], a);
  check("Composed n=10 all-0: all decide", a == 10);
  check("Composed n=10 all-0: decide 0", decs[0] == 0);

  /* n=4 t=1, 2:2 split, coin=0 */
  inits[0] = inits[1] = 0;
  inits[2] = inits[3] = 1;
  CoinVal = 0;
  a = simComposed(4, 1, inits, decs, 20, 0, 0, 0);
  printf("  n=4  t=1 split 2:2: decide %u (%u/4) coin=0\n",
         (unsigned)decs[0], a);
  check("Composed n=4 split: all decide", a == 4);

  /* Even n+t: n=5 t=1 */
  for (i = 0; i < MAX_N; ++i) inits[i] = 0;
  CoinVal = 0;
  a = simComposed(5, 1, inits, decs, 10, 0, 0, 0);
  printf("  n=5  t=1 all-0    : decide %u (%u/5)\n",
         (unsigned)decs[0], a);
  check("Composed n=5 all-0: all decide", a == 5);
  check("Composed n=5 all-0: decide 0", decs[0] == 0);

  /* Even n+t: n=8 t=2 */
  a = simComposed(8, 2, inits, decs, 10, 0, 0, 0);
  printf("  n=8  t=2 all-0    : decide %u (%u/8)\n",
         (unsigned)decs[0], a);
  check("Composed n=8 all-0: all decide", a == 8);
  check("Composed n=8 all-0: decide 0", decs[0] == 0);
}

/*
 * Composed simulation with shuffled message delivery.
 * Runs multiple seeds, verifies all reach agreement.
 */
static void
testComposedShuffled(
  void
){
  unsigned char inits[MAX_N];
  unsigned char decs[MAX_N];
  unsigned int a;
  unsigned int i;
  unsigned int seed;
  int allOk;

  printf("\n  Shuffled composed tests:\n");

  /* n=4 t=1, all start 0, 10 seeds */
  for (i = 0; i < MAX_N; ++i) inits[i] = 0;
  CoinVal = 0;
  allOk = 1;
  for (seed = 1; seed <= 10; ++seed) {
    a = simComposed(4, 1, inits, decs, 10, 0, 0, seed);
    if (a != 4 || decs[0] != 0)
      allOk = 0;
  }
  printf("    n=4 t=1 all-0 seed 1-10 : %s\n",
         allOk ? "all decided" : "FAIL");
  check("Composed shuffled n=4 all-0", allOk);

  /* n=7 t=2, all start 0, 10 seeds */
  for (i = 0; i < MAX_N; ++i) inits[i] = 0;
  CoinVal = 0;
  allOk = 1;
  for (seed = 1; seed <= 10; ++seed) {
    a = simComposed(7, 2, inits, decs, 10, 0, 0, seed);
    if (a != 7 || decs[0] != 0)
      allOk = 0;
  }
  printf("    n=7 t=2 all-0 seed 1-10 : %s\n",
         allOk ? "all decided" : "FAIL");
  check("Composed shuffled n=7 all-0", allOk);

  /* n=4 t=1, 2:2 split, 10 seeds */
  inits[0] = inits[1] = 0;
  inits[2] = inits[3] = 1;
  CoinVal = 0;
  allOk = 1;
  for (seed = 1; seed <= 10; ++seed) {
    a = simComposed(4, 1, inits, decs, 20, 0, 0, seed);
    if (a != 4)
      allOk = 0;
  }
  printf("    n=4 t=1 split seed 1-10 : %s\n",
         allOk ? "all decided" : "FAIL");
  check("Composed shuffled n=4 split", allOk);
}

/*
 * Byzantine equivocating origin through full pipeline.
 * Peer 0 is Byzantine: sends value 0 to some, value 1 to others.
 * Honest peers must agree (Lemma 2, Theorem 2).
 */
static void
testByzantineComposed(
  void
){
  unsigned char inits[MAX_N];
  unsigned char decs[MAX_N];
  unsigned int a;
  unsigned int i;
  unsigned int honestN;

  printf("\n  Byzantine composed tests:\n");

  /* n=4 t=1, peer 0 Byzantine, split 1:3 */
  for (i = 0; i < MAX_N; ++i) inits[i] = 0;
  CoinVal = 0;
  honestN = 3;
  a = simComposed(4, 1, inits, decs, 20, 1u, 1, 0);
  printf("    n=4  byz split 1:3  : decide %u (%u/%u)\n",
         (unsigned)decs[1], a, honestN);
  check("ByzComposed n=4 split 1:3: honest decide", a == honestN);

  /* n=7 t=2, peer 0 Byzantine, split 3:4 */
  for (i = 0; i < MAX_N; ++i) inits[i] = 0;
  CoinVal = 0;
  honestN = 6;
  a = simComposed(7, 2, inits, decs, 20, 1u, 3, 0);
  printf("    n=7  byz split 3:4  : decide %u (%u/%u)\n",
         (unsigned)decs[1], a, honestN);
  check("ByzComposed n=7 split 3:4: honest decide", a == honestN);

  /* n=7 t=2, peer 0 Byzantine, split 3:4, shuffled */
  for (i = 0; i < MAX_N; ++i) inits[i] = 0;
  CoinVal = 0;
  a = simComposed(7, 2, inits, decs, 20, 1u, 3, 42);
  printf("    n=7  byz shuffled   : decide %u (%u/%u)\n",
         (unsigned)decs[1], a, honestN);
  check("ByzComposed n=7 shuffled: honest decide", a == honestN);
}

/*
 * Post-decide multi-phase: decided process runs through 3+ additional
 * phases, verifying D_FLAG never leaks into step 1 broadcasts and
 * the decision value stays frozen.
 */
static void
testPostDecideMultiPhase(
  void
){
  struct bracha87Fig4 *b;
  unsigned long sz;
  unsigned char vals[MAX_N];
  unsigned char senders[MAX_N];
  unsigned int act;
  unsigned int i;
  unsigned int k;
  int dflagLeak;
  int decChanged;

  printf("\n  Post-decide multi-phase tests:\n");

  sz = bracha87Fig4Sz(3, 10);
  b = (struct bracha87Fig4 *)calloc(1, sz);
  CoinVal = 0;
  bracha87Fig4Init(b, 3, 1, 10, 0, testCoin, 0);

  for (i = 0; i < 4; ++i) {
    senders[i] = (unsigned char)i;
    vals[i] = 0;
  }

  /* Drive to decision at phase 0 */
  bracha87Fig4Round(b, 0, 4, senders, vals);
  bracha87Fig4Round(b, 1, 4, senders, vals);
  for (i = 0; i < 4; ++i) vals[i] = 0 | BRACHA87_D_FLAG;
  act = bracha87Fig4Round(b, 2, 4, senders, vals);
  check("MultiPhase: decided", b->decided == 1);
  check("MultiPhase: decide+broadcast",
        act == (BRACHA87_DECIDE | BRACHA87_BROADCAST));

  /* Run through phases 1, 2, 3 (rounds 3-11) */
  dflagLeak = 0;
  decChanged = 0;
  for (k = 3; k <= 11; ++k) {
    unsigned int sub;

    /* Feed uniform values matching decision */
    for (i = 0; i < 4; ++i)
      vals[i] = b->value;

    act = bracha87Fig4Round(b, (unsigned char)k, 4, senders, vals);

    sub = k % 3;

    /* After step 1 (sub was 0, now subRound is 1): no D_FLAG */
    if (sub == 0 && (b->value & BRACHA87_D_FLAG))
      dflagLeak = 1;

    /* Decision must never change */
    if (b->decision != 0)
      decChanged = 1;

    /* Must keep broadcasting */
    check("MultiPhase: keeps broadcasting",
          (act & BRACHA87_BROADCAST) != 0);
    /* Must not re-emit DECIDE */
    check("MultiPhase: no re-decide",
          (act & BRACHA87_DECIDE) == 0);
  }
  check("MultiPhase: no D_FLAG in step 1", !dflagLeak);
  check("MultiPhase: decision unchanged", !decChanged);
  printf("    3 phases post-decide : ok\n");

  free(b);
}

/*
 * Fig1 value switch: Byzantine initial commits to A, honest echo
 * quorum for B corrects the value. Full accept path verified.
 */
static void
testFig1ValueSwitch(
  void
){
  struct bracha87Fig1 *b;
  unsigned long sz;
  unsigned char out[3];
  unsigned int nout;
  unsigned char valA;
  unsigned char valB;

  printf("\n  Fig1 value switch (Byzantine initial) tests:\n");

  /* n=4, t=1, vLen=1 for consensus */
  sz = bracha87Fig1Sz(3, 0);
  b = (struct bracha87Fig1 *)calloc(1, sz);
  bracha87Fig1Init(b, 3, 1, 0);

  valA = 0;
  valB = 1;

  /* Byzantine initial: commit to A */
  nout = bracha87Fig1Input(b, BRACHA87_INITIAL, 0, &valA, out);
  check("ValSwitch: INITIAL echoes", nout >= 1);
  check("ValSwitch: committed to A", bracha87Fig1Value(b)[0] == valA);

  /* 3 echoes for B from honest peers: threshold = (4+1)/2+1 = 3 */
  bracha87Fig1Input(b, BRACHA87_ECHO, 1, &valB, out);
  bracha87Fig1Input(b, BRACHA87_ECHO, 2, &valB, out);
  nout = bracha87Fig1Input(b, BRACHA87_ECHO, 3, &valB, out);

  /* Rule 4 fires for B: ready(B) */
  check("ValSwitch: rdSent", (b->flags & BRACHA87_F1_RDSENT));
  check("ValSwitch: value switched to B",
        bracha87Fig1Value(b)[0] == valB);

  /* 3 readys for B: threshold 2t+1 = 3 */
  bracha87Fig1Input(b, BRACHA87_READY, 1, &valB, out);
  bracha87Fig1Input(b, BRACHA87_READY, 2, &valB, out);
  nout = bracha87Fig1Input(b, BRACHA87_READY, 3, &valB, out);

  check("ValSwitch: accepted", (b->flags & BRACHA87_F1_ACCEPTED));
  check("ValSwitch: accepted B",
        bracha87Fig1Value(b)[0] == valB);

  printf("    Byz initial -> honest B : accepted B\n");
  free(b);
}

/*
 * BPR (Bracha Phase Re-emitter) unit tests.
 *
 * White-box tests of the bracha87Fig1Bpr entry point against the
 * contract in bracha87Fig1.dtc's BPR section: dispatch over the
 * (reconsider) discriminator and the existing committed-state
 * flags only; outputs reuse BRACHA87_ECHO_ALL / READY_ALL with
 * no commit and no flag change.
 *
 * Configuration: n=4, t=1.  Echo threshold (n+t)/2+1 = 3, Rule 5
 * ready threshold t+1 = 2, accept threshold 2t+1 = 3.
 */
static void
testFig1Bpr(
  void
){
  struct bracha87Fig1 *b;
  unsigned long sz;
  unsigned char out[3];
  unsigned int nout;
  unsigned char val[VLEN];
  unsigned char committed[VLEN];
  unsigned int i;

  printf("\n  Fig1 BPR (Bracha Phase Re-emitter) tests:\n");

  memcpy(val, "AAAA", VLEN);

  sz = bracha87Fig1Sz(3, VLEN - 1);

  /*
   * Fresh instance: nothing committed.  BPR has no echo or ready
   * to replay; returns 0 actions.  Repeat calls are idempotent.
   */
  b = (struct bracha87Fig1 *)calloc(1, sz);
  bracha87Fig1Init(b, 3, 1, VLEN - 1);
  nout = bracha87Fig1Bpr(b, out);
  check("BPR fresh: 0 actions", nout == 0);
  check("BPR fresh: ECHOED clear", !(b->flags & BRACHA87_F1_ECHOED));
  check("BPR fresh: RDSENT clear", !(b->flags & BRACHA87_F1_RDSENT));
  nout = bracha87Fig1Bpr(b, out);
  check("BPR fresh idempotent: 0 actions", nout == 0);
  printf("    fresh                : %u actions\n", nout);
  free(b);

  /*
   * After Rule 1 (INITIAL -> ECHOED): BPR replays the echo.
   * One action, ECHO_ALL.  Repeat calls return the same.
   * State flags unchanged across BPR calls.
   */
  b = (struct bracha87Fig1 *)calloc(1, sz);
  bracha87Fig1Init(b, 3, 1, VLEN - 1);
  bracha87Fig1Input(b, BRACHA87_INITIAL, 0, val, out);
  check("BPR after Rule 1: ECHOED set", (b->flags & BRACHA87_F1_ECHOED));
  check("BPR after Rule 1: RDSENT clear", !(b->flags & BRACHA87_F1_RDSENT));

  nout = bracha87Fig1Bpr(b, out);
  check("BPR echoed: 1 action", nout == 1);
  check("BPR echoed: action is ECHO_ALL", out[0] == BRACHA87_ECHO_ALL);
  check("BPR echoed: ECHOED still set", (b->flags & BRACHA87_F1_ECHOED));
  check("BPR echoed: RDSENT still clear", !(b->flags & BRACHA87_F1_RDSENT));

  /* Idempotency under repeat */
  for (i = 0; i < 5; ++i) {
    nout = bracha87Fig1Bpr(b, out);
    check("BPR echoed: idempotent count", nout == 1);
    check("BPR echoed: idempotent action", out[0] == BRACHA87_ECHO_ALL);
  }
  printf("    after Rule 1         : %u action(s) per call, 6 calls all ECHO_ALL\n",
         (unsigned)1);
  free(b);

  /*
   * After Rule 5 fires (echoed + t+1=2 readys -> RDSENT): BPR
   * replays both echo and ready.  Two actions in order: ECHO_ALL
   * then READY_ALL.
   */
  b = (struct bracha87Fig1 *)calloc(1, sz);
  bracha87Fig1Init(b, 3, 1, VLEN - 1);
  bracha87Fig1Input(b, BRACHA87_INITIAL, 0, val, out);
  bracha87Fig1Input(b, BRACHA87_READY, 1, val, out);
  bracha87Fig1Input(b, BRACHA87_READY, 2, val, out);
  check("BPR after Rule 5: ECHOED set", (b->flags & BRACHA87_F1_ECHOED));
  check("BPR after Rule 5: RDSENT set", (b->flags & BRACHA87_F1_RDSENT));
  check("BPR after Rule 5: not yet accepted",
        !(b->flags & BRACHA87_F1_ACCEPTED));

  nout = bracha87Fig1Bpr(b, out);
  check("BPR rdSent: 2 actions", nout == 2);
  check("BPR rdSent: first ECHO_ALL", nout >= 1 && out[0] == BRACHA87_ECHO_ALL);
  check("BPR rdSent: second READY_ALL", nout >= 2 && out[1] == BRACHA87_READY_ALL);

  /* Idempotency under repeat */
  for (i = 0; i < 5; ++i) {
    nout = bracha87Fig1Bpr(b, out);
    check("BPR rdSent: idempotent count", nout == 2);
    check("BPR rdSent: idempotent ECHO_ALL", out[0] == BRACHA87_ECHO_ALL);
    check("BPR rdSent: idempotent READY_ALL", out[1] == BRACHA87_READY_ALL);
  }
  printf("    after Rule 5         : %u actions per call, 6 calls all ECHO+READY\n",
         (unsigned)2);
  free(b);

  /*
   * After Rule 4 fires (echoed + (n+t)/2+1 = 3 echoes -> RDSENT
   * with no readys yet): BPR replays both committed actions.
   * This path stresses DTC's leaf merger - the BPR rule must
   * keep type discrimination at (RDSENT=1, rdGeTPlus1=no) where
   * the paper rules produce all-zero outputs.
   */
  b = (struct bracha87Fig1 *)calloc(1, sz);
  bracha87Fig1Init(b, 3, 1, VLEN - 1);
  bracha87Fig1Input(b, BRACHA87_INITIAL, 0, val, out);
  bracha87Fig1Input(b, BRACHA87_ECHO, 1, val, out);
  bracha87Fig1Input(b, BRACHA87_ECHO, 2, val, out);
  bracha87Fig1Input(b, BRACHA87_ECHO, 3, val, out);
  check("BPR after Rule 4: ECHOED set", (b->flags & BRACHA87_F1_ECHOED));
  check("BPR after Rule 4: RDSENT set", (b->flags & BRACHA87_F1_RDSENT));
  check("BPR after Rule 4: not yet accepted",
        !(b->flags & BRACHA87_F1_ACCEPTED));
  nout = bracha87Fig1Bpr(b, out);
  check("BPR rule4 path: 2 actions", nout == 2);
  check("BPR rule4 path: ECHO_ALL first", nout >= 1 && out[0] == BRACHA87_ECHO_ALL);
  check("BPR rule4 path: READY_ALL second", nout >= 2 && out[1] == BRACHA87_READY_ALL);
  printf("    after Rule 4 (no readys): %u actions, ECHO+READY\n", nout);
  free(b);

  /*
   * After Rule 6 fires (2t+1 = 3 readys -> ACCEPTED): BPR keeps
   * replaying ECHO + READY.  Bracha eventual delivery (Lemma 4)
   * requires the accepting peer to keep helping peers still
   * below the 2t+1 threshold cross it; replay is the mechanism
   * for that under fair-loss.  An accepted-implies-silent gate
   * would strand slower peers (gap 3 of the Apr-26 BPR review).
   * The application's silence-quorum exit retires the instance.
   */
  b = (struct bracha87Fig1 *)calloc(1, sz);
  bracha87Fig1Init(b, 3, 1, VLEN - 1);
  bracha87Fig1Input(b, BRACHA87_INITIAL, 0, val, out);
  bracha87Fig1Input(b, BRACHA87_READY, 1, val, out);
  bracha87Fig1Input(b, BRACHA87_READY, 2, val, out);
  bracha87Fig1Input(b, BRACHA87_READY, 3, val, out);
  check("BPR after Rule 6: ACCEPTED set",
        (b->flags & BRACHA87_F1_ACCEPTED));

  nout = bracha87Fig1Bpr(b, out);
  check("BPR post-accept: 2 actions (helping slow peers)", nout == 2);
  check("BPR post-accept: ECHO_ALL first",
        nout >= 1 && out[0] == BRACHA87_ECHO_ALL);
  check("BPR post-accept: READY_ALL second",
        nout >= 2 && out[1] == BRACHA87_READY_ALL);
  for (i = 0; i < 3; ++i) {
    nout = bracha87Fig1Bpr(b, out);
    check("BPR post-accept: idempotent count", nout == 2);
    check("BPR post-accept: idempotent ECHO_ALL", out[0] == BRACHA87_ECHO_ALL);
    check("BPR post-accept: idempotent READY_ALL", out[1] == BRACHA87_READY_ALL);
  }
  printf("    after Rule 6         : %u actions (post-accept ECHO+READY replay)\n", nout);
  free(b);

  /*
   * Value preservation across BPR calls.  Drive to RDSENT, then
   * call BPR repeatedly.  bracha87Fig1Value content must be
   * byte-identical to the originally committed value - BPR must
   * not re-commit, must not mutate the value buffer.
   */
  b = (struct bracha87Fig1 *)calloc(1, sz);
  bracha87Fig1Init(b, 3, 1, VLEN - 1);
  bracha87Fig1Input(b, BRACHA87_INITIAL, 0, val, out);
  bracha87Fig1Input(b, BRACHA87_READY, 1, val, out);
  bracha87Fig1Input(b, BRACHA87_READY, 2, val, out);
  memcpy(committed, bracha87Fig1Value(b), VLEN);
  for (i = 0; i < 10; ++i)
    bracha87Fig1Bpr(b, out);
  check("BPR value preservation: committed value intact",
        memcmp(bracha87Fig1Value(b), committed, VLEN) == 0);
  check("BPR value preservation: matches input",
        memcmp(bracha87Fig1Value(b), val, VLEN) == 0);
  printf("    value preservation   : 10 BPR calls, value byte-identical\n");
  free(b);

  /*
   * Cross-talk discipline: bracha87Fig1Input now also computes
   * the BPR replay outputs (the merged dispatch produces all
   * five), but the wrapper discards them on the Input path so
   * Bracha's emission count is unaffected.  Verified directly
   * here for two cases: (a) Input call that fires a paper rule,
   * Bracha's action count is exactly the paper count - no stray
   * replay piggy-backed; (b) Input call after committed state
   * exists but no paper rule fires, BPR's would-be replay is
   * silently discarded - Input returns 0 actions.
   */
  b = (struct bracha87Fig1 *)calloc(1, sz);
  bracha87Fig1Init(b, 3, 1, VLEN - 1);
  /* (a) Rule 1 fires alone: pre-state echoed=0, BPR replay is
   * inhibited by its chained input "send (echo, v) = yes" - the
   * dispatch produces sendEcho=yes and replayEcho=no together. */
  nout = bracha87Fig1Input(b, BRACHA87_INITIAL, 0, val, out);
  check("BPR cross-talk: Rule 1 emits exactly 1 action",
        nout == 1 && out[0] == BRACHA87_ECHO_ALL);
  /* (b) Subsequent ECHO from peer 1 with no threshold met: no
   * Bracha rule fires.  BPR replay would say yes (echoed=1,
   * sendEcho=no), but the Input wrapper discards it. */
  nout = bracha87Fig1Input(b, BRACHA87_ECHO, 1, val, out);
  check("BPR cross-talk: non-firing Input emits 0 actions",
        nout == 0);
  printf("    cross-talk discard   : Input emits paper count, BPR replay ignored\n");
  free(b);

  /*
   * Interleaved Input + BPR: insert BPR calls between every
   * message arrival.  Bracha's rules must still fire on the same
   * messages and accept must occur at the same point as a pure-
   * Input run.  This pins the property that BPR rules are silent
   * (don't fire send/ready/accept) when entered via Input - i.e.
   * no cross-talk between the two entry points.
   */
  b = (struct bracha87Fig1 *)calloc(1, sz);
  bracha87Fig1Init(b, 3, 1, VLEN - 1);

  bracha87Fig1Bpr(b, out);     /* fresh */
  bracha87Fig1Input(b, BRACHA87_INITIAL, 0, val, out);  /* Rule 1 */
  bracha87Fig1Bpr(b, out);     /* echoed */
  bracha87Fig1Input(b, BRACHA87_READY, 1, val, out);
  bracha87Fig1Bpr(b, out);
  bracha87Fig1Input(b, BRACHA87_READY, 2, val, out);   /* Rule 5 */
  check("BPR interleaved: RDSENT after 2nd READY",
        (b->flags & BRACHA87_F1_RDSENT));
  bracha87Fig1Bpr(b, out);     /* rdSent */
  bracha87Fig1Input(b, BRACHA87_READY, 3, val, out);   /* Rule 6 */
  check("BPR interleaved: ACCEPTED after 3rd READY",
        (b->flags & BRACHA87_F1_ACCEPTED));
  check("BPR interleaved: value matches input",
        memcmp(bracha87Fig1Value(b), val, VLEN) == 0);
  nout = bracha87Fig1Bpr(b, out);
  check("BPR interleaved: post-accept BPR returns 2 (ECHO+READY)",
        nout == 2);
  printf("    interleaved          : Input+BPR sequence, accept at expected point\n");
  free(b);

  /*
   * Originator INITIAL replay (gap 4 of the Apr-26 BPR review).
   *
   * bracha87Fig1Origin sets BRACHA87_F1_ORIGIN and stores the
   * value to be broadcast.  Until Rule 1, 2, or 3 sets ECHOED
   * (loopback or echo / ready cascade), bracha87Fig1Bpr emits
   * BRACHA87_INITIAL_ALL on every call so the originator's
   * (initial, v) survives loss to all other honest peers.
   *
   * Once ECHOED is set the INITIAL replay drops; ECHO replay
   * carries the value forward (Rule 2 lets any peer with > (n+t)/2
   * echoes echo on its own, so origin's INITIAL is redundant).
   */
  b = (struct bracha87Fig1 *)calloc(1, sz);
  bracha87Fig1Init(b, 3, 1, VLEN - 1);
  bracha87Fig1Origin(b, val);
  check("BPR origin: ORIGIN flag set",
        (b->flags & BRACHA87_F1_ORIGIN));
  check("BPR origin: ECHOED clear pre-loopback",
        !(b->flags & BRACHA87_F1_ECHOED));
  check("BPR origin: Value() returns committed value",
        bracha87Fig1Value(b)
        && memcmp(bracha87Fig1Value(b), val, VLEN) == 0);

  nout = bracha87Fig1Bpr(b, out);
  check("BPR origin pre-loopback: 1 action", nout == 1);
  check("BPR origin pre-loopback: action is INITIAL_ALL",
        out[0] == BRACHA87_INITIAL_ALL);

  /* Idempotency under repeat */
  for (i = 0; i < 5; ++i) {
    nout = bracha87Fig1Bpr(b, out);
    check("BPR origin pre-loopback: idempotent count", nout == 1);
    check("BPR origin pre-loopback: idempotent INITIAL_ALL",
          out[0] == BRACHA87_INITIAL_ALL);
  }

  /* Now self-feed (loopback): Rule 1 fires, ECHOED gets set.
   * INITIAL replay continues (BPR rule, not gated by ECHOED) AND
   * ECHO replay starts. */
  bracha87Fig1Input(b, BRACHA87_INITIAL, 0, val, out);
  check("BPR origin post-loopback: ECHOED set",
        (b->flags & BRACHA87_F1_ECHOED));
  check("BPR origin post-loopback: ORIGIN still set",
        (b->flags & BRACHA87_F1_ORIGIN));

  nout = bracha87Fig1Bpr(b, out);
  check("BPR origin post-loopback: 2 actions (INITIAL+ECHO replay)",
        nout == 2);
  check("BPR origin post-loopback: INITIAL_ALL first",
        nout >= 1 && out[0] == BRACHA87_INITIAL_ALL);
  check("BPR origin post-loopback: ECHO_ALL second",
        nout >= 2 && out[1] == BRACHA87_ECHO_ALL);

  printf("    origin INITIAL replay: pre-loopback INITIAL_ALL, post-loopback INITIAL+ECHO\n");
  free(b);

  /*
   * Origin + advanced-state matrix.  An originator's Fig1 can
   * progress through every committed-state combination (echoed
   * via Rule 1, rdSent via Rule 4 / 5, accepted via Rule 6) just
   * like a non-origin Fig1.  Verify Bpr replay outputs at each
   * combination match the non-origin replay rules: once ECHOED,
   * INITIAL replay stops; ECHO replay takes over; RDSENT adds
   * READY replay; ACCEPTED keeps both (gap 3).
   */
  b = (struct bracha87Fig1 *)calloc(1, sz);
  bracha87Fig1Init(b, 3, 1, VLEN - 1);
  bracha87Fig1Origin(b, val);
  bracha87Fig1Input(b, BRACHA87_INITIAL, 0, val, out);  /* Rule 1 -> ECHOED */
  bracha87Fig1Input(b, BRACHA87_READY, 1, val, out);
  bracha87Fig1Input(b, BRACHA87_READY, 2, val, out);    /* Rule 5 -> RDSENT */
  check("BPR origin+rdSent: ORIGIN set",
        (b->flags & BRACHA87_F1_ORIGIN));
  check("BPR origin+rdSent: ECHOED set",
        (b->flags & BRACHA87_F1_ECHOED));
  check("BPR origin+rdSent: RDSENT set",
        (b->flags & BRACHA87_F1_RDSENT));
  check("BPR origin+rdSent: not yet accepted",
        !(b->flags & BRACHA87_F1_ACCEPTED));

  nout = bracha87Fig1Bpr(b, out);
  check("BPR origin+rdSent: 3 actions (INITIAL+ECHO+READY)", nout == 3);
  check("BPR origin+rdSent: INITIAL_ALL first",
        nout >= 1 && out[0] == BRACHA87_INITIAL_ALL);
  check("BPR origin+rdSent: ECHO_ALL second",
        nout >= 2 && out[1] == BRACHA87_ECHO_ALL);
  check("BPR origin+rdSent: READY_ALL third",
        nout >= 3 && out[2] == BRACHA87_READY_ALL);
  free(b);

  /* Origin + ACCEPTED (Rule 6 fires).  Same outputs: ECHO+READY,
   * no INITIAL.  Asserts gap 3 fix applies symmetrically to
   * originator instances. */
  b = (struct bracha87Fig1 *)calloc(1, sz);
  bracha87Fig1Init(b, 3, 1, VLEN - 1);
  bracha87Fig1Origin(b, val);
  bracha87Fig1Input(b, BRACHA87_INITIAL, 0, val, out);  /* Rule 1 -> ECHOED */
  bracha87Fig1Input(b, BRACHA87_READY, 1, val, out);
  bracha87Fig1Input(b, BRACHA87_READY, 2, val, out);    /* Rule 5 -> RDSENT */
  bracha87Fig1Input(b, BRACHA87_READY, 3, val, out);    /* Rule 6 -> ACCEPTED */
  check("BPR origin+accepted: ORIGIN set",
        (b->flags & BRACHA87_F1_ORIGIN));
  check("BPR origin+accepted: ACCEPTED set",
        (b->flags & BRACHA87_F1_ACCEPTED));

  nout = bracha87Fig1Bpr(b, out);
  check("BPR origin+accepted: 3 actions (INITIAL+ECHO+READY)", nout == 3);
  check("BPR origin+accepted: INITIAL_ALL first",
        nout >= 1 && out[0] == BRACHA87_INITIAL_ALL);
  check("BPR origin+accepted: ECHO_ALL second",
        nout >= 2 && out[1] == BRACHA87_ECHO_ALL);
  check("BPR origin+accepted: READY_ALL third",
        nout >= 3 && out[2] == BRACHA87_READY_ALL);
  for (i = 0; i < 5; ++i) {
    nout = bracha87Fig1Bpr(b, out);
    check("BPR origin+accepted: idempotent count", nout == 3);
    check("BPR origin+accepted: idempotent INITIAL_ALL",
          out[0] == BRACHA87_INITIAL_ALL);
    check("BPR origin+accepted: idempotent ECHO_ALL",
          out[1] == BRACHA87_ECHO_ALL);
    check("BPR origin+accepted: idempotent READY_ALL",
          out[2] == BRACHA87_READY_ALL);
  }
  printf("    origin state matrix  : ORIGIN+RDSENT and ORIGIN+ACCEPTED emit INITIAL+ECHO+READY\n");
  free(b);

  /*
   * Defensive: NULL pointer guards and re-Origin idempotency.
   * Origin called twice with different values is "user error
   * but defined" -- the second call overwrites the stored value
   * and clears no flags.  This mirrors the API doc and guards
   * the case where an application Init+Origin sequence is
   * re-driven (e.g. epoch restart).
   */
  b = (struct bracha87Fig1 *)calloc(1, sz);
  bracha87Fig1Init(b, 3, 1, VLEN - 1);

  bracha87Fig1Origin(0, val);  /* NULL b -> no crash */
  bracha87Fig1Origin(b, 0);    /* NULL value -> no crash, no flag set */
  check("BPR origin NULL value: ORIGIN flag NOT set",
        !(b->flags & BRACHA87_F1_ORIGIN));

  /* Real Origin call works */
  bracha87Fig1Origin(b, val);
  check("BPR origin defensive: ORIGIN set after real call",
        (b->flags & BRACHA87_F1_ORIGIN));

  /* Re-Origin overwrites value */
  {
    unsigned char val2[VLEN];
    memcpy(val2, "BBBB", VLEN);
    bracha87Fig1Origin(b, val2);
    check("BPR origin re-call: value overwritten",
          memcmp(bracha87Fig1Value(b), val2, VLEN) == 0);
  }

  /* NULL out to Bpr -> 0 actions, no crash */
  nout = bracha87Fig1Bpr(b, 0);
  check("BPR NULL out: 0 actions", nout == 0);

  /* NULL b to Bpr -> 0 actions, no crash */
  nout = bracha87Fig1Bpr(0, out);
  check("BPR NULL b: 0 actions", nout == 0);

  printf("    defensive guards     : NULL ptrs handled, re-Origin overwrites value\n");
  free(b);
}

/*
 * Test echo threshold with even n+t.
 *
 * The echo quorum must be strictly greater than (n+t)/2 to guarantee
 * quorum intersection contains at least one correct process (Lemma 1).
 * In integer arithmetic: threshold = (n+t)/2 + 1.
 *
 * For n=3t+1 (n+t odd), ceil((n+t)/2) == (n+t)/2+1 — no difference.
 * For n=3t+2 (n+t even), ceil((n+t)/2) == (n+t)/2, which is too low.
 *
 * n=5, t=1: n+t=6. Correct threshold = 6/2+1 = 4 (not ceil(6/2)=3).
 * n=8, t=2: n+t=10. Correct threshold = 10/2+1 = 6 (not ceil(10/2)=5).
 */
static void
testFig1EvenNplusT(
  void
){
  struct bracha87Fig1 *b;
  unsigned long sz;
  unsigned char out[3];
  unsigned int nout;
  unsigned char val_A[VLEN];
  unsigned char val_B[VLEN];
  unsigned int a;
  unsigned char aval[VLEN];

  memcpy(val_A, "AAAA", VLEN);
  memcpy(val_B, "BBBB", VLEN);

  printf("\n  Even n+t echo threshold tests:\n");

  /*
   * n=5, t=1: echo threshold must be 4, not 3.
   * Unit test: 3 echoes must NOT trigger Rule 2.
   */
  sz = bracha87Fig1Sz(4, VLEN - 1);
  b = (struct bracha87Fig1 *)calloc(1, sz);
  bracha87Fig1Init(b, 4, 1, VLEN - 1);

  bracha87Fig1Input(b, BRACHA87_ECHO, 0, val_A, out);
  bracha87Fig1Input(b, BRACHA87_ECHO, 1, val_A, out);
  nout = bracha87Fig1Input(b, BRACHA87_ECHO, 2, val_A, out);
  printf("    n=5 t=1: 3 echoes     : echoed=%u\n", !!(b->flags & BRACHA87_F1_ECHOED));
  check("EvenNT n=5: 3 echoes must NOT trigger echo (threshold=4)",
        !(b->flags & BRACHA87_F1_ECHOED) && nout == 0);

  /* 4th echo reaches threshold */
  nout = bracha87Fig1Input(b, BRACHA87_ECHO, 3, val_A, out);
  printf("    n=5 t=1: 4 echoes     : echoed=%u\n", !!(b->flags & BRACHA87_F1_ECHOED));
  check("EvenNT n=5: 4th echo triggers Rule 2",
        (b->flags & BRACHA87_F1_ECHOED) && nout >= 1 && out[0] == BRACHA87_ECHO_ALL);
  free(b);

  /*
   * n=5, t=1: Rule 4 (echo->ready) also needs 4.
   * INITIAL sets echoed, then echoes must reach 4.
   */
  b = (struct bracha87Fig1 *)calloc(1, sz);
  bracha87Fig1Init(b, 4, 1, VLEN - 1);

  bracha87Fig1Input(b, BRACHA87_INITIAL, 0, val_A, out);
  check("EvenNT Rule4 setup: echoed", (b->flags & BRACHA87_F1_ECHOED));

  bracha87Fig1Input(b, BRACHA87_ECHO, 1, val_A, out);
  bracha87Fig1Input(b, BRACHA87_ECHO, 2, val_A, out);
  nout = bracha87Fig1Input(b, BRACHA87_ECHO, 3, val_A, out);
  check("EvenNT n=5: 3 echoes after INITIAL, no ready",
        !(b->flags & BRACHA87_F1_RDSENT));

  nout = bracha87Fig1Input(b, BRACHA87_ECHO, 4, val_A, out);
  printf("    n=5 t=1: Rule 4 at 4  : rdSent=%u\n", !!(b->flags & BRACHA87_F1_RDSENT));
  check("EvenNT n=5: 4th echo triggers Rule 4",
        (b->flags & BRACHA87_F1_RDSENT));
  free(b);

  /*
   * n=8, t=2: echo threshold must be 6, not 5. n+t=10 (even).
   */
  sz = bracha87Fig1Sz(7, VLEN - 1);
  b = (struct bracha87Fig1 *)calloc(1, sz);
  bracha87Fig1Init(b, 7, 2, VLEN - 1);

  {
    unsigned int i;
    for (i = 0; i < 5; ++i)
      bracha87Fig1Input(b, BRACHA87_ECHO, (unsigned char)i, val_A, out);
  }
  check("EvenNT n=8: 5 echoes must NOT trigger echo (threshold=6)",
        !(b->flags & BRACHA87_F1_ECHOED));

  nout = bracha87Fig1Input(b, BRACHA87_ECHO, 5, val_A, out);
  printf("    n=8 t=2: 6 echoes     : echoed=%u\n", !!(b->flags & BRACHA87_F1_ECHOED));
  check("EvenNT n=8: 6th echo triggers Rule 2",
        (b->flags & BRACHA87_F1_ECHOED) && nout >= 1 && out[0] == BRACHA87_ECHO_ALL);
  free(b);

  /*
   * Simulation: all-honest n=5 t=1. All must accept.
   */
  a = simFig1(5, 1, 0, val_A, 0);
  printf("    n=5 t=1 honest sim    : accept %u/5\n", a);
  check("EvenNT n=5 honest: all accept", a == 5);

  /* All-honest n=8 t=2 */
  a = simFig1(8, 2, 0, val_A, 0);
  printf("    n=8 t=2 honest sim    : accept %u/8\n", a);
  check("EvenNT n=8 honest: all accept", a == 8);

  /* n=5 t=1 with 1 silent peer: all should still accept */
  a = simFig1(5, 1, 0, val_A, 1u << 4);
  printf("    n=5 t=1 1-silent sim  : accept %u/5\n", a);
  check("EvenNT n=5 1-silent: all accept", a == 5);

  /* n=8 t=2 with 2 silent peers */
  a = simFig1(8, 2, 0, val_A, (1u << 6) | (1u << 7));
  printf("    n=8 t=2 2-silent sim  : accept %u/8\n", a);
  check("EvenNT n=8 2-silent: all accept", a == 8);

  /*
   * Equivocation with even n+t: Lemma 2 must hold.
   * n=5 t=1, origin=4 (Byzantine), split at 2.
   */
  a = simFig1Equivoc(5, 1, 4, val_A, val_B, 2, aval);
  printf("    n=5 t=1 equivoc 2:3   : accept %u/5", a);
  if (a > 0) printf(" value=%c%c%c%c", aval[0], aval[1], aval[2], aval[3]);
  printf("\n");

  /* n=8 t=2, origin=7, split at 3 */
  a = simFig1Equivoc(8, 2, 7, val_A, val_B, 3, aval);
  printf("    n=8 t=2 equivoc 3:5   : accept %u/8", a);
  if (a > 0) printf(" value=%c%c%c%c", aval[0], aval[1], aval[2], aval[3]);
  printf("\n");
}


/*************************************************************************/
/*  Main — sequential test cases                                         */
/*************************************************************************/

int
main(
  void
){
  unsigned char val[VLEN];
  unsigned int a;
  unsigned char dval;
  char label[80];


  printf("bracha87 test suite\n");
  printf("===================\n\n");

  /*
   * Figure 1 — Reliable Broadcast
   *
   * All-honest tests: every peer should accept.
   */
  memcpy(val, "TEST", VLEN);

  printf("Figure 1 — Reliable Broadcast\n");

  /* n=1 t=0 */
  a = simFig1(1, 0, 0, val, 0);
  printf("  n=1  t=0          : accept %u/1\n", a);
  check("Fig1 n=1 t=0", a == 1);

  /* n=2 t=0 */
  a = simFig1(2, 0, 0, val, 0);
  printf("  n=2  t=0          : accept %u/2\n", a);
  check("Fig1 n=2 t=0", a == 2);

  /* n=3 t=0 */
  a = simFig1(3, 0, 0, val, 0);
  printf("  n=3  t=0          : accept %u/3\n", a);
  check("Fig1 n=3 t=0", a == 3);

  /* n=4 t=1 */
  a = simFig1(4, 1, 0, val, 0);
  printf("  n=4  t=1          : accept %u/4\n", a);
  check("Fig1 n=4 t=1", a == 4);

  /* n=7 t=2 */
  a = simFig1(7, 2, 0, val, 0);
  printf("  n=7  t=2          : accept %u/7\n", a);
  check("Fig1 n=7 t=2", a == 7);

  /* n=10 t=3 */
  a = simFig1(10, 3, 0, val, 0);
  printf("  n=10 t=3          : accept %u/10\n", a);
  check("Fig1 n=10 t=3", a == 10);

  /* n=13 t=4 */
  a = simFig1(13, 4, 0, val, 0);
  printf("  n=13 t=4          : accept %u/13\n", a);
  check("Fig1 n=13 t=4", a == 13);

  /* Even n+t configurations */
  /* n=5 t=1 (n+t=6) */
  a = simFig1(5, 1, 0, val, 0);
  printf("  n=5  t=1          : accept %u/5\n", a);
  check("Fig1 n=5 t=1", a == 5);

  /* n=8 t=2 (n+t=10) */
  a = simFig1(8, 2, 0, val, 0);
  printf("  n=8  t=2          : accept %u/8\n", a);
  check("Fig1 n=8 t=2", a == 8);

  /*
   * Silent peer tests: one peer's outbound messages are dropped.
   * All peers should still accept (Lemma 3/4).
   */
  printf("\n  Silent peer tests:\n");

  /* n=4 t=1, peer 3 silent */
  a = simFig1(4, 1, 0, val, 1u << 3);
  printf("  n=4  t=1 1-silent : accept %u/4\n", a);
  check("Fig1 n=4 t=1 1-silent", a == 4);

  /* n=7 t=2, peers 5,6 silent */
  a = simFig1(7, 2, 0, val, (1u << 5) | (1u << 6));
  printf("  n=7  t=2 2-silent : accept %u/7\n", a);
  check("Fig1 n=7 t=2 2-silent", a == 7);

  /* n=10 t=3, peers 7,8,9 silent */
  a = simFig1(10, 3, 0, val, (1u << 7) | (1u << 8) | (1u << 9));
  printf("  n=10 t=3 3-silent : accept %u/10\n", a);
  check("Fig1 n=10 t=3 3-silent", a == 10);

  /*
   * Equivocating origin tests (Byzantine origin sends different values).
   * Lemma 2: if any two correct processes accept, they accept the same value.
   */
  printf("\n  Equivocating origin tests:\n");

  {
    unsigned char val1[VLEN], val2[VLEN], aval[VLEN];

    memcpy(val1, "AAAA", VLEN);
    memcpy(val2, "BBBB", VLEN);

    /* n=4 t=1, origin=3 (Byzantine), split at 2: peers 0,1 get val1; 2,3 get val2 */
    a = simFig1Equivoc(4, 1, 3, val1, val2, 2, aval);
    printf("  n=4  t=1 equivoc  : accept %u/4", a);
    if (a > 0) printf(" value=%c%c%c%c", aval[0], aval[1], aval[2], aval[3]);
    printf("\n");
    /* With equivocation, some or all may accept, but Lemma 2 holds */

    /* n=7 t=2, origin=6, split at 3 */
    a = simFig1Equivoc(7, 2, 6, val1, val2, 3, aval);
    printf("  n=7  t=2 equivoc  : accept %u/7", a);
    if (a > 0) printf(" value=%c%c%c%c", aval[0], aval[1], aval[2], aval[3]);
    printf("\n");

    /* n=10 t=3, origin=9, split at 5 */
    a = simFig1Equivoc(10, 3, 9, val1, val2, 5, aval);
    printf("  n=10 t=3 equivoc  : accept %u/10", a);
    if (a > 0) printf(" value=%c%c%c%c", aval[0], aval[1], aval[2], aval[3]);
    printf("\n");
  }

  /*
   * Unit-level rule tests
   */
  testFig1Rules();
  testFig1Cascade();
  testFig1Dedup();
  testFig1EdgeCases();
  testFig1Thresholds();
  testFig1Liveness();
  testFig1AsymEquivoc();
  testFig1Shuffled();
  testFig1EvenNplusT();

  /*
   * Figure 4 — Consensus
   *
   * All-honest, all peers see all n messages per round.
   */
  printf("\nFigure 4 — Consensus\n");

  {
    unsigned char inits[MAX_N];
    unsigned int i;

    /* All start with 0: should decide 0 */
    for (i = 0; i < MAX_N; ++i) inits[i] = 0;

    CoinVal = 0;

    a = simFig4(4, 1, inits, &dval, 10);
    sprintf(label, "Fig4 n=4 t=1 all-0: decide %u", (unsigned)dval);
    printf("  n=4  t=1 all-0    : decide %u (%u/%u)\n", (unsigned)dval, a, 4);
    check(label, a == 4 && dval == 0);

    a = simFig4(7, 2, inits, &dval, 10);
    sprintf(label, "Fig4 n=7 t=2 all-0: decide %u", (unsigned)dval);
    printf("  n=7  t=2 all-0    : decide %u (%u/%u)\n", (unsigned)dval, a, 7);
    check(label, a == 7 && dval == 0);

    a = simFig4(10, 3, inits, &dval, 10);
    sprintf(label, "Fig4 n=10 t=3 all-0: decide %u", (unsigned)dval);
    printf("  n=10 t=3 all-0    : decide %u (%u/%u)\n", (unsigned)dval, a, 10);
    check(label, a == 10 && dval == 0);

    /* All start with 1: should decide 1 (Lemma 9) */
    for (i = 0; i < MAX_N; ++i) inits[i] = 1;

    a = simFig4(4, 1, inits, &dval, 10);
    sprintf(label, "Fig4 n=4 t=1 all-1: decide %u", (unsigned)dval);
    printf("  n=4  t=1 all-1    : decide %u (%u/%u)\n", (unsigned)dval, a, 4);
    check(label, a == 4 && dval == 1);

    a = simFig4(7, 2, inits, &dval, 10);
    sprintf(label, "Fig4 n=7 t=2 all-1: decide %u", (unsigned)dval);
    printf("  n=7  t=2 all-1    : decide %u (%u/%u)\n", (unsigned)dval, a, 7);
    check(label, a == 7 && dval == 1);

    /* Mixed inputs: majority 0 */
    for (i = 0; i < MAX_N; ++i) inits[i] = 0;
    inits[0] = 1; /* 1 dissenter out of 4 */

    a = simFig4(4, 1, inits, &dval, 10);
    printf("  n=4  t=1 mixed 1:3: decide %u (%u/%u)\n", (unsigned)dval, a, 4);
    check("Fig4 n=4 t=1 mixed 1:3", a == 4 && dval == 0);

    /* Mixed inputs: majority 1 */
    for (i = 0; i < MAX_N; ++i) inits[i] = 1;
    inits[0] = 0; /* 1 dissenter out of 4 */

    a = simFig4(4, 1, inits, &dval, 10);
    printf("  n=4  t=1 mixed 3:1: decide %u (%u/%u)\n", (unsigned)dval, a, 4);
    check("Fig4 n=4 t=1 mixed 3:1", a == 4 && dval == 1);

    /* Even split: 2:2 */
    for (i = 0; i < MAX_N; ++i) inits[i] = (i < 2) ? 0 : 1;

    CoinVal = 0;
    a = simFig4(4, 1, inits, &dval, 10);
    printf("  n=4  t=1 split 2:2: decide %u (%u/%u) coin=0\n", (unsigned)dval, a, 4);
    check("Fig4 n=4 t=1 split coin=0", a == 4);

    CoinVal = 1;
    a = simFig4(4, 1, inits, &dval, 10);
    printf("  n=4  t=1 split 2:2: decide %u (%u/%u) coin=1\n", (unsigned)dval, a, 4);
    check("Fig4 n=4 t=1 split coin=1", a == 4);

    /* Larger: n=7 t=2, mixed 3:4 */
    for (i = 0; i < MAX_N; ++i) inits[i] = (i < 3) ? 0 : 1;

    CoinVal = 0;
    a = simFig4(7, 2, inits, &dval, 10);
    printf("  n=7  t=2 mixed 3:4: decide %u (%u/%u)\n", (unsigned)dval, a, 7);
    check("Fig4 n=7 t=2 mixed 3:4", a == 7 && dval == 1);

    /* n=10 t=3, all start 0 */
    for (i = 0; i < MAX_N; ++i) inits[i] = 0;

    a = simFig4(10, 3, inits, &dval, 10);
    printf("  n=10 t=3 all-0    : decide %u (%u/%u)\n", (unsigned)dval, a, 10);
    check("Fig4 n=10 t=3 all-0", a == 10 && dval == 0);

    /* Even n+t configurations */
    for (i = 0; i < MAX_N; ++i) inits[i] = 0;

    a = simFig4(5, 1, inits, &dval, 10);
    printf("  n=5  t=1 all-0    : decide %u (%u/%u)\n", (unsigned)dval, a, 5);
    check("Fig4 n=5 t=1 all-0", a == 5 && dval == 0);

    a = simFig4(8, 2, inits, &dval, 10);
    printf("  n=8  t=2 all-0    : decide %u (%u/%u)\n", (unsigned)dval, a, 8);
    check("Fig4 n=8 t=2 all-0", a == 8 && dval == 0);

    for (i = 0; i < MAX_N; ++i) inits[i] = 1;

    a = simFig4(5, 1, inits, &dval, 10);
    printf("  n=5  t=1 all-1    : decide %u (%u/%u)\n", (unsigned)dval, a, 5);
    check("Fig4 n=5 t=1 all-1", a == 5 && dval == 1);

    a = simFig4(8, 2, inits, &dval, 10);
    printf("  n=8  t=2 all-1    : decide %u (%u/%u)\n", (unsigned)dval, a, 8);
    check("Fig4 n=8 t=2 all-1", a == 8 && dval == 1);
  }

  /*
   * Figure 2 — Abstract protocol round
   */
  testFig2();

  /*
   * Figure 3 — VALID sets
   */
  testFig3();
  testFig3Deep();
  testFig3Reeval();
  testFig3RecascadeOnGrowth();

  /*
   * Figure 4 — step-by-step, post-decide, edge cases, multi-phase
   */
  testFig4Steps();
  testFig4Step3Boundary();
  testFig4PostDecide();
  testFig4PostDecideAdversarial();
  testFig4EdgeCases();
  testFig4SubsetMajority();
  testFig4SubsetMajorityBoundary();
  testFig4DflagInjection();
  testFig4MaxPhasesClamp();
  testFig4MultiPhase();
  testFig4Byzantine();

  /*
   * Composed simulation — full Fig1+Fig3+Fig4 pipeline
   */
  testComposed();
  testComposedShuffled();
  testByzantineComposed();
  testPostDecideMultiPhase();
  testFig1ValueSwitch();
  testFig1Bpr();

  printf("\n===================\n");
  if (Fail)
    printf("%d FAILURES\n", Fail);
  else
    printf("ALL PASSED\n");

  return (Fail ? 1 : 0);
}
