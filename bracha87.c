/*
 * asynchronousByzantineAgreementProtocols - Asynchronous Byzantine Agreement Protocols
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
 * INFORMATION AND COMPUTATION 75, 130-143 (1987)
 * Gabriel Bracha - Asynchronous Byzantine Agreement Protocols
 *
 * Pure state machines for Figures 1, 2, 3, and 4.
 */

#include <assert.h>
#include <string.h>
#include "bracha87.h"

/* Bitmap helpers */
#define BIT_SZ(n)  (((unsigned int)(n) + 7) >> 3)
#define BIT_TST(a, i) ((a)[(unsigned int)(i) >> 3] & (1 << ((unsigned int)(i) & 7)))
#define BIT_SET(a, i) ((a)[(unsigned int)(i) >> 3] |= (unsigned char)(1 << ((unsigned int)(i) & 7)))

/*************************************************************************/
/*  Figure 1 — Reliable broadcast primitive                              */
/*************************************************************************/

/*
 * B_N: actual process count (n + 1, range 1..256).
 * Works for any struct with an unsigned char n field.
 *
 * F1_VLEN: actual value length (vLen + 1, range 1..256).
 *
 * Layout (N = B_N, L = F1_VLEN, BS = BIT_SZ(N)):
 *   value[L]             committed value
 *   ecFrom[BS]           echo bitmap
 *   ecVal[N * L]         echo value per peer
 *   rdFrom[BS]           ready bitmap
 *   rdVal[N * L]         ready value per peer
 */

#define B_N(b)       ((unsigned int)(b)->n + 1)
#define F1_VLEN(b)   ((unsigned int)(b)->vLen + 1)
#define F1_VALUE(b)  ((b)->data)
#define F1_ECFROM(b) ((b)->data + F1_VLEN(b))
#define F1_ECVAL(b)  ((b)->data + F1_VLEN(b) + BIT_SZ(B_N(b)))
#define F1_RDFROM(b) ((b)->data + F1_VLEN(b) + BIT_SZ(B_N(b)) + (unsigned long)B_N(b) * F1_VLEN(b))
#define F1_RDVAL(b)  ((b)->data + F1_VLEN(b) + BIT_SZ(B_N(b)) + (unsigned long)B_N(b) * F1_VLEN(b) + BIT_SZ(B_N(b)))

unsigned long
bracha87Fig1Sz(
  unsigned int n
 ,unsigned int vLen
){
  unsigned int N;
  unsigned int L;

  /* actual counts: n/vLen encode 0..255 as 1..256 */
  N = n + 1;
  L = vLen + 1;
  /*
   * Struct up to data[1] minus the 1, plus layout.
   * May over-allocate by the struct's trailing padding (at most
   * sizeof (unsigned short) - 1 here); harmless.
   */
  return (sizeof (struct bracha87Fig1) - 1
    + L                       /* value */
    + BIT_SZ(N)               /* ecFrom bitmap */
    + (unsigned long)N * L    /* ecVal */
    + BIT_SZ(N)               /* rdFrom bitmap */
    + (unsigned long)N * L);  /* rdVal */
}

void
bracha87Fig1Init(
  struct bracha87Fig1 *b
 ,unsigned char n
 ,unsigned char t
 ,unsigned char vLen
){
  /* n is encoded: actual process count = n + 1. Bracha requires
   * actual > 3t.  Use unsigned int to avoid wrap at n = 255. */
  assert((unsigned int)n + 1 > 3u * (unsigned int)t);
  memset(b, 0, bracha87Fig1Sz(n, vLen));
  b->n = n;
  b->t = t;
  b->vLen = vLen;
}

/*
 * Count echoes for value v (echo_count[v] from the paper).
 * O(1) for binary values (vLen==0, v in {0,1}) via incremental counters.
 * Falls back to O(N) scan for other values or multi-byte vLen.
 */
static unsigned int
fig1EcCnt(
  const struct bracha87Fig1 *b
 ,const unsigned char *v
){
  const unsigned char *ef;
  const unsigned char *ev;
  unsigned int N;
  unsigned int L;
  unsigned int cnt;
  unsigned int j;

  /* O(1) fast path for binary consensus */
  if (!b->vLen && v[0] <= 1)
    return (b->ecCnt[v[0]]);

  N = B_N(b);
  L = F1_VLEN(b);
  ef = F1_ECFROM(b);
  ev = F1_ECVAL(b);
  cnt = 0;
  if (L == 1) {
    /* Single-byte values: inline comparison, no memcmp overhead */
    for (j = 0; j < N; ++j)
      if (BIT_TST(ef, j) && ev[j] == v[0])
        ++cnt;
  } else {
    for (j = 0; j < N; ++j)
      if (BIT_TST(ef, j) && !memcmp(ev + (unsigned long)j * L, v, L))
        ++cnt;
  }
  return (cnt);
}

/*
 * Count readys for value v (ready_count[v] from the paper).
 * O(1) for binary values (vLen==0, v in {0,1}) via incremental counters.
 * Falls back to O(N) scan for other values or multi-byte vLen.
 */
