/*
 * asynchronousByzantineAgreementProtocols - BKR94 Asynchronous Common Subset
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
 * BKR94 Asynchronous Common Subset — Protocol Agreement[Q].
 *
 * This file is BKR94 Section 4 Figure 3.  Direct line-by-line port
 * of Ben-Or/Kelmer/Rabin 1994; see BKR94ACS.txt for the paper
 * extract used as the spec.  Composes N Bracha87 Fig1 instances
 * (proposal reliable broadcast supplying Q) with N Bracha87 Fig4
 * instances (binary consensus on inclusion — the BKR94 "BA"
 * subprotocol).
 *
 * Step 1 lives in bkr94acsProposalInput (vote 1 on Fig1 ACCEPT).
 * Step 2 lives in bkr94acsConsensusInput (vote 0 fanout when the
 *   2t+1-BAs-with-output-1 threshold hits inside the Fig4Round
 *   DECIDE branch).
 * Step 3 lives in bkr94acsConsensusInput (BKR94ACS_ACT_COMPLETE
 *   when all N BAs have decided) and bkr94acsSubset
 *   (SubSet = { j : BA_j = 1 }).
 */

#include <assert.h>
#include <string.h>
#include "bkr94acs.h"

#define A_N(a) ((unsigned int)(a)->n + 1)

/*
 * Pointer alignment for carving Fig1/Fig4 instances out of a single
 * buffer.  bracha87Fig4 begins with function-pointer fields, and
 * bracha87Fig1 begins with 2-byte shorts; rounding up to pointer
 * alignment subsumes both.  struct bkr94acs.pad[7] pairs with this
 * to place a->data at a pointer-aligned offset from a.
 */
#define BKR94ACS_ALIGN_P  ((unsigned long)sizeof (void *))
#define BKR94ACS_ALIGN_UP(x)  (((unsigned long)(x) + BKR94ACS_ALIGN_P - 1) \
                               & ~(BKR94ACS_ALIGN_P - 1))

/*------------------------------------------------------------------------*/
/*  Internal layout helpers                                               */
/*                                                                        */
/*  data[] layout (N = n + 1, MR = maxPhases * 3, all sub-sizes rounded   */
/*  up to pointer alignment):                                             */
/*    voted[N]           per-origin vote status                           */
/*    baDecision[N]      per-origin BA decision                           */
/*    conNextRound[N]    per-origin next consensus round to check         */
/*    (alignment pad to pointer alignment)                                */
/*    propFig1 area      N * propF1Sz bytes                               */
/*    conPipeline area   N * (MR * N * conF1Sz + fig4Sz) bytes            */
/*------------------------------------------------------------------------*/

/* Per-origin vote status */
#define BKR94ACS_VOTE_NONE 0
#define BKR94ACS_VOTE_ONE  1
#define BKR94ACS_VOTE_ZERO 2

/* ACS-event discriminator for the bkr94acsRules.c dispatch.  See
 * bkr94acsToC.dtc; the values are opaque tokens compared by equality. */
#define BKR94ACS_ACS_EVENT_Q   0
#define BKR94ACS_ACS_EVENT_BA0 1
#define BKR94ACS_ACS_EVENT_BA1 2

/*
 * All layout helpers take const struct bkr94acs * — they only read
 * the header fields n/vLen/maxPhases to compute offsets.  Callers
 * that need to write through the returned pointer cast away const.
 * This lets the read-only query functions (bkr94acsSubset,
 * bkr94acsProposalValue) stay const-correct.
 */

static unsigned char *
bkr94acsVoted(
  const struct bkr94acs *a
){
  return ((unsigned char *)a->data);
}

static unsigned char *
bkr94acsDecision(
  const struct bkr94acs *a
){
  return ((unsigned char *)a->data + A_N(a));
}

static unsigned char *
bkr94acsConNextRound(
  const struct bkr94acs *a
){
  return ((unsigned char *)a->data + 2 * A_N(a));
}

/*
 * Size of the voted/baDecision/conNextRound header, padded up to
 * pointer alignment so that propF1Base is aligned.
 */
static unsigned long
headerSz(
  const struct bkr94acs *a
){
  return (BKR94ACS_ALIGN_UP(3UL * A_N(a)));
}

static unsigned long
propF1Sz(
  const struct bkr94acs *a
){
  return (BKR94ACS_ALIGN_UP(bracha87Fig1Sz(a->n, a->vLen)));
}

static unsigned long
conF1Sz(
  const struct bkr94acs *a
){
  return (BKR94ACS_ALIGN_UP(bracha87Fig1Sz(a->n, 0)));
}

