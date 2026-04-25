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
 * BKR94 Asynchronous Common Subset
 *
 * Direct implementation of Ben-Or/Kelmer/Rabin 1994, "Asynchronous
 * Secure Computations with Optimal Resilience (Extended Abstract),"
 * PODC '94, pages 183-192, Section 4 Figure 3 (Protocol
 * Agreement[Q]).  See BKR94ACS.txt for the line-by-line paper
 * extract this header and its companion .c file are aligned to.
 *
 * Composes Bracha87 Figure 1 (reliable broadcast) with Bracha87
 * Figure 4 (binary consensus) into multi-value agreement on a common
 * subset.  N reliable broadcasts distribute proposals; N binary
 * consensuses decide inclusion.
 *
 * BKR94 parameterizes the protocol by a predicate Q(j).  Under the
 * two paper assumptions — (1) Q eventually equals 1 for every honest
 * player, (2) every honest player eventually learns Q(j) for every j
 * — Protocol Agreement[Q] produces a common subset of size >= n-t of
 * players for whom Q(j) = 1.
 *
 *   This deployment: Q(j) = "Fig1 reliable broadcast for origin j
 *   has ACCEPTED" (the BKR94 MPC-construction equivalent is "P_j has
 *   properly shared his input").  Reliable broadcast gives both Q
 *   assumptions for free: Bracha87 Fig1 eventually accepts every
 *   honest broadcast at every honest receiver (Lemma 4).
 *
 * The three Figure 3 steps, per player P_i:
 *
 *   Step 1. For each j where you know Q(j)=1, vote 1 in BA_j.
 *   Step 2. When 2t+1 BAs have terminated with output 1, vote 0 in
 *           every BA where you have not yet entered a value.
 *   Step 3. Once all N BAs terminate, SubSet = { j : BA_j = 1 }.
 *
 * The "2t+1" in Step 2 is n-t in the paper's regime (n = 3t+1) and
 * is the implementation threshold for all supported (n, t).
 *
 * Pure state machine: no I/O, no threads, no dynamic allocation.
 * Caller provides memory and delivers messages.
 *
 * Two message classes on the network:
 *   BKR94ACS_CLS_PROPOSAL  — Fig1 messages carrying proposal values
 *   BKR94ACS_CLS_CONSENSUS — Fig1 messages for per-origin binary consensus
 *
 * Operational limits:
 *   n:         unsigned char, encodes process count 1..256 (n + 1)
 *   t:         unsigned char, max 85 (n + 1 > 3t required)
 *   vLen:      unsigned char, encodes proposal length 1..256 (vLen + 1)
 *   maxPhases: unsigned char, for binary consensus (per BKR94 BA instance)
 */

#ifndef BKR94ACS_H
#define BKR94ACS_H

#include "bracha87.h"

/*
 * Maximum peers for BKR94 ACS.
 * Bounded by the Bracha87 Fig1/Fig4 limits (unsigned char addressing).
 */
#define BKR94ACS_MAX_PEERS 256

/*************************************************************************/
/*  Message classes                                                      */
/*************************************************************************/

#define BKR94ACS_CLS_PROPOSAL  0   /* Fig1 reliable broadcast of proposals */
#define BKR94ACS_CLS_CONSENSUS 1   /* Fig1 messages for binary consensus */

/*************************************************************************/
/*  Output actions                                                       */
/*                                                                       */
/*  Returned in struct bkr94acsAct array from bkr94acsInput functions.   */
/*  Caller sends the described messages on the network.                  */
/*************************************************************************/

#define BKR94ACS_ACT_PROP_ECHO   1  /* send proposal echo to all for .origin */
#define BKR94ACS_ACT_PROP_READY  2  /* send proposal ready to all for .origin */
#define BKR94ACS_ACT_CON_SEND    3  /* send consensus msg: .origin .round .conType .conValue */
#define BKR94ACS_ACT_BA_DECIDED  4  /* BA for .origin decided .conValue */
#define BKR94ACS_ACT_COMPLETE    5  /* all N BAs decided; common subset final */