static unsigned int
fig1RdCnt(
  const struct bracha87Fig1 *b
 ,const unsigned char *v
){
  const unsigned char *rf;
  const unsigned char *rv;
  unsigned int N;
  unsigned int L;
  unsigned int cnt;
  unsigned int j;

  /* O(1) fast path for binary consensus */
  if (!b->vLen && v[0] <= 1)
    return (b->rdCnt[v[0]]);

  N = B_N(b);
  L = F1_VLEN(b);
  rf = F1_RDFROM(b);
  rv = F1_RDVAL(b);
  cnt = 0;
  if (L == 1) {
    /* Single-byte values: inline comparison, no memcmp overhead */
    for (j = 0; j < N; ++j)
      if (BIT_TST(rf, j) && rv[j] == v[0])
        ++cnt;
  } else {
    for (j = 0; j < N; ++j)
      if (BIT_TST(rf, j) && !memcmp(rv + (unsigned long)j * L, v, L))
        ++cnt;
  }
  return (cnt);
}

/*
 * Record that peer 'from' sent echo with value v.
 */
static void
fig1SetEc(
  struct bracha87Fig1 *b
 ,unsigned char from
 ,const unsigned char *v
){
  BIT_SET(F1_ECFROM(b), from);
  memcpy(F1_ECVAL(b) + (unsigned long)from * F1_VLEN(b), v, F1_VLEN(b));
  if (!b->vLen && v[0] <= 1)
    ++b->ecCnt[v[0]];
}

/*
 * Record that peer 'from' sent ready with value v.
 */
static void
fig1SetRd(
  struct bracha87Fig1 *b
 ,unsigned char from
 ,const unsigned char *v
){
  BIT_SET(F1_RDFROM(b), from);
  memcpy(F1_RDVAL(b) + (unsigned long)from * F1_VLEN(b), v, F1_VLEN(b));
  if (!b->vLen && v[0] <= 1)
    ++b->rdCnt[v[0]];
}

/*
 * Commit to value v (set echoed, store committed value).
 */
static void
fig1Commit(
  struct bracha87Fig1 *b
 ,const unsigned char *v
){
  memcpy(F1_VALUE(b), v, F1_VLEN(b));
  b->flags |= BRACHA87_F1_ECHOED;
}


void
bracha87Fig1Origin(
  struct bracha87Fig1 *b
 ,const unsigned char *value
){
  if (!b || !value)
    return;
  memcpy(F1_VALUE(b), value, F1_VLEN(b));
  b->flags |= BRACHA87_F1_ORIGIN;
}

unsigned int
bracha87Fig1Input(
  struct bracha87Fig1 *b
 ,unsigned char type
 ,unsigned char from
 ,const unsigned char *value
 ,unsigned char *out
){
  unsigned int nout;
  unsigned int ec;
  unsigned int rd;
  unsigned char haveEchoed;
  unsigned char haveSentReady;
  unsigned char ecGtHalfNT;
  unsigned char rdGeTPlus1;
  unsigned char rdGe2TPlus1;
  unsigned char sendEcho;
  unsigned char sendReady;
  unsigned char acceptV;
  unsigned char replayEcho;
  unsigned char replayReady;

  if (!b || !value || !out || from > b->n || (b->flags & BRACHA87_F1_ACCEPTED))
    return (0);

  /* Per-sender deduplication and count update; ec/rd are 0 on the
   * branches where the corresponding count cannot fire a rule. */
  ec = 0;
  rd = 0;
  switch (type) {
  case BRACHA87_INITIAL:
    break;
  case BRACHA87_ECHO:
    if (BIT_TST(F1_ECFROM(b), from))
      return (0);
    fig1SetEc(b, from, value);
    ec = fig1EcCnt(b, value);
    break;
  case BRACHA87_READY:
    if (BIT_TST(F1_RDFROM(b), from))
      return (0);
    fig1SetRd(b, from, value);
    rd = fig1RdCnt(b, value);
    break;
  default:
    return (0);
  }

  haveEchoed    = (b->flags & BRACHA87_F1_ECHOED) ? 1 : 0;
  haveSentReady = (b->flags & BRACHA87_F1_RDSENT) ? 1 : 0;
  ecGtHalfNT    = ec >= (B_N(b) + b->t) / 2 + 1;
  rdGeTPlus1    = rd >= (unsigned int)b->t + 1;
  rdGe2TPlus1   = rd >= 2u * b->t + 1;
  sendEcho    = 0;
  sendReady   = 0;
  acceptV     = 0;
  replayEcho  = 0;
  replayReady = 0;

#include "bracha87Fig1.c"

  /* Entry-point discriminator: BPR replay outputs are
   * exhaustiveness-only on the Input path and discarded. */
  (void)replayEcho;
  (void)replayReady;

  nout = 0;
  if (sendEcho) {
    fig1Commit(b, value);
    out[nout++] = BRACHA87_ECHO_ALL;
  }
  if (sendReady) {
    memcpy(F1_VALUE(b), value, F1_VLEN(b));
    b->flags |= BRACHA87_F1_RDSENT;
    out[nout++] = BRACHA87_READY_ALL;
  }
  if (acceptV) {
    memcpy(F1_VALUE(b), value, F1_VLEN(b));
    b->flags |= BRACHA87_F1_ACCEPTED;
    out[nout++] = BRACHA87_ACCEPT;
  }
  return (nout);
}