static unsigned long
fig4Sz(
  const struct bkr94acs *a
){
  return (BKR94ACS_ALIGN_UP(bracha87Fig4Sz(a->n, a->maxPhases)));
}

static unsigned int
maxRounds(
  const struct bkr94acs *a
){
  return ((unsigned int)a->maxPhases * 3);
}

/* Base of proposal Fig1 area */
static unsigned char *
propF1Base(
  const struct bkr94acs *a
){
  return ((unsigned char *)a->data + headerSz(a));
}

/* Proposal Fig1 instance for origin j */
static struct bracha87Fig1 *
propF1(
  const struct bkr94acs *a
 ,unsigned int j
){
  return ((struct bracha87Fig1 *)(propF1Base(a) + j * propF1Sz(a)));
}

/* Base of consensus pipeline area */
static unsigned char *
conBase(
  const struct bkr94acs *a
){
  return (propF1Base(a) + A_N(a) * propF1Sz(a));
}

/* Size of one consensus pipeline (for origin j) */
static unsigned long
conPipelineSz(
  const struct bkr94acs *a
){
  return ((unsigned long)maxRounds(a) * A_N(a) * conF1Sz(a) + fig4Sz(a));
}

/* Consensus Fig1 instance for origin j, round r, broadcaster k */
static struct bracha87Fig1 *
conF1(
  const struct bkr94acs *a
 ,unsigned int j
 ,unsigned int r
 ,unsigned int k
){
  unsigned char *base;

  base = conBase(a) + j * conPipelineSz(a);
  return ((struct bracha87Fig1 *)(base
    + ((unsigned long)r * A_N(a) + k) * conF1Sz(a)));
}

/* Consensus Fig4 instance for origin j */
static struct bracha87Fig4 *
conF4(
  const struct bkr94acs *a
 ,unsigned int j
){
  unsigned char *base;

  base = conBase(a) + j * conPipelineSz(a);
  return ((struct bracha87Fig4 *)(base
    + (unsigned long)maxRounds(a) * A_N(a) * conF1Sz(a)));
}


/*------------------------------------------------------------------------*/
/*  Public API                                                            */
/*------------------------------------------------------------------------*/

unsigned long
bkr94acsSz(
  unsigned int n
 ,unsigned int vLen
 ,unsigned int maxPhases
){
  unsigned int N;
  unsigned long pf1;
  unsigned long cf1;
  unsigned long f4;
  unsigned long mr;
  unsigned long hdr;
  unsigned long conPipe;

  /*
   * Encoded limits: n and vLen carry actual_count - 1 in an unsigned
   * char, so they must fit 0..255.  maxPhases is bounded by the
   * unsigned-char round counter (3 * maxPhases <= 255).
   */
  if (n > 255 || vLen > 255 || maxPhases == 0
   || maxPhases > BRACHA87_MAX_PHASES)
    return (0);

  N = n + 1;
  pf1 = BKR94ACS_ALIGN_UP(bracha87Fig1Sz(n, vLen));
  cf1 = BKR94ACS_ALIGN_UP(bracha87Fig1Sz(n, 0));
  f4 = BKR94ACS_ALIGN_UP(bracha87Fig4Sz(n, maxPhases));
  mr = (unsigned long)maxPhases * 3;
  hdr = BKR94ACS_ALIGN_UP(3UL * N);
  conPipe = mr * N * cf1 + f4;

  return (sizeof (struct bkr94acs) - 1
    + hdr                      /* voted + baDecision + conNextRound + pad */
    + N * pf1                  /* proposal Fig1 instances */
    + N * conPipe);            /* consensus pipelines */
}

