/*
 * BrachaAsynchronousByzantineAgreementProtocols - Asynchronous Common Subset
 * Copyright (C) 2026 G. David Butler <gdb@dbSystems.com>
 *
 * This file is part of BrachaAsynchronousByzantineAgreementProtocols
 *
 * BrachaAsynchronousByzantineAgreementProtocols is free software: you can
 * redistribute it and/or modify it under the terms of the GNU Lesser General
 * Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * BrachaAsynchronousByzantineAgreementProtocols is distributed in the hope
 * that it will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * Asynchronous Common Subset — BKR construction.
 *
 * Composes N Fig1 instances (proposal reliable broadcast) with
 * N Fig4 instances (binary consensus on inclusion).
 */

#include <string.h>
#include "acs.h"

#define A_N(a) ((unsigned int)(a)->n + 1)

/*------------------------------------------------------------------------*/
/*  Internal layout helpers                                               */
/*                                                                        */
/*  data[] layout (N = n + 1, MR = maxPhases * 3):                        */
/*    voted[N]           per-origin vote status                           */
/*    baDecision[N]      per-origin BA decision                           */
/*    conNextRound[N]    per-origin next consensus round to check         */
/*    propFig1 area      N * propF1Sz bytes                               */
/*    conPipeline area   N * (MR * N * conF1Sz + fig4Sz) bytes            */
/*------------------------------------------------------------------------*/

/* Per-origin vote status */
#define ACS_VOTE_NONE 0
#define ACS_VOTE_ONE  1
#define ACS_VOTE_ZERO 2

static unsigned char *
acsVoted(
  struct acs *a
){
  return (a->data);
}

static unsigned char *
acsDecision(
  struct acs *a
){
  return (a->data + A_N(a));
}

static unsigned char *
acsConNextRound(
  struct acs *a
){
  return (a->data + 2 * A_N(a));
}

static unsigned long
propF1Sz(
  struct acs *a
){
  return (bracha87Fig1Sz(a->n, a->vLen));
}

static unsigned long
conF1Sz(
  struct acs *a
){
  return (bracha87Fig1Sz(a->n, 0));
}

static unsigned long
fig4Sz(
  struct acs *a
){
  return (bracha87Fig4Sz(a->n, a->maxPhases));
}

static unsigned int
maxRounds(
  struct acs *a
){
  return ((unsigned int)a->maxPhases * 3);
}

/* Base of proposal Fig1 area */
static unsigned char *
propF1Base(
  struct acs *a
){
  return (a->data + 3 * A_N(a));
}

/* Proposal Fig1 instance for origin j */
static struct bracha87Fig1 *
propF1(
  struct acs *a
 ,unsigned int j
){
  return ((struct bracha87Fig1 *)(propF1Base(a) + j * propF1Sz(a)));
}

/* Base of consensus pipeline area */
static unsigned char *
conBase(
  struct acs *a
){
  return (propF1Base(a) + A_N(a) * propF1Sz(a));
}

/* Size of one consensus pipeline (for origin j) */
static unsigned long
conPipelineSz(
  struct acs *a
){
  return ((unsigned long)maxRounds(a) * A_N(a) * conF1Sz(a) + fig4Sz(a));
}

/* Consensus Fig1 instance for origin j, round r, broadcaster k */
static struct bracha87Fig1 *
conF1(
  struct acs *a
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
  struct acs *a
 ,unsigned int j
){
  unsigned char *base;

  base = conBase(a) + j * conPipelineSz(a);
  return ((struct bracha87Fig4 *)(base
    + (unsigned long)maxRounds(a) * A_N(a) * conF1Sz(a)));
}

/* Consensus Fig3 (embedded in Fig4) for origin j */
static struct bracha87Fig3 *
conF3(
  struct acs *a
 ,unsigned int j
){
  return ((struct bracha87Fig3 *)conF4(a, j)->data);
}

/*------------------------------------------------------------------------*/
/*  Coin function — deterministic alternating (same as consensus.c)       */
/*------------------------------------------------------------------------*/

static unsigned char
acsCoin(
  void *closure
 ,unsigned char phase
){
  (void)closure;
  return (phase % 2);
}

/*------------------------------------------------------------------------*/
/*  Public API                                                            */
/*------------------------------------------------------------------------*/