const unsigned char *
bracha87Fig1Value(
  const struct bracha87Fig1 *b
){
  if (!b || !(b->flags & (BRACHA87_F1_ECHOED | BRACHA87_F1_ORIGIN)))
    return (0);
  return (F1_VALUE(b));
}

unsigned int
bracha87Fig1Bpr(
  struct bracha87Fig1 *b
 ,unsigned char *out
){
  unsigned int nout;
  unsigned char type;
  unsigned char haveEchoed;
  unsigned char haveSentReady;
  unsigned char ecGtHalfNT;
  unsigned char rdGeTPlus1;
  unsigned char rdGe2TPlus1;
  unsigned char sendEcho;
  unsigned char sendReady;
  unsigned char acceptV;
  unsigned char replayEcho;
  unsigned char replayReady;

  if (!b || !out)
    return (0);

  /* Nothing committed yet and not the originator -> nothing to replay. */
  if (!(b->flags & (BRACHA87_F1_ECHOED | BRACHA87_F1_ORIGIN)))
    return (0);

  nout = 0;

  /*
   * Origin-side INITIAL replay: an originator replays
   * (initial, v) on every Bpr call, whether or not it has
   * locally echoed.  Stopping at ECHOED would assume the cascade
   * is self-sustaining via Rule 2 once enough peers have
   * echoed -- but Rule 2's echo threshold is (n+t)/2+1 and at
   * the boundary regime n = 3t+1 with t honest peers
   * occluded by drop or byzantine silence, the threshold is
   * exactly the count of honest peers.  Any honest peer that
   * missed the bootstrap INITIAL can leave the cascade one
   * echo short forever; only the originator can break the
   * deadlock by re-sending INITIAL.
   *
   * The "always replay INITIAL when ORIGIN" rule is symmetric
   * with the "always replay READY when RDSENT" rule in the
   * dispatch (gap 3): both reject local-state-as-saturation
   * arguments because the load-bearing case is helping
   * OTHER peers, not the local one.  Application's silence-
   * quorum exit retires the instance.
   *
   * Single-bit guard, captured in C rather than as a DTC
   * sub-table (see decisionTableCompiler/README.md
   * "Cross-Domain Bridge Pattern" -- a single boundary-input
   * gate with no ordering insight stays in C).
   */
  if (b->flags & BRACHA87_F1_ORIGIN)
    out[nout++] = BRACHA87_INITIAL_ALL;

  /*
   * Echo / ready replay: chain on the merged paper+BPR
   * dispatch.  Bpr enters with a "kind of message" that cannot
   * fire any paper rule given current committed state -- type =
   * INITIAL with have echoed = yes leaves Rules 1-3 inhibited
   * (need !echoed) and Rules 4-6 unmet (need kind = (echo, v)
   * or (ready, v)).  Bracha outputs come back all "no" and the
   * chained BPR rules fire from "send X = no AND have-X-state =
   * yes".  Threshold predicates are immaterial to the chained
   * BPR rules in this state; 0 suffices.
   *
   * In the origin-but-not-echoed branch (above), the INITIAL
   * replay is already emitted; the dispatch is skipped because
   * ECHOED clear means the BPR echo / ready rules would emit
   * no-replay regardless of their other inputs (their guards
   * require haveEchoed = yes / haveSentReady = yes).
   */
  if (b->flags & BRACHA87_F1_ECHOED) {
    type          = BRACHA87_INITIAL;
    haveEchoed    = 1;
    haveSentReady = (b->flags & BRACHA87_F1_RDSENT) ? 1 : 0;
    ecGtHalfNT    = 0;
    rdGeTPlus1    = 0;
    rdGe2TPlus1   = 0;
    sendEcho    = 0;
    sendReady   = 0;
    acceptV     = 0;
    replayEcho  = 0;
    replayReady = 0;

#include "bracha87Fig1.c"

    /* Bracha outputs are guaranteed 0 by the type/state passed;
     * Bpr applies BPR replay outputs only. */
    (void)sendEcho;
    (void)sendReady;
    (void)acceptV;

    if (replayEcho)
      out[nout++] = BRACHA87_ECHO_ALL;
    if (replayReady)
      out[nout++] = BRACHA87_READY_ALL;
  }
  return (nout);
}

/*************************************************************************/
/*  Figure 2 — Abstract protocol round (0-based)                         */
/*************************************************************************/

/*
 * Layout:
 *   complete[(maxRounds+7)/8]  round-complete bitmap
 *   Per round (maxRounds rounds):
 *     recvCount                (unsigned char)
 *     received[(N+7)/8]        bitmap per peer
 *     values[N]                value per peer
 */

#define F2_CBMP(b)    ((b)->data)
#define F2_RDATA(b)   ((b)->data + BIT_SZ((unsigned int)(b)->maxRounds))
#define F2_RSZ(n)     (1 + BIT_SZ((unsigned int)(n) + 1) + (unsigned int)(n) + 1)
#define F2_REC(b, k)  (F2_RDATA(b) + (unsigned long)(k) * F2_RSZ((b)->n))
#define F2_RCNT(b, k) (*(F2_REC((b), (k))))
#define F2_RECV(b, k) (F2_REC((b), (k)) + 1)
#define F2_VALS(b, k) (F2_REC((b), (k)) + 1 + BIT_SZ(B_N(b)))