void
bkr94acsInit(
  struct bkr94acs *a
 ,unsigned char n
 ,unsigned char t
 ,unsigned char vLen
 ,unsigned char maxPhases
 ,unsigned char self
 ,bracha87CoinFn coin
 ,void *coinClosure
){
  unsigned int N;
  unsigned int i;
  unsigned int j;
  unsigned int r;

  /*
   * Validate caller contract.  bkr94acsSz rejects the same
   * out-of-range maxPhases by returning 0; if bkr94acsInit proceeded
   * past these checks it would memcpy into fields beyond the
   * allocation and the Fig4 step 3 path would crash on the missing
   * coin.  Bracha also requires actual_N > 3t (asserted in each
   * Fig*Init below, but checking here gives a cleaner failure mode).
   */
  if (!coin)
    return;
  if (!maxPhases || maxPhases > BRACHA87_MAX_PHASES)
    return;
  assert((unsigned int)n + 1 > 3u * (unsigned int)t);

  memset(a, 0, bkr94acsSz(n, vLen, maxPhases));
  a->n = n;
  a->t = t;
  a->vLen = vLen;
  a->maxPhases = maxPhases;
  a->self = self;

  N = (unsigned int)n + 1;

  /* Mark all BA decisions as undecided */
  memset(bkr94acsDecision(a), 0xFF, N);

  /* Initialize proposal Fig1 instances */
  for (i = 0; i < N; ++i)
    bracha87Fig1Init(propF1(a, i), n, t, vLen);

  /* Initialize consensus pipelines */
  {
    unsigned int mr2;

    mr2 = (unsigned int)maxPhases * 3;
    for (i = 0; i < N; ++i) {
      for (r = 0; r < mr2; ++r)
        for (j = 0; j < N; ++j)
          bracha87Fig1Init(conF1(a, i, r, j), n, t, 0);
      bracha87Fig4Init(conF4(a, i), n, t, maxPhases, 0,
                       coin, coinClosure);
    }
  }
}

/*
 * Internal: enter this peer's input value into BA_origin.
 *
 * Called from both BKR94 Step 1 (vote=1 on Fig1 ACCEPT) and BKR94
 * Step 2 (vote=0 when the n-t-BAs-output-1 threshold fires).  The
 * voted[] guard enforces the paper's single-input-per-BA rule:
 * "Once a BA has received an input from Pi (1 from step 1 or 0
 * from step 2), step 1 and step 2 stop touching it — BA semantics
 * demand a single input per player."  First caller wins.
 *
 * Returns number of BKR94ACS_ACT_CON_SEND actions added (0 if
 * already voted, 1 otherwise).
 */
static unsigned int
bkr94acsVote(
  struct bkr94acs *a
 ,unsigned int origin
 ,unsigned char vote
 ,struct bkr94acsAct *out
){
  unsigned char *voted;

  voted = bkr94acsVoted(a);
  if (voted[origin] != BKR94ACS_VOTE_NONE)
    return (0);

  voted[origin] = vote ? BKR94ACS_VOTE_ONE : BKR94ACS_VOTE_ZERO;

  /*
   * Initial broadcast of our BA input.  Mark the corresponding
   * (origin, round=0, broadcaster=self) consensus Fig1 as the
   * originator and store the value, so bkr94acsPump's BPR walk
   * replays BKR94ACS_ACT_CON_SEND with conType=INITIAL until
   * loopback or echo / ready cascade sets ECHOED on that Fig1.
   * Consensus Fig1s are vLen=0 (binary value), so we pass the
   * raw vote byte (0 or 1), not the BKR94ACS_VOTE_* encoding
   * stored in voted[].
   */
  {
    unsigned char binary;

    binary = vote ? 1 : 0;
    bracha87Fig1Origin(conF1(a, origin, 0, a->self), &binary);
  }

  out->value = 0;
  out->act = BKR94ACS_ACT_CON_SEND;
  out->origin = (unsigned char)origin;
  out->round = 0;
  out->type = BRACHA87_INITIAL;
  out->conValue = vote;
  out->broadcaster = a->self;
  return (1);
}