unsigned long
acsSz(
  unsigned int n
 ,unsigned int vLen
 ,unsigned int maxPhases
){
  unsigned int N;
  unsigned long pf1;
  unsigned long cf1;
  unsigned long f4;
  unsigned long mr;
  unsigned long conPipe;

  N = n + 1;
  pf1 = bracha87Fig1Sz(n, vLen);
  cf1 = bracha87Fig1Sz(n, 0);
  f4 = bracha87Fig4Sz(n, maxPhases);
  mr = (unsigned long)maxPhases * 3;
  conPipe = mr * N * cf1 + f4;

  return (sizeof (struct acs) - 1
    + 3 * N                    /* voted + baDecision + conNextRound */
    + N * pf1                  /* proposal Fig1 instances */
    + N * conPipe);            /* consensus pipelines */
}

void
acsInit(
  struct acs *a
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

  memset(a, 0, acsSz(n, vLen, maxPhases));
  a->n = n;
  a->t = t;
  a->vLen = vLen;
  a->maxPhases = maxPhases;
  a->self = self;

  N = (unsigned int)n + 1;

  /* Mark all BA decisions as undecided */
  memset(acsDecision(a), 0xFF, N);

  /* Initialize proposal Fig1 instances */
  for (i = 0; i < N; ++i)
    bracha87Fig1Init(propF1(a, i), n, t, vLen);

  /* Initialize consensus pipelines */
  for (i = 0; i < N; ++i) {
    unsigned int mr2;

    mr2 = (unsigned int)maxPhases * 3;
    for (r = 0; r < mr2; ++r)
      for (j = 0; j < N; ++j)
        bracha87Fig1Init(conF1(a, i, r, j), n, t, 0);
    bracha87Fig4Init(conF4(a, i), n, t, maxPhases, 0,
                     coin ? coin : acsCoin, coinClosure);
  }
}

/*
 * Internal: start a binary consensus vote for origin j.
 * Generates ACS_ACT_CON_SEND actions for the INITIAL broadcast.
 * Returns number of actions added.
 */
static unsigned int
acsVote(
  struct acs *a
 ,unsigned int origin
 ,unsigned char vote
 ,struct acsAct *out
){
  unsigned char *voted;

  voted = acsVoted(a);
  if (voted[origin] != ACS_VOTE_NONE)
    return (0);

  voted[origin] = vote ? ACS_VOTE_ONE : ACS_VOTE_ZERO;

  /* Set the Fig4 initial value for this consensus */
  conF4(a, origin)->value = vote;

  /* Generate INITIAL broadcast to all peers */
  out->act = ACS_ACT_CON_SEND;
  out->origin = (unsigned char)origin;
  out->round = 0;
  out->conType = BRACHA87_INITIAL;
  out->conValue = vote;
  out->broadcaster = a->self;
  return (1);
}

unsigned int
acsProposalInput(
  struct acs *a
 ,unsigned char origin
 ,unsigned char type
 ,unsigned char from
 ,const unsigned char *value
 ,struct acsAct *out
){
  struct bracha87Fig1 *f1;
  unsigned char f1out[3];
  unsigned int nf1;
  unsigned int nact;
  unsigned int k;

  if (!a || origin > a->n || from > a->n || !value || !out)
    return (0);
  if (a->complete)
    return (0);

  f1 = propF1(a, origin);
  nf1 = bracha87Fig1Input(f1, type, from, value, f1out);
  nact = 0;

  for (k = 0; k < nf1; ++k) {
    if (f1out[k] == BRACHA87_ECHO_ALL) {
      out[nact].act = ACS_ACT_PROP_ECHO;
      out[nact].origin = origin;
      out[nact].round = 0;
      out[nact].conType = 0;
      out[nact].conValue = 0;
      ++nact;
    } else if (f1out[k] == BRACHA87_READY_ALL) {
      out[nact].act = ACS_ACT_PROP_READY;
      out[nact].origin = origin;
      out[nact].round = 0;
      out[nact].conType = 0;
      out[nact].conValue = 0;
      ++nact;
    } else if (f1out[k] == BRACHA87_ACCEPT) {
      /*
       * Proposal accepted for this origin.
       * Vote 1 in this origin's binary consensus.
       */
      nact += acsVote(a, origin, 1, &out[nact]);
      ++a->nAccepted;

      /*
       * If we've now accepted n-t proposals, vote 0 for all
       * origins where we haven't voted yet.
       */
      if (!a->threshold
       && a->nAccepted >= A_N(a) - a->t) {
        unsigned int j;

        a->threshold = 1;
        for (j = 0; j < A_N(a); ++j)
          nact += acsVote(a, j, 0, &out[nact]);
      }
    }
  }

  return (nact);
}