unsigned long
bracha87Fig2Sz(
  unsigned int n
 ,unsigned int maxRounds
){
  unsigned int N;

  N = n + 1;
  return (sizeof (struct bracha87Fig2) - 1
    + BIT_SZ(maxRounds)
    + (unsigned long)maxRounds * (1 + BIT_SZ(N) + N));
}

void
bracha87Fig2Init(
  struct bracha87Fig2 *b
 ,unsigned char n
 ,unsigned char t
 ,unsigned char maxRounds
){
  assert((unsigned int)n + 1 > 3u * (unsigned int)t);
  memset(b, 0, bracha87Fig2Sz(n, maxRounds));
  b->n = n;
  b->t = t;
  b->maxRounds = maxRounds;
}

unsigned int
bracha87Fig2Receive(
  struct bracha87Fig2 *b
 ,unsigned char k
 ,unsigned char sender
 ,unsigned char value
){
  if (!b || k >= b->maxRounds || sender > b->n)
    return (0);

  /* Deduplicate: one message per sender per round */
  if (BIT_TST(F2_RECV(b, k), sender))
    return (0);

  BIT_SET(F2_RECV(b, k), sender);
  F2_VALS(b, k)[sender] = value;
  ++F2_RCNT(b, k);

  /* Check n-t threshold */
  if (!BIT_TST(F2_CBMP(b), k)
   && F2_RCNT(b, k) >= B_N(b) - b->t) {
    BIT_SET(F2_CBMP(b), k);
    return (BRACHA87_ROUND_COMPLETE);
  }

  return (0);
}

unsigned int
bracha87Fig2RecvCount(
  const struct bracha87Fig2 *b
 ,unsigned char k
){
  if (!b || k >= b->maxRounds)
    return (0);
  return (F2_RCNT(b, k));
}

unsigned int
bracha87Fig2GetReceived(
  const struct bracha87Fig2 *b
 ,unsigned char k
 ,unsigned char *senders
 ,unsigned char *values
){
  const unsigned char *recv;
  const unsigned char *vals;
  unsigned int cnt;
  unsigned int i;

  if (!b || k >= b->maxRounds)
    return (0);

  recv = F2_RECV(b, k);
  vals = F2_VALS(b, k);
  cnt = 0;
  for (i = 0; i < B_N(b); ++i) {
    if (BIT_TST(recv, i)) {
      if (senders)
        senders[cnt] = (unsigned char)i;
      if (values)
        values[cnt] = vals[i];
      ++cnt;
    }
  }
  return (cnt);
}

/*************************************************************************/
/*  Figure 3 — Correctness enforcement (VALID sets, 0-based)             */
/*************************************************************************/

/*
 * Layout:
 *   complete[(maxRounds+7)/8]  round-complete bitmap
 *   Per round (maxRounds rounds):
 *     validCount               (unsigned char)
 *     arrived[(N+7)/8]         bitmap per peer
 *     valid[(N+7)/8]           bitmap per peer
 *     values[N]                value per peer
 */

#define F3_CBMP(b)    ((b)->data)
#define F3_RDATA(b)   ((b)->data + BIT_SZ((unsigned int)(b)->maxRounds))
#define F3_RSZ(n)     (1 + 2 * BIT_SZ((unsigned int)(n) + 1) + (unsigned int)(n) + 1)
#define F3_REC(b, k)  (F3_RDATA(b) + (unsigned long)(k) * F3_RSZ((b)->n))
#define F3_VCNT(b, k) (*(F3_REC((b), (k))))
#define F3_ARVD(b, k) (F3_REC((b), (k)) + 1)
#define F3_VALD(b, k) (F3_REC((b), (k)) + 1 + BIT_SZ(B_N(b)))
#define F3_VALS(b, k) (F3_REC((b), (k)) + 1 + 2 * BIT_SZ(B_N(b)))

unsigned long
bracha87Fig3Sz(
  unsigned int n
 ,unsigned int maxRounds
){
  unsigned int N;

  N = n + 1;
  return (sizeof (struct bracha87Fig3) - 1
    + BIT_SZ(maxRounds)
    + (unsigned long)maxRounds * (1 + 2 * BIT_SZ(N) + N));
}

void
bracha87Fig3Init(
  struct bracha87Fig3 *b
 ,unsigned char n
 ,unsigned char t
 ,unsigned char maxRounds
 ,bracha87Nfn N
 ,void *Nclosure
){
  assert((unsigned int)n + 1 > 3u * (unsigned int)t);
  memset(b, 0, bracha87Fig3Sz(n, maxRounds));
  b->n = n;
  b->t = t;
  b->maxRounds = maxRounds;
  b->N = N;
  b->Nclosure = Nclosure;
}

/*
 * Check VALID^k predicate for a message (sender, k, value).
 *
 * VALID^0: accepted && value in {0, 1}
 * VALID^k (k>0): accepted && exists n-t in VALID^{k-1}
 *                s.t. value = N(k-1, set)
 */