unsigned int
bkr94acsProposalInput(
  struct bkr94acs *a
 ,unsigned char origin
 ,unsigned char type
 ,unsigned char from
 ,const unsigned char *value
 ,struct bkr94acsAct *out
){
  struct bracha87Fig1 *f1;
  unsigned char f1out[3];
  unsigned int nf1;
  unsigned int nact;
  unsigned int k;
  unsigned char acsEvent;
  unsigned char inputToBAj;
  unsigned char fanoutTriggered;
  unsigned char postCountOneAtNT;
  unsigned char postCountAllN;
  unsigned char doInput1;
  unsigned char doInput0Fanout;
  unsigned char doOutputSubset;

  /*
   * Encoded: a->n = actual_N - 1, so valid peer indices are
   * 0..a->n inclusive.  "> a->n" rejects actual_N and above.
   *
   * Do NOT short-circuit on a->complete.  This peer has locally
   * decided all N BAs, but other peers may still be working on
   * some BAs and depend on THIS peer's continued Fig1 echoes and
   * readys to reach their own n-t thresholds.  Bracha requires
   * post-decide continuation at the BA level (pitfall #1); the
   * same obligation applies at the BKR94 ACS level — a
   * locally-complete peer must keep participating until the
   * application decides to exit (e.g. progress-silence quorum).
   * A blanket complete-guard causes classic post-decide stalls
   * where the fastest peer strands the slowest.  The per-action
   * emission blocks below (BA_DECIDED on Fig4 DECIDE, COMPLETE on
   * nDecided crossing N, votes via bkr94acsVote's voted-state
   * dedup) are idempotent, so continuing after complete cannot
   * emit duplicate terminal actions.
   */
  if (!a || origin > a->n || from > a->n || !value || !out)
    return (0);

  f1 = propF1(a, origin);
  nf1 = bracha87Fig1Input(f1, type, from, value, f1out);
  nact = 0;

  for (k = 0; k < nf1; ++k) {
    if (f1out[k] == BRACHA87_ECHO_ALL || f1out[k] == BRACHA87_READY_ALL) {
      out[nact].value = bracha87Fig1Value(f1);
      out[nact].act = BKR94ACS_ACT_PROP_SEND;
      out[nact].origin = origin;
      out[nact].round = 0;
      out[nact].type = (f1out[k] == BRACHA87_ECHO_ALL)
                       ? BRACHA87_ECHO : BRACHA87_READY;
      out[nact].conValue = 0;
      out[nact].broadcaster = 0;
      ++nact;
    } else if (f1out[k] == BRACHA87_ACCEPT) {
      /*
       * BKR94 Step 1: "For each Pj for whom you (Pi) know Q(j) = 1,
       * participate in BA_j with input 1."  Q(j) = 1 is carried by
       * Fig1 ACCEPT for origin j (Bracha87 Lemma 4 gives BKR94's Q
       * assumption (2) for free).  Step 2's n-t BA-output-1 trigger
       * fires in bkr94acsConsensusInput; only Step 1 is reachable at
       * this dispatch site, but the bridge sees the full rule set so
       * the unreachable outputs are zeroed by the dispatch and the
       * post-include applies only doInput1.
       */
      acsEvent = BKR94ACS_ACS_EVENT_Q;
      inputToBAj = bkr94acsVoted(a)[origin];
      fanoutTriggered = a->threshold ? 1 : 0;
      postCountOneAtNT = 0;
      postCountAllN = 0;
      doInput1 = 0;
      doInput0Fanout = 0;
      doOutputSubset = 0;
#include "bkr94acsRules.c"
      /* Step 2/3 outputs are unreachable on a Q event; the dispatch
       * still resolves them to 0 at every leaf. */
      (void)doInput0Fanout;
      (void)doOutputSubset;
      if (doInput1)
        nact += bkr94acsVote(a, origin, 1, &out[nact]);
    }
  }

  return (nact);
}