struct bkr94acsAct {
  unsigned char act;          /* BKR94ACS_ACT_* */
  unsigned char origin;       /* which origin this relates to */
  unsigned char round;        /* consensus round (BKR94ACS_ACT_CON_SEND only) */
  unsigned char conType;      /* BRACHA87_INITIAL/ECHO/READY (CON_SEND only) */
  unsigned char conValue;     /* binary value (CON_SEND, BA_DECIDED only) */
  unsigned char broadcaster;  /* who originated this Fig1 broadcast (CON_SEND) */
};

/*************************************************************************/
/*  BKR94 ACS state                                                      */
/*************************************************************************/

struct bkr94acs {
  unsigned char n;          /* process count encoding: actual = n + 1 */
  unsigned char t;          /* max Byzantine (n + 1 > 3t) */
  unsigned char vLen;       /* proposal value length encoding: actual = vLen + 1 */
  unsigned char maxPhases;  /* per binary consensus instance */
  unsigned char self;       /* this peer's index (needed for consensus routing) */
  unsigned char nDecidedOne;/* BKR94 Step 2 trigger: count of BAs decided with output 1 */
  unsigned char nDecided;   /* BKR94 Step 3 trigger: count of BAs that have decided */
  unsigned char threshold;  /* 1 iff BKR94 Step 2 has fired (vote-0 fanout done) */
  unsigned char complete;   /* 1 iff all N BAs decided (Step 3 complete) */
  /*
   * Pad data[] to a pointer-aligned offset so Fig4 instances carved
   * out of data[] are correctly aligned for their function-pointer
   * fields. With 9 leading chars we need 7 bytes of pad to reach
   * offset 16, which is a multiple of sizeof (void *) on all
   * common 32- and 64-bit ABIs.
   */
  unsigned char pad[7];
  unsigned char data[1];    /* variable: see bkr94acsSz */
};

/*
 * Layout of data[] (N = n + 1):
 *   voted[N]            per-origin: 0=not voted, 1=voted-1, 2=voted-0
 *   baDecision[N]       per-origin: 0xFF=undecided, 0=excluded, 1=included
 *   propFig1[N]         N Fig1 instances for proposal broadcast (each propF1Sz bytes)
 *   Per-origin consensus (N instances):
 *     conFig1[maxRounds * N]  Fig1 instances for consensus (each conF1Sz bytes)
 *     conFig4[1]              Fig4 instance (fig4Sz bytes)
 *     conNextRound            (unsigned char) next round to check
 */

/* Size in bytes needed for a BKR94 ACS instance */
unsigned long
bkr94acsSz(
  unsigned int             /* n: actual process count = n + 1 */
 ,unsigned int             /* vLen: actual proposal length = vLen + 1 */
 ,unsigned int             /* maxPhases: per binary consensus instance */
);

/*
 * Initialize a BKR94 ACS instance. Caller has allocated bkr94acsSz bytes.
 *
 * coin must be non-null. For Bracha's t < n/3 regime a per-peer
 * local source (e.g. arc4random) is appropriate; see Mostefaoui,
 * Perrin, Weibel (PODC 2024). A deterministic coin such as
 * phase%2 is safe only under a non-adversarial scheduler and is
 * not supplied as a default.
 */
void
bkr94acsInit(
  struct bkr94acs *
 ,unsigned char            /* n: actual process count = n + 1 */
 ,unsigned char            /* t */
 ,unsigned char            /* vLen: actual proposal length = vLen + 1 */
 ,unsigned char            /* maxPhases */
 ,unsigned char            /* self: this peer's index */
 ,bracha87CoinFn           /* coin function, must be non-null */
 ,void *                   /* coin closure */
);

/*
 * Maximum output actions from a single input call.
 *
 * Proposal input (BKR94ACS_CLS_PROPOSAL):
 *   up to 2 (echo/ready) + 1 (vote-1 from BKR94 Step 1 on accept).
 *   Step 2's vote-0 fanout lives in consensus input, not here.
 *   Bound: 3.
 *
 * Consensus input (BKR94ACS_CLS_CONSENSUS):
 *   up to 2 (echo/ready) from the Fig1 input, plus a cascade over
 *   newly-validated rounds.  Adversarial delivery can make Fig3's
 *   forward cascade validate many rounds in a single Fig3Accept call
 *   (round k+1 unlocks when round k crosses n-t, etc.), so the
 *   per-call ceiling is:
 *     2 (echo/ready) + M (CON_SEND per round advanced)
 *       + 1 (BA_DECIDED, fires at most once per BA)
 *       + N (BKR94 Step 2 vote-0 fanout, fires at most once per ACS)
 *       + 1 (COMPLETE, fires at most once per ACS instance)
 *   where M = maxPhases * 3 is the BA's round bound and N = n + 1.
 *   Bound: M + N + 4.
 *
 * Consensus case strictly dominates, so the unified bound is
 * M + N + 4.  BKR94ACS_MAX_ACTS takes maxPhases so the cascade bound
 * is exact for the configured consensus, not the 85-phase ceiling.
 */