static int
fig3IsValid(
  const struct bracha87Fig3 *b
 ,unsigned char k
 ,unsigned char value
){
  unsigned int nt;
  unsigned int vc;
  unsigned char senders[256];
  unsigned char values[256];
  unsigned int i;
  unsigned int j;
  const unsigned char *vbm;
  const unsigned char *vls;
  unsigned char result;

  if (k >= b->maxRounds)
    return (0);

  /* VALID^0: value in {0, 1} */
  if (k == 0)
    return (value <= 1);

  /* VALID^k, k > 0: need n-t validated messages from round k-1 */
  nt = B_N(b) - b->t;
  vc = F3_VCNT(b, k - 1);
  if (vc < nt)
    return (0);

  /*
   * Collect ALL validated messages from round k-1.
   * N receives the full set so it can determine whether different
   * n-t subsets could produce different results (existential check).
   */
  vbm = F3_VALD(b, k - 1);
  vls = F3_VALS(b, k - 1);
  j = 0;
  for (i = 0; i < B_N(b); ++i) {
    if (BIT_TST(vbm, i)) {
      senders[j] = (unsigned char)i;
      values[j] = vls[i];
      ++j;
    }
  }

  /* Check: value = N(k-1, set) */
  {
    int rc;

    rc = b->N(b->Nclosure, k - 1, j, senders, values, &result);
    if (rc < 0)
      return (0);
    if (rc > 0) {
      /*
       * Permissive: base value must be 0 or 1.  *result carries the
       * D_FLAG-permission bit and (when D_FLAG is set) the legitimate
       * base value.  Reject incoming D_FLAG when N did not mark D_FLAG
       * as legitimate, and reject D_FLAG with a base that differs from
       * *result's base — at most one of (0|D_FLAG) or (1|D_FLAG) can
       * be legitimate per evaluation (see fig4Nfn case 1).
       */
      if ((value & (unsigned char)~BRACHA87_D_FLAG) > 1)
        return (0);
      if (value & BRACHA87_D_FLAG) {
        if (!(result & BRACHA87_D_FLAG))
          return (0);
        if ((value & 1) != (result & 1))
          return (0);
      }
      return (1);
    }
    return (result == value);
  }
}

unsigned int
bracha87Fig3Accept(
  struct bracha87Fig3 *b
 ,unsigned char k
 ,unsigned char sender
 ,unsigned char value
 ,unsigned int *validCount
){
  unsigned char *rec;
  unsigned int bs;
  unsigned char *arvd;
  unsigned char *vald;
  unsigned char *vals;
  unsigned char haveStored;
  unsigned char validKHolds;
  unsigned char roundKComplete;
  unsigned char postMarkAtNT;
  unsigned char doStore;
  unsigned char doMarkValid;
  unsigned char doSignalComplete;
  unsigned char doCascade;

  if (!b || k >= b->maxRounds || sender > b->n)
    return (0);

  /* Cache record pointer — avoids repeated F3_REC multiplication */
  bs = BIT_SZ(B_N(b));
  rec = F3_REC(b, k);
  arvd = rec + 1;
  vald = rec + 1 + bs;
  vals = rec + 1 + 2 * bs;

  /* Boundary inputs.  fig3IsValid is short-circuited on duplicate:
   * the dispatch zeros every output when haveStored is 1 anyway, and
   * the predicate is the one expensive call here. */
  haveStored = BIT_TST(arvd, sender) ? 1 : 0;
  validKHolds = !haveStored && fig3IsValid(b, k, value);
  roundKComplete = BIT_TST(F3_CBMP(b), k) ? 1 : 0;
  postMarkAtNT = (*rec + validKHolds) >= B_N(b) - b->t;

  doStore = 0;
  doMarkValid = 0;
  doSignalComplete = 0;
  doCascade = 0;

#include "bracha87Fig3.c"

  /* Apply outputs in order: store, mark valid, signal complete, cascade. */
  if (doStore) {
    BIT_SET(arvd, sender);
    vals[sender] = value;
  }
  if (doMarkValid) {
    BIT_SET(vald, sender);
    ++*rec;
  }
  if (doSignalComplete) {
    BIT_SET(F3_CBMP(b), k);
  }
  if (doCascade) {
    /*
     * Fig 2 round coordination: when VALID^k has reached n-t,
     * re-evaluate stored messages at r = k+1, k+2, ...  VALID^{r}
     * is existential over n-t subsets of VALID^{r-1} and is
     * monotone in VALID^{r-1} (paper VALID definition + Lemma 6),
     * so growth at r-1 past n-t can unlock previously-stored but
     * not-yet-valid round-r messages.  Therefore re-evaluation
     * must fire on any growth at r-1 that reaches n-t, not only
     * on the first crossing.  Termination: each pass either adds
     * at least one valid (changed) or stops.
     */
    unsigned int r;

    for (r = (unsigned int)k + 1; r < b->maxRounds; ++r) {
      unsigned char csnd[256];
      unsigned char cval[256];
      unsigned int cj;
      unsigned char cres;
      int crc;
      unsigned char *pvbm;
      unsigned char *pvls;
      unsigned char *ra;
      unsigned char *rv;
      unsigned char *rvl;
      unsigned int i;
      int changed;

      /* VALID^r requires n-t in VALID^{r-1}. If not yet, cascade stalls. */
      if (F3_VCNT(b, r - 1) < B_N(b) - b->t)
        break;

      /*
       * VALID^r check (paper Fig 3), hoisted out of the per-peer
       * loop: collect the validated set from round r-1 once and
       * call N once, then test each peer's value against the result.
       */
      pvbm = F3_VALD(b, r - 1);
      pvls = F3_VALS(b, r - 1);
      cj = 0;
      for (i = 0; i < B_N(b); ++i) {
        if (BIT_TST(pvbm, i)) {
          csnd[cj] = (unsigned char)i;
          cval[cj] = pvls[i];
          ++cj;
        }
      }
      crc = b->N(b->Nclosure, r - 1, cj, csnd, cval, &cres);

      ra = F3_ARVD(b, r);
      rv = F3_VALD(b, r);
      rvl = F3_VALS(b, r);
      changed = 0;
      for (i = 0; i < B_N(b); ++i) {
        if (BIT_TST(ra, i) && !BIT_TST(rv, i)) {
          int valid;

          /* VALID^r: value = N(r-1, S) — same logic as fig3IsValid.
           * Permissive *result encodes the D_FLAG permission and, when
           * set, the legitimate base.  D_FLAG with a mismatched base is
           * rejected (fig4Nfn case 1 produces at most one of 0|D_FLAG
           * or 1|D_FLAG per evaluation). */
          if (crc < 0)
            valid = 0;
          else if (crc > 0) {
            valid = ((rvl[i] & (unsigned char)~BRACHA87_D_FLAG) <= 1);
            if (valid && (rvl[i] & BRACHA87_D_FLAG)) {
              if (!(cres & BRACHA87_D_FLAG))
                valid = 0;
              else if ((rvl[i] & 1) != (cres & 1))
                valid = 0;
            }
          } else
            valid = (rvl[i] == cres);
          if (valid) {
            BIT_SET(rv, i);
            ++F3_VCNT(b, r);
            changed = 1;
          }
        }
      }
      if (F3_VCNT(b, r) >= B_N(b) - b->t && !BIT_TST(F3_CBMP(b), r))
        BIT_SET(F3_CBMP(b), r);
      if (!changed)
        break; /* round r didn't grow — no downstream unlock possible */
    }
  }

  if (validCount)
    *validCount = *rec;
  return (doMarkValid ? BRACHA87_VALIDATED : 0);
}