unsigned int
bkr94acsConsensusInput(
  struct bkr94acs *a
 ,unsigned char origin
 ,unsigned char round
 ,unsigned char broadcaster
 ,unsigned char type
 ,unsigned char from
 ,unsigned char value
 ,struct bkr94acsAct *out
){
  unsigned char *pipe;
  unsigned long cf1sz;
  unsigned long f4off;
  unsigned int N;
  unsigned int mr;
  struct bracha87Fig1 *f1;
  struct bracha87Fig3 *f3;
  struct bracha87Fig4 *f4;
  unsigned char f1out[3];
  unsigned int nf1;
  unsigned int nact;
  unsigned int k;
  unsigned char *nextRound;
  unsigned char acsEvent;
  unsigned char inputToBAj;
  unsigned char fanoutTriggered;
  unsigned char postCountOneAtNT;
  unsigned char postCountAllN;
  unsigned char doInput1;
  unsigned char doInput0Fanout;
  unsigned char doOutputSubset;

  if (!a || origin > a->n || broadcaster > a->n || from > a->n || !out)
    return (0);
  if (round >= maxRounds(a))
    return (0);

  /*
   * Two intentional non-short-circuits — both are Bracha post-decide
   * continuation (pitfall #1) applied at different layers.
   *
   * 1. We do NOT short-circuit on bkr94acsDecision[origin] != 0xFF.
   *    Bracha Fig4 requires a decided process to continue
   *    broadcasting so peers lagging in THIS BA can reach n-t
   *    validated and decide.  The BKR94ACS_ACT_BA_DECIDED emission
   *    is gated by Fig4Round returning DECIDE, which fires exactly
   *    once per BA, so idempotence holds.
   *
   * 2. We do NOT short-circuit on a->complete.  A locally-complete
   *    peer has decided all N BAs but other peers may still be
   *    working on some BAs.  Their Fig1 instances for (origin_X,
   *    round_Y, broadcaster_THIS) wait on THIS peer's continued
   *    echoes and readys to cross n-t thresholds.  Dropping inputs
   *    after local complete strands lagging peers — a classic
   *    post-decide stall.  BKR94ACS_ACT_COMPLETE emission is gated
   *    by nDecided crossing N, which happens once; continuing past
   *    complete cannot emit a second BKR94ACS_ACT_COMPLETE.
   *    Application exit is a separate concern (see progress-silence
   *    quorum).
   */

  /* Compute pipeline base once — avoids redundant external calls */
  N = A_N(a);
  mr = maxRounds(a);
  cf1sz = conF1Sz(a);
  f4off = (unsigned long)mr * N * cf1sz;
  pipe = conBase(a) + (unsigned int)origin * conPipelineSz(a);
  f1 = (struct bracha87Fig1 *)(pipe
    + ((unsigned long)round * N + broadcaster) * cf1sz);
  f4 = (struct bracha87Fig4 *)(pipe + f4off);
  f3 = &f4->fig3;
  nextRound = &bkr94acsConNextRound(a)[origin];
  nact = 0;

  nf1 = bracha87Fig1Input(f1, type, from, &value, f1out);

  for (k = 0; k < nf1; ++k) {
    if (f1out[k] == BRACHA87_ACCEPT) {
      const unsigned char *cv;
      unsigned int vc;

      cv = bracha87Fig1Value(f1);
      if (!cv)
        continue;

      /* Fig3 sender is the broadcaster (whose broadcast was accepted) */
      bracha87Fig3Accept(f3, round, broadcaster, cv[0], &vc);

      /*
       * Check for completed rounds (including cascades).
       * Same pattern as example/bracha87.c.
       */
      while (*nextRound < mr
          && bracha87Fig3RoundComplete(f3, *nextRound)) {
        unsigned char rsnd[256];
        unsigned char rval[256];
        unsigned int rcnt;
        unsigned int act;

        rcnt = bracha87Fig3GetValid(f3, *nextRound, rsnd, rval);
        act = bracha87Fig4Round(f4, *nextRound, rcnt, rsnd, rval);
        ++*nextRound;

        if (act & BRACHA87_DECIDE) {
          /* BA-decided notification (always emitted; not part of
           * BKR94 rules, just an observability signal). */
          bkr94acsDecision(a)[origin] = f4->decision;
          out[nact].value = 0;
          out[nact].act = BKR94ACS_ACT_BA_DECIDED;
          out[nact].origin = origin;
          out[nact].round = 0;
          out[nact].type = 0;
          out[nact].conValue = f4->decision;
          out[nact].broadcaster = 0;
          ++nact;
          ++a->nDecided;
          if (f4->decision == 1)
            ++a->nDecidedOne;

          /*
           * BKR94 Steps 2 and 3 dispatch.  The bridge maps "post-
           * output BA-output-1 count >= n-t" to nDecidedOne >= n-t
           * (post-increment); "post-output BA-output count == n" to
           * nDecided >= n.  Step 2's threshold-firing rule (vote 0
           * to all unvoted BAs once when count reaches n-t) and
           * Step 3's all-decided rule (output SubSet) are both
           * guarded by their respective post-output predicates and
           * by acsEvent's branching on BA_1 vs BA_0.  Lemma 2 Part A
           * case (i) requires Step 2's trigger to be a BA decide of
           * 1, not a Fig1 accept; the .dtc carries that exactly.
           */
          acsEvent = f4->decision
            ? BKR94ACS_ACS_EVENT_BA1
            : BKR94ACS_ACS_EVENT_BA0;
          inputToBAj = bkr94acsVoted(a)[origin];
          fanoutTriggered = a->threshold ? 1 : 0;
          postCountOneAtNT = a->nDecidedOne >= A_N(a) - a->t;
          postCountAllN = a->nDecided >= A_N(a);
          doInput1 = 0;
          doInput0Fanout = 0;
          doOutputSubset = 0;
#include "bkr94acsRules.c"
          /* Step 1 is unreachable on a BA-output event; the dispatch
           * still resolves doInput1 to 0 at every leaf. */
          (void)doInput1;
          if (doInput0Fanout) {
            unsigned int j;

            a->threshold = 1;
            for (j = 0; j < A_N(a); ++j)
              nact += bkr94acsVote(a, j, 0, &out[nact]);
          }
          if (doOutputSubset) {
            a->complete = 1;
            out[nact].value = 0;
            out[nact].act = BKR94ACS_ACT_COMPLETE;
            out[nact].origin = 0;
            out[nact].round = 0;
            out[nact].type = 0;
            out[nact].conValue = 0;
            out[nact].broadcaster = 0;
            ++nact;
          }
        }

        if ((act & BRACHA87_BROADCAST)
         && *nextRound < mr) {
          /*
           * Broadcast INITIAL for the new consensus round
           * (self is broadcaster).  Mark the corresponding
           * (origin, round=*nextRound, broadcaster=self)
           * consensus Fig1 as the originator and store the
           * value so bkr94acsPump replays this INITIAL on
           * subsequent ticks until ECHOED (loopback or echo /
           * ready cascade) takes over.  Same pattern as the
           * round-0 case in bkr94acsVote.
           */
          {
            unsigned char binary;

            binary = f4->value;
            bracha87Fig1Origin(conF1(a, origin, *nextRound, a->self),
                               &binary);
          }

          out[nact].value = 0;
          out[nact].act = BKR94ACS_ACT_CON_SEND;
          out[nact].origin = origin;
          out[nact].round = *nextRound;
          out[nact].type = BRACHA87_INITIAL;
          out[nact].conValue = f4->value;
          out[nact].broadcaster = a->self;
          ++nact;
        }
      }
      continue;
    }

    /* ECHO_ALL or READY_ALL for the consensus Fig1 */
    {
      const unsigned char *cv;

      cv = bracha87Fig1Value(f1);
      if (!cv)
        continue;

      out[nact].value = 0;
      out[nact].act = BKR94ACS_ACT_CON_SEND;
      out[nact].origin = origin;
      out[nact].round = round;
      out[nact].type = (f1out[k] == BRACHA87_ECHO_ALL)
        ? BRACHA87_ECHO : BRACHA87_READY;
      out[nact].conValue = cv[0];
      out[nact].broadcaster = broadcaster;
      ++nact;
    }
  }

  return (nact);
}