#define BKR94ACS_MAX_ACTS(n, maxPhases) \
  ((unsigned int)(maxPhases) * 3 + (unsigned int)(n) + 5)

/*
 * Process a proposal broadcast message (BKR94ACS_CLS_PROPOSAL).
 *
 * These are Fig1 messages carrying proposal values.
 * Returns number of actions written to out[].
 * Caller provides out[] with room for BKR94ACS_MAX_ACTS(n, maxPhases) entries.
 *
 * On BKR94ACS_ACT_PROP_ECHO / BKR94ACS_ACT_PROP_READY:
 *   Caller sends the proposal echo/ready to all peers.
 *   Value to send: bkr94acsProposalValue(acs, origin).
 *
 * On BKR94ACS_ACT_CON_SEND:
 *   Caller sends a consensus message to all peers.
 *   Fields: .origin, .round, .conType, .conValue.
 */
unsigned int
bkr94acsProposalInput(
  struct bkr94acs *
 ,unsigned char            /* origin: whose proposal */
 ,unsigned char            /* type: BRACHA87_INITIAL/ECHO/READY */
 ,unsigned char            /* from: sender of this message */
 ,const unsigned char *    /* value: vLen + 1 bytes */
 ,struct bkr94acsAct *     /* out: actions, room for BKR94ACS_MAX_ACTS(n, maxPhases) */
);

/*
 * Process a consensus message (BKR94ACS_CLS_CONSENSUS).
 *
 * These are Fig1 messages for the binary consensus on origin's inclusion.
 * Returns number of actions written to out[].
 * Caller provides out[] with room for BKR94ACS_MAX_ACTS(n, maxPhases) entries.
 *
 * The consensus for each origin is a full Fig1+Fig3+Fig4 pipeline
 * (same structure as example/bracha87.c), deciding 0 or 1.
 *
 * On BKR94ACS_ACT_CON_SEND:
 *   Caller sends a consensus message to all peers.
 *
 * On BKR94ACS_ACT_BA_DECIDED:
 *   BA for .origin decided .conValue (0=exclude, 1=include).
 *
 * On BKR94ACS_ACT_COMPLETE:
 *   All N BAs decided. Common subset is final.
 *   Query with bkr94acsSubset().
 */
unsigned int
bkr94acsConsensusInput(
  struct bkr94acs *
 ,unsigned char            /* origin: which origin's consensus */
 ,unsigned char            /* round: consensus round (0-based) */
 ,unsigned char            /* broadcaster: who initiated this Fig1 broadcast */
 ,unsigned char            /* type: BRACHA87_INITIAL/ECHO/READY */
 ,unsigned char            /* from: sender of this message */
 ,unsigned char            /* value: binary consensus value */
 ,struct bkr94acsAct *     /* out: actions, room for BKR94ACS_MAX_ACTS(n, maxPhases) */
);

/*
 * Query: is the common subset decided?
 */
int
bkr94acsComplete(
  const struct bkr94acs *
);

/*
 * Query: get the decided common subset.
 * Returns count of included origins.
 * Fills origins[] with the included origin indices (caller provides n entries).
 * Only valid after bkr94acsComplete() returns true.
 */
unsigned int
bkr94acsSubset(
  const struct bkr94acs *
 ,unsigned char *          /* origins out, n entries */
);

/*
 * Query: get the accepted proposal value for an origin.
 * Returns pointer to the vLen + 1 byte value, or 0 if not yet accepted.
 */
const unsigned char *
bkr94acsProposalValue(
  const struct bkr94acs *
 ,unsigned char            /* origin */
);

#endif /* BKR94ACS_H */