unsigned int
bracha87Fig3ValidCount(
  const struct bracha87Fig3 *b
 ,unsigned char k
){
  if (!b || k >= b->maxRounds)
    return (0);
  return (F3_VCNT(b, k));
}

unsigned int
bracha87Fig3GetValid(
  const struct bracha87Fig3 *b
 ,unsigned char k
 ,unsigned char *senders
 ,unsigned char *values
){
  const unsigned char *vbm;
  const unsigned char *vls;
  unsigned int cnt;
  unsigned int i;

  if (!b || k >= b->maxRounds)
    return (0);

  vbm = F3_VALD(b, k);
  vls = F3_VALS(b, k);
  cnt = 0;
  for (i = 0; i < B_N(b); ++i) {
    if (BIT_TST(vbm, i)) {
      if (senders)
        senders[cnt] = (unsigned char)i;
      if (values)
        values[cnt] = vls[i];
      ++cnt;
    }
  }
  return (cnt);
}

int
bracha87Fig3RoundComplete(
  const struct bracha87Fig3 *b
 ,unsigned char k
){
  if (!b || k >= b->maxRounds)
    return (0);
  return (BIT_TST(F3_CBMP(b), k) != 0);
}

/*************************************************************************/
/*  Figure 4 — Consensus protocol (0-based)                              */
/*************************************************************************/

/*
 * Figure 4 embeds a Figure 3 instance.
 * The N function implements the three round-specific functions:
 *   Round 3i   (sub 0): majority
 *   Round 3i+1 (sub 1): decision check
 *   Round 3i+2 (sub 2): (handled directly in fig4, N not needed)
 */

/*
 * The N function for Figure 4.
 * Implements the protocol function parameterizing Figure 3.
 *
 * Receives all validated messages for the round (may exceed n-t).
 * When n_msgs > n-t, checks whether different n-t subsets could
 * produce different results. If so, returns permissive (>0) to
 * implement the paper's existential quantifier in VALID^k.
 *
 * Round 3i   (sub 0): v = majority of the n-t values (tie-breaks to 0,
 *                     matching Fig4Round's state transition)
 * Round 3i+1 (sub 1): v = (d, v') if >n/2 agree, else v unchanged
 * Round 3i+2 (sub 2): decide/adopt/coin (deterministic only when
 *                     >2t d-messages hold across every n-t subset)
 *
 * Permissive *result encoding: the D_FLAG bit conveys whether a
 * D-flagged incoming value can be legitimately produced by some
 * n-t subset.  When not set, fig3IsValid rejects D_FLAG on the
 * incoming message (defense against Byzantine d-injection).
 */