unsigned int
bkr94acsSubset(
  const struct bkr94acs *a
 ,unsigned char *origins
){
  unsigned int cnt;
  unsigned int i;
  const unsigned char *dec;

  if (!a || !origins)
    return (0);

  /*
   * BKR94 Step 3 read: SubSet_i = { j : BA_j had output 1 }.
   * Lemma 2 Part A gives |SubSet| >= 2t+1 = n-t; Part C gives
   * cross-peer agreement on SubSet; Part D gives Q(j)=1 for every
   * j in SubSet.  Caller must gate this on a->complete to
   * observe the final subset; a mid-run read reports the partial
   * set of decided-1 origins.
   */
  dec = bkr94acsDecision(a);
  cnt = 0;
  for (i = 0; i < A_N(a); ++i) {
    if (dec[i] == 1)
      origins[cnt++] = (unsigned char)i;
  }
  return (cnt);
}

const unsigned char *
bkr94acsProposalValue(
  const struct bkr94acs *a
 ,unsigned char origin
){
  if (!a || origin > a->n)
    return (0);
  return (bracha87Fig1Value(propF1(a, origin)));
}

unsigned int
bkr94acsPropose(
  struct bkr94acs *a
 ,const unsigned char *value
 ,struct bkr94acsAct *out
){
  if (!a || !value || !out)
    return (0);

  /*
   * Mark the local proposal Fig1 (origin = self) as the
   * broadcast originator.  bracha87Fig1Origin sets F1_ORIGIN
   * and copies value into the committed-value slot so
   * bracha87Fig1Value returns it (without setting ECHOED --
   * Rule 1 still fires from bkr94acsProposalInput receiving
   * (initial, v) via the network loopback).
   *
   * Until loopback or echo / ready cascade sets ECHOED,
   * bkr94acsPump replays BKR94ACS_ACT_PROP_INITIAL every
   * tick.  After ECHOED, the proposal Fig1's own BPR carries
   * the value forward via echo / ready replays.
   */
  bracha87Fig1Origin(propF1(a, a->self), value);

  out->value = bracha87Fig1Value(propF1(a, a->self));
  out->act = BKR94ACS_ACT_PROP_SEND;
  out->origin = a->self;
  out->round = 0;
  out->type = BRACHA87_INITIAL;
  out->conValue = 0;
  out->broadcaster = 0;
  return (1);
}