unsigned int
acsConsensusInput(
  struct acs *a
 ,unsigned char origin
 ,unsigned char round
 ,unsigned char broadcaster
 ,unsigned char type
 ,unsigned char from
 ,unsigned char value
 ,struct acsAct *out
){
  struct bracha87Fig1 *f1;
  struct bracha87Fig3 *f3;
  struct bracha87Fig4 *f4;
  unsigned char f1out[3];
  unsigned int nf1;
  unsigned int nact;
  unsigned int k;
  unsigned char *nextRound;

  if (!a || origin > a->n || broadcaster > a->n || from > a->n || !out)
    return (0);
  if (a->complete)
    return (0);
  if (round >= maxRounds(a))
    return (0);
  /* Skip if this BA already decided */
  if (acsDecision(a)[origin] != 0xFF)
    return (0);

  /* Route to Fig1 instance by (round, broadcaster), feed from as sender */
  f1 = conF1(a, origin, round, broadcaster);
  f3 = conF3(a, origin);
  f4 = conF4(a, origin);
  nextRound = &acsConNextRound(a)[origin];
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
       * Same pattern as consensus.c.
       */
      while (*nextRound < maxRounds(a)
          && bracha87Fig3RoundComplete(f3, *nextRound)) {
        unsigned char rsnd[256];
        unsigned char rval[256];
        unsigned int rcnt;
        unsigned int act;

        rcnt = bracha87Fig3GetValid(f3, *nextRound, rsnd, rval);
        act = bracha87Fig4Round(f4, *nextRound, rcnt, rsnd, rval);
        ++*nextRound;

        if (act & BRACHA87_DECIDE) {
          acsDecision(a)[origin] = f4->decision;
          out[nact].act = ACS_ACT_BA_DECIDED;
          out[nact].origin = origin;
          out[nact].round = 0;
          out[nact].conType = 0;
          out[nact].conValue = f4->decision;
          out[nact].broadcaster = 0;
          ++nact;
          ++a->nDecided;

          if (a->nDecided >= A_N(a)) {
            a->complete = 1;
            out[nact].act = ACS_ACT_COMPLETE;
            out[nact].origin = 0;
            out[nact].round = 0;
            out[nact].conType = 0;
            out[nact].conValue = 0;
            out[nact].broadcaster = 0;
            ++nact;
          }
        }

        if ((act & BRACHA87_BROADCAST)
         && *nextRound < maxRounds(a)) {
          /* Broadcast INITIAL for the new consensus round (self is broadcaster) */
          out[nact].act = ACS_ACT_CON_SEND;
          out[nact].origin = origin;
          out[nact].round = *nextRound;
          out[nact].conType = BRACHA87_INITIAL;
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

      out[nact].act = ACS_ACT_CON_SEND;
      out[nact].origin = origin;
      out[nact].round = round;
      out[nact].conType = (f1out[k] == BRACHA87_ECHO_ALL)
        ? BRACHA87_ECHO : BRACHA87_READY;
      out[nact].conValue = cv[0];
      out[nact].broadcaster = broadcaster;
      ++nact;
    }
  }

  return (nact);
}

int
acsComplete(
  const struct acs *a
){
  if (!a)
    return (0);
  return (a->complete);
}

unsigned int
acsSubset(
  const struct acs *a
 ,unsigned char *origins
){
  unsigned int cnt;
  unsigned int i;
  const unsigned char *dec;

  if (!a || !origins)
    return (0);

  dec = ((const struct acs *)a)->data + A_N(a);
  cnt = 0;
  for (i = 0; i < A_N(a); ++i) {
    if (dec[i] == 1)
      origins[cnt++] = (unsigned char)i;
  }
  return (cnt);
}

const unsigned char *
acsProposalValue(
  const struct acs *a
 ,unsigned char origin
){
  if (!a || origin > a->n)
    return (0);
  return (bracha87Fig1Value(propF1((struct acs *)a, origin)));
}