static int
fig4Nfn(
  void *closure
 ,unsigned char k
 ,unsigned int n_msgs
 ,const unsigned char *senders
 ,const unsigned char *values
 ,unsigned char *result
){
  struct bracha87Fig4 *f4 = (struct bracha87Fig4 *)closure;
  unsigned int nt;
  unsigned int cnt[2];
  unsigned int dc[2];
  unsigned int i;
  unsigned int sub;
  unsigned char v;

  (void)senders;

  if (!n_msgs)
    return (-1);

  nt = B_N(f4) - f4->t;
  sub = (unsigned int)(k % 3);
  cnt[0] = cnt[1] = 0;
  dc[0] = dc[1] = 0;

  for (i = 0; i < n_msgs; ++i) {
    v = values[i] & (unsigned char)~BRACHA87_D_FLAG;
    if (v <= 1) {
      ++cnt[v];
      if (values[i] & BRACHA87_D_FLAG)
        ++dc[v];
    }
  }

  switch (sub) {

  case 0:
    /*
     * Round 3i+1 broadcast: value = majority of step-1 (round 3i) set.
     * N tie-breaks to 0 (matches Fig4Round case 0 state transition).
     *
     * Reachability of result 1 in some n-t subset: max s[1] > nt/2,
     *   i.e. cnt[1] >= nt/2 + 1 (strict majority).
     * Reachability of result 0 in some n-t subset: min s[1] <= nt/2
     *   (even nt; tie breaks to 0) or min s[1] <= (nt-1)/2 (odd nt),
     *   i.e. cnt[0] >= (nt + 1) / 2.  The formula works uniformly.
     *
     * Output is a pure binary; no D_FLAG legitimate here.
     */
    *result = (cnt[1] > cnt[0]) ? 1 : 0;
    if (n_msgs > nt
     && cnt[0] >= (nt + 1) / 2
     && cnt[1] >= nt / 2 + 1)
      return (1); /* permissive: both 0 and 1 reachable; no D_FLAG */
    break;

  case 1:
    /*
     * Round 3i+2 broadcast: step-2 output.  v|D_FLAG if >n/2 of the
     * round-3i+1 (step-1 majority) inputs agree on v, else v unchanged
     * (unchanged path is non-deterministic across peers).
     *
     * With n_msgs > nt, a subset might reach >n/2 even when the full
     * set does not, or vice versa.  Worst-case n-t subset for v
     * removes up to excess = n_msgs - nt copies of v.
     */
    if (cnt[0] * 2 > B_N(f4)) {
      *result = 0 | BRACHA87_D_FLAG;
      /*
       * Invariant: n_msgs <= B_N, so excess = n_msgs - nt <= t.
       * In Bracha's regime t < N/3, and cnt[0] > N/2 > t >= excess,
       * so the unsigned subtraction below cannot wrap.
       */
      assert(cnt[0] >= n_msgs - nt);
      if (n_msgs > nt
       && (cnt[0] - (n_msgs - nt)) * 2 <= B_N(f4))
        return (1); /* permissive, D_FLAG legitimate */
      return (0);
    }
    if (cnt[1] * 2 > B_N(f4)) {
      *result = 1 | BRACHA87_D_FLAG;
      assert(cnt[1] >= n_msgs - nt);
      if (n_msgs > nt
       && (cnt[1] - (n_msgs - nt)) * 2 <= B_N(f4))
        return (1); /* permissive, D_FLAG legitimate */
      return (0);
    }
    /*
     * No strict majority in full set.  In Bracha's t < n/3 regime
     * nt > N/2, so max s[v] = min(cnt[v], nt).  Given cnt[v]*2 <= N
     * (and cnt[v] <= nt by n_msgs <= N), no subset has strict
     * majority either — D_FLAG is not legitimate for any honest peer.
     * Value is the peer's prior value (non-deterministic), so
     * permissive on the base value only.
     */
    *result = 0;
    return (1); /* permissive, no D_FLAG */

  case 2:
    /*
     * Round 3(i+1) broadcast: step-3 output, which is always a plain
     * binary value (decided value from case (i), adopted value from
     * case (ii), or coin from case (iii) — none carry D_FLAG).
     *
     * Input S is the round 3i+2 messages (step-2 outputs, which may
     * carry D_FLAG).  Exact result only when case (i) holds across
     * every n-t subset: dc[dm] - excess > 2t.  Otherwise the output
     * depends on the peer's own (d,v) set and/or a coin, so permissive.
     *
     * Invariant: excess <= t and dc[dm] > 2t together imply
     * dc[dm] > excess, so the unsigned subtraction below does not
     * wrap.  Removing the outer guard would break that invariant.
     */
    {
      unsigned char dm;
      unsigned int excess;

      dm = (dc[1] > dc[0]) ? 1 : 0;
      excess = (n_msgs > nt) ? n_msgs - nt : 0;
      if (dc[dm] > 2u * f4->t) {
        /*
         * dc[dm] > 2t and excess <= t together guarantee
         * dc[dm] > excess, so the subtraction cannot wrap.
         */
        assert(dc[dm] >= excess);
        if (dc[dm] - excess > 2u * f4->t) {
          *result = dm;
          return (0);
        }
      }
      *result = 0; /* permissive, no D_FLAG (output is step-3) */
      return (1);
    }
  }

  /* case 0 exits here via break: exact with *result set above, no D_FLAG */
  return (0);
}

unsigned long
bracha87Fig4Sz(
  unsigned int n
 ,unsigned int maxPhases
){
  /* Clamp: maxPhases * 3 must fit in unsigned char round count. */
  if (maxPhases > BRACHA87_MAX_PHASES)
    maxPhases = BRACHA87_MAX_PHASES;
  /*
   * sizeof (struct bracha87Fig4) counts the embedded fig3 as exactly
   * sizeof (struct bracha87Fig3) (its declared size, including the
   * 1-byte data[1]).  The actual Fig3 instance needs Fig3Sz bytes;
   * subtract the placeholder, add the real size.
   */
  return (sizeof (struct bracha87Fig4) - sizeof (struct bracha87Fig3)
    + bracha87Fig3Sz(n, maxPhases * 3));
}