/*
 * BPR pump.  Walks the cursor forward until a Fig1 instance
 * with replay output is found (or the cursor wraps back to its
 * starting position with no work).  Per-call ceiling:
 * BKR94ACS_PUMP_MAX_ACTS (3 actions = INITIAL + ECHO + READY
 * for one Fig1).
 *
 * Cursor walks two phases:
 *   phase 0: proposal Fig1s, origin 0..N-1
 *   phase 1: consensus Fig1s, (origin, round, broadcaster)
 *            in that order; round bounded by the per-origin
 *            conNextRound (no committed state past the
 *            current Fig4 round).
 *
 * Per-origin pump-gate (see bkr94acs.dtc BPR section) trims
 * the proposal walk for BAs that have decided 0 (excluded
 * from SubSet); the consensus walk is unconditional, with
 * Fig1Bpr returning 0 on uncommitted instances.
 *
 * Internal helpers fold each cursor advance step into the
 * walker.  Plain-C three-arm switch over BA decision is the
 * BPR gate (see bkr94acs.dtc BPR documentation block); the
 * pitfall #1 reasoning is captured in the .dtc, the C
 * implementation is the trivial mechanical guard.
 */
static int
bkr94acsPumpOriginGate(
  const struct bkr94acs *a
 ,unsigned int origin
){
  unsigned char dec;

  /*
   * BA decision states:
   *   0xFF -> undecided    -> pump (Q(j)=1 may still be
   *                                 learned by other peers)
   *   0    -> decided 0    -> skip (j excluded; Step 2's
   *                                 fanout already conveyed
   *                                 our vote)
   *   1    -> decided 1    -> pump (Bracha post-decide
   *                                 continuation: peers that
   *                                 have not yet observed
   *                                 Fig1 ACCEPT for j still
   *                                 need our echoes/readys)
   */
  dec = bkr94acsDecision(a)[origin];
  return (dec != 0);  /* 0xFF and 1 -> pump; 0 -> skip */
}

static unsigned int
bkr94acsPumpEmitProposal(
  struct bkr94acs *a
 ,unsigned int origin
 ,struct bkr94acsAct *out
){
  struct bracha87Fig1 *f1;
  const unsigned char *cv;
  unsigned char f1out[3];
  unsigned int n;
  unsigned int k;
  unsigned int nact;

  f1 = propF1(a, origin);
  n = bracha87Fig1Bpr(f1, f1out);
  if (!n)
    return (0);
  cv = bracha87Fig1Value(f1);
  if (!cv)
    return (0);

  nact = 0;
  for (k = 0; k < n; ++k) {
    out[nact].value = cv;
    out[nact].act = BKR94ACS_ACT_PROP_SEND;
    out[nact].origin = (unsigned char)origin;
    out[nact].round = 0;
    out[nact].type = (f1out[k] == BRACHA87_INITIAL_ALL)
                     ? BRACHA87_INITIAL
                   : (f1out[k] == BRACHA87_ECHO_ALL)
                     ? BRACHA87_ECHO
                   :   BRACHA87_READY;
    out[nact].conValue = 0;
    out[nact].broadcaster = 0;
    ++nact;
  }
  return (nact);
}

static unsigned int
bkr94acsPumpEmitConsensus(
  struct bkr94acs *a
 ,unsigned int origin
 ,unsigned int round
 ,unsigned int broadcaster
 ,struct bkr94acsAct *out
){
  struct bracha87Fig1 *f1;
  const unsigned char *cv;
  unsigned char f1out[3];
  unsigned int n;
  unsigned int k;
  unsigned int nact;

  f1 = conF1(a, origin, round, broadcaster);
  n = bracha87Fig1Bpr(f1, f1out);
  if (!n)
    return (0);
  cv = bracha87Fig1Value(f1);
  if (!cv)
    return (0);

  nact = 0;
  for (k = 0; k < n; ++k) {
    out[nact].value = 0;
    out[nact].act = BKR94ACS_ACT_CON_SEND;
    out[nact].origin = (unsigned char)origin;
    out[nact].round = (unsigned char)round;
    out[nact].type = (f1out[k] == BRACHA87_INITIAL_ALL)
                     ? BRACHA87_INITIAL
                   : (f1out[k] == BRACHA87_ECHO_ALL)
                     ? BRACHA87_ECHO
                   :   BRACHA87_READY;
    out[nact].conValue = cv[0];
    out[nact].broadcaster = (unsigned char)broadcaster;
    ++nact;
  }
  return (nact);
}