void
bracha87Fig4Init(
  struct bracha87Fig4 *b
 ,unsigned char n
 ,unsigned char t
 ,unsigned char maxPhases
 ,unsigned char initialValue
 ,bracha87CoinFn coin
 ,void *coinClosure
){
  assert((unsigned int)n + 1 > 3u * (unsigned int)t);
  /* Clamp: maxPhases * 3 must fit in unsigned char round count. */
  if (maxPhases > BRACHA87_MAX_PHASES)
    maxPhases = BRACHA87_MAX_PHASES;
  memset(b, 0, bracha87Fig4Sz(n, maxPhases));
  b->n = n;
  b->t = t;
  b->maxPhases = maxPhases;
  b->phase = 0;
  b->subRound = 0;
  b->value = initialValue;
  b->decided = 0;
  b->decision = 0;
  b->coin = coin;
  b->coinClosure = coinClosure;
  bracha87Fig3Init(
    &b->fig3
   ,n, t, (unsigned char)(maxPhases * 3), fig4Nfn, b);
}

unsigned int
bracha87Fig4Round(
  struct bracha87Fig4 *b
 ,unsigned char k
 ,unsigned int n_msgs
 ,const unsigned char *senders
 ,const unsigned char *values
){
  unsigned int cnt[2];
  unsigned int dc[2];
  unsigned int sub;
  unsigned int ph;
  unsigned int i;
  unsigned char v;
  unsigned char dmax;
  unsigned char subRound;
  unsigned char haveDecided;
  unsigned char n2Half;
  unsigned char gt2T;
  unsigned char gtT;
  unsigned char setMajority;
  unsigned char setDMajority;
  unsigned char decideV;
  unsigned char adoptV;
  unsigned char setCoin;

  (void)senders;
  if (!b || !n_msgs)
    return (0);

  sub = (unsigned int)(k % 3);
  ph = (unsigned int)(k / 3);

  /* Count base values (strip d flag) and d-flagged values separately */
  cnt[0] = cnt[1] = 0;
  dc[0] = dc[1] = 0;
  for (i = 0; i < n_msgs; ++i) {
    v = values[i] & (unsigned char)~BRACHA87_D_FLAG;
    if (v <= 1) {
      ++cnt[v];
      if (values[i] & BRACHA87_D_FLAG)
        ++dc[v];
    }
  }
  dmax = (dc[1] > dc[0]) ? 1 : 0;

  /* Boundary inputs.  At-most-one of dc[0]/dc[1] can exceed t (and
   * therefore 2t), since their sum is bounded by n_msgs <= n-t and
   * 2t+2 > n-t when n > 3t -- so dmax disambiguates safely. */
  subRound    = (unsigned char)sub;
  haveDecided = b->decided ? 1 : 0;
  n2Half      = (cnt[0] * 2 > B_N(b)) || (cnt[1] * 2 > B_N(b));
  gt2T        = dc[dmax] > 2u * b->t;
  gtT         = dc[dmax] > (unsigned int)b->t;

  setMajority  = 0;
  setDMajority = 0;
  decideV      = 0;
  adoptV       = 0;
  setCoin      = 0;

#include "bracha87Fig4.c"

  /* Apply value updates from dispatch outputs.  At most one fires. */
  if (setMajority)
    b->value = (cnt[1] > cnt[0]) ? 1 : 0;
  if (setDMajority)
    b->value = (unsigned char)(((cnt[1] * 2 > B_N(b)) ? 1 : 0) | BRACHA87_D_FLAG);
  if (decideV) {
    b->value = dmax;
    b->decision = dmax;
    b->decided = 1;
  }
  if (adoptV)
    b->value = dmax;
  if (setCoin)
    b->value = b->coin(b->coinClosure, (unsigned char)ph);

  /* Procedural advance + return code.  Phase advance happens only at
   * end-of-phase (sub=2); the decide/exhausted/maxPhases return
   * codes can't be expressed declaratively so they stay in C. */
  switch (sub) {
  case 0:
    b->phase = (unsigned char)ph;
    b->subRound = 1;
    return (BRACHA87_BROADCAST);
  case 1:
    b->phase = (unsigned char)ph;
    b->subRound = 2;
    return (BRACHA87_BROADCAST);
  case 2:
    if (decideV) {
      if (ph + 1 >= b->maxPhases)
        return (BRACHA87_DECIDE);
      b->phase = (unsigned char)(ph + 1);
      b->subRound = 0;
      return (BRACHA87_DECIDE | BRACHA87_BROADCAST);
    }
    if (haveDecided) {
      /* Post-decide continuation: broadcast the decision. */
      b->value = b->decision;
      if (ph + 1 >= b->maxPhases)
        return (0);
      b->phase = (unsigned char)(ph + 1);
      b->subRound = 0;
      return (BRACHA87_BROADCAST);
    }
    if (ph + 1 >= b->maxPhases)
      return (BRACHA87_EXHAUSTED);
    b->phase = (unsigned char)(ph + 1);
    b->subRound = 0;
    return (BRACHA87_BROADCAST);
  }
  return (0);
}