unsigned int
bkr94acsPump(
  struct bkr94acs *a
 ,struct bkr94acsAct *out
){
  unsigned int N;
  unsigned int mr;
  unsigned int swept;
  unsigned int total;
  unsigned int nact;

  if (!a || !out)
    return (0);

  N = A_N(a);
  mr = maxRounds(a);
  /*
   * Cursor space size: N proposals + N origins * mr rounds *
   * N broadcasters consensus Fig1s.  swept counts cursor
   * advances; total bounds the maximum walk before declaring
   * idle (one full cycle through the entire space).
   */
  total = N + N * mr * N;
  swept = 0;

  while (swept < total) {
    nact = 0;

    if (a->cursorPhase == 0) {
      /* Proposal Fig1 cursor */
      if (bkr94acsPumpOriginGate(a, a->cursorOrigin))
        nact = bkr94acsPumpEmitProposal(a, a->cursorOrigin, out);

      /* Advance: next origin; on wrap, switch to consensus phase */
      ++a->cursorOrigin;
      if (a->cursorOrigin >= N) {
        a->cursorOrigin = 0;
        a->cursorPhase = 1;
        a->cursorRound = 0;
        a->cursorBroadcaster = 0;
      }
    } else {
      /* Consensus Fig1 cursor: (origin, round, broadcaster) */
      nact = bkr94acsPumpEmitConsensus(a, a->cursorOrigin,
        a->cursorRound, a->cursorBroadcaster, out);

      /* Advance broadcaster, then round, then origin */
      ++a->cursorBroadcaster;
      if (a->cursorBroadcaster >= N) {
        a->cursorBroadcaster = 0;
        ++a->cursorRound;
        if (a->cursorRound >= mr) {
          a->cursorRound = 0;
          ++a->cursorOrigin;
          if (a->cursorOrigin >= N) {
            a->cursorOrigin = 0;
            a->cursorPhase = 0;
          }
        }
      }
    }

    ++swept;
    if (nact)
      return (nact);
  }
  return (0);
}

/*------------------------------------------------------------------------*/
/*  Diagnostic accessors                                                  */
/*------------------------------------------------------------------------*/

/*
 * Identity bytes for chanBlbChnRsec-style transports.  See
 * bkr94acs.h for the field-usage contract.
 */
unsigned int
bkr94acsActIdentity(
  const struct bkr94acsAct *act
 ,unsigned char *out
 ,unsigned int outCap
){
  if (!act || !out || outCap < BKR94ACS_ACT_IDENTITY_LEN)
    return (0);
  if (act->act != BKR94ACS_ACT_PROP_SEND
   && act->act != BKR94ACS_ACT_CON_SEND)
    return (0);
  out[0] = act->act;
  out[1] = act->origin;
  out[2] = act->round;       /* 0 for PROP_SEND */
  out[3] = act->broadcaster; /* 0 for PROP_SEND */
  out[4] = act->type;
  return (BKR94ACS_ACT_IDENTITY_LEN);
}

unsigned char
bkr94acsBaDecision(
  const struct bkr94acs *a
 ,unsigned char origin
){
  if (!a || origin > a->n)
    return (0xFF);
  return (bkr94acsDecision(a)[origin]);
}

unsigned int
bkr94acsCommittedFig1Count(
  const struct bkr94acs *a
){
  unsigned int N;
  unsigned int mr;
  unsigned int origin;
  unsigned int round;
  unsigned int bcast;
  unsigned int count;
  unsigned char committedMask;
  const unsigned char *nextRound;
  const struct bracha87Fig1 *f1;

  if (!a)
    return (0);

  /*
   * Committed = any of F1_ORIGIN / F1_ECHOED / F1_RDSENT set.
   * These are the flags that drive Bpr replay output; F1_ACCEPTED
   * piggybacks on F1_RDSENT remaining set post-accept (pitfall 10),
   * so it is implicitly counted.
   */
  committedMask = (unsigned char)(BRACHA87_F1_ECHOED
                                | BRACHA87_F1_RDSENT
                                | BRACHA87_F1_ORIGIN);

  N = A_N(a);
  mr = maxRounds(a);
  nextRound = bkr94acsConNextRound(a);
  count = 0;

  for (origin = 0; origin < N; ++origin) {
    f1 = propF1((struct bkr94acs *)a, (unsigned char)origin);
    if (f1->flags & committedMask)
      ++count;
  }

  /*
   * Consensus Fig1s are committed only at rounds the BA has
   * actually entered (round < conNextRound[origin]).  Past that,
   * no Fig1Input has run; flags are zero by calloc.  Walking
   * the full mr * N space is correct but wasteful; we cap on
   * conNextRound to keep this O(active).
   */
  for (origin = 0; origin < N; ++origin) {
    unsigned int activeRounds;

    activeRounds = nextRound[origin];
    if (activeRounds > mr)
      activeRounds = mr;
    for (round = 0; round < activeRounds; ++round) {
      for (bcast = 0; bcast < N; ++bcast) {
        f1 = conF1((struct bkr94acs *)a,
                   (unsigned char)origin,
                   (unsigned char)round,
                   (unsigned char)bcast);
        if (f1->flags & committedMask)
          ++count;
      }
    }
  }

  return (count);
}

