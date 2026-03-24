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
 * Asynchronous Common Subset (ACS)
 *
 * Composition of Bracha's reliable broadcast (Figure 1) and binary
 * consensus (Figure 4) into multi-value agreement on a common subset.
 *
 * Construction (BKR 1994, Miller et al. 2016 "HoneyBadger"):
 *   Each of N peers proposes a value (arbitrary bytes, up to vLen+1).
 *   N reliable broadcasts (Fig1) distribute proposals.
 *   N binary consensuses (Fig4) decide inclusion.
 *
 *   When Fig1 for origin j reaches ACCEPT: vote 1 in BA_j.
 *   When n-t Fig1 broadcasts accepted: vote 0 in all remaining BAs.
 *   Common subset = { j : BA_j decided 1 }.
 *   Subset contains at least n-t origins.
 *
 * Pure state machine: no I/O, no threads, no dynamic allocation.
 * Caller provides memory and delivers messages.
 *
 * Two message classes on the network:
 *   ACS_CLS_PROPOSAL  — Fig1 messages carrying proposal values
 *   ACS_CLS_CONSENSUS — Fig1 messages for per-origin binary consensus
 *
 * Operational limits:
 *   n:         unsigned char, encodes process count 1..256 (n + 1)
 *   t:         unsigned char, max 85 (n + 1 > 3t required)
 *   vLen:      unsigned char, encodes proposal length 1..256 (vLen + 1)
 *   maxPhases: unsigned char, for binary consensus (per BKR instance)
 */

#ifndef _ACS_H
#define _ACS_H

#include "bracha87.h"

/*
 * Maximum peers for ACS.
 * Bounded by the Fig1/Fig4 limits (unsigned char addressing).
 */
#define ACS_MAX_PEERS 256

/*************************************************************************/
/*  Message classes                                                      */
/*************************************************************************/

#define ACS_CLS_PROPOSAL  0   /* Fig1 reliable broadcast of proposals */
#define ACS_CLS_CONSENSUS 1   /* Fig1 messages for binary consensus */

/*************************************************************************/
/*  Output actions                                                       */
/*                                                                       */
/*  Returned in struct acsAct array from acsInput functions.             */
/*  Caller sends the described messages on the network.                  */
/*************************************************************************/

#define ACS_ACT_PROP_ECHO   1  /* send proposal echo to all for .origin */
#define ACS_ACT_PROP_READY  2  /* send proposal ready to all for .origin */
#define ACS_ACT_CON_SEND    3  /* send consensus msg: .origin .round .conType .conValue */
#define ACS_ACT_BA_DECIDED  4  /* BA for .origin decided .conValue */
#define ACS_ACT_COMPLETE    5  /* all N BAs decided; common subset final */

struct acsAct {
  unsigned char act;          /* ACS_ACT_* */
  unsigned char origin;       /* which origin this relates to */
  unsigned char round;        /* consensus round (ACS_ACT_CON_SEND only) */
  unsigned char conType;      /* BRACHA87_INITIAL/ECHO/READY (CON_SEND only) */
  unsigned char conValue;     /* binary value (CON_SEND, BA_DECIDED only) */
  unsigned char broadcaster;  /* who originated this Fig1 broadcast (CON_SEND) */
};

/*************************************************************************/
/*  ACS state                                                            */
/*************************************************************************/

struct acs {
  unsigned char n;          /* process count encoding: actual = n + 1 */
  unsigned char t;          /* max Byzantine (n + 1 > 3t) */
  unsigned char vLen;       /* proposal value length encoding: actual = vLen + 1 */
  unsigned char maxPhases;  /* per binary consensus instance */
  unsigned char self;       /* this peer's index (needed for consensus routing) */
  unsigned char nAccepted;  /* count of Fig1 proposals accepted */
  unsigned char nDecided;   /* count of BAs that have decided */
  unsigned char threshold;  /* 1 if n-t proposals accepted (vote-0 done) */
  unsigned char complete;   /* 1 if all N BAs decided */
  unsigned char data[1];    /* variable: see acsSz */
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

/* Size in bytes needed for an ACS instance */
unsigned long
acsSz(
  unsigned int             /* n: actual process count = n + 1 */
 ,unsigned int             /* vLen: actual proposal length = vLen + 1 */
 ,unsigned int             /* maxPhases: per binary consensus instance */
);

/* Initialize an ACS instance. Caller has allocated acsSz bytes. */
void
acsInit(
  struct acs *
 ,unsigned char            /* n: actual process count = n + 1 */
 ,unsigned char            /* t */
 ,unsigned char            /* vLen: actual proposal length = vLen + 1 */
 ,unsigned char            /* maxPhases */
 ,unsigned char            /* self: this peer's index */
 ,bracha87CoinFn           /* coin function for binary consensus */
 ,void *                   /* coin closure */
);

/*
 * Maximum output actions from a single input call.
 * Proposal input: 2 (echo/ready) + N (consensus initials on threshold).
 * Consensus input: 2 (echo/ready) + 1 (broadcast) + 2 (decided/complete).
 * Use N + 4 to be safe for both.
 */
#define ACS_MAX_ACTS(n) ((unsigned int)(n) + 1 + 4)

/*
 * Process a proposal broadcast message (ACS_CLS_PROPOSAL).
 *
 * These are Fig1 messages carrying proposal values.
 * Returns number of actions written to out[].
 * Caller provides out[] with room for ACS_MAX_ACTS(n) entries.
 *
 * On ACS_ACT_PROP_ECHO / ACS_ACT_PROP_READY:
 *   Caller sends the proposal echo/ready to all peers.
 *   Value to send: acsProposalValue(acs, origin).
 *
 * On ACS_ACT_CON_SEND:
 *   Caller sends a consensus message to all peers.
 *   Fields: .origin, .round, .conType, .conValue.
 */
unsigned int
acsProposalInput(
  struct acs *
 ,unsigned char            /* origin: whose proposal */
 ,unsigned char            /* type: BRACHA87_INITIAL/ECHO/READY */
 ,unsigned char            /* from: sender of this message */
 ,const unsigned char *    /* value: vLen + 1 bytes */
 ,struct acsAct *          /* out: actions, room for ACS_MAX_ACTS */
);

/*
 * Process a consensus message (ACS_CLS_CONSENSUS).
 *
 * These are Fig1 messages for the binary consensus on origin's inclusion.
 * Returns number of actions written to out[].
 * Caller provides out[] with room for ACS_MAX_ACTS(n) entries.
 *
 * The consensus for each origin is a full Fig1+Fig3+Fig4 pipeline
 * (same structure as consensus.c), deciding 0 or 1.
 *
 * On ACS_ACT_CON_SEND:
 *   Caller sends a consensus message to all peers.
 *
 * On ACS_ACT_BA_DECIDED:
 *   BA for .origin decided .conValue (0=exclude, 1=include).
 *
 * On ACS_ACT_COMPLETE:
 *   All N BAs decided. Common subset is final.
 *   Query with acsSubset().
 */
unsigned int
acsConsensusInput(
  struct acs *
 ,unsigned char            /* origin: which origin's consensus */
 ,unsigned char            /* round: consensus round (0-based) */
 ,unsigned char            /* broadcaster: who initiated this Fig1 broadcast */
 ,unsigned char            /* type: BRACHA87_INITIAL/ECHO/READY */
 ,unsigned char            /* from: sender of this message */
 ,unsigned char            /* value: binary consensus value */
 ,struct acsAct *          /* out: actions, room for ACS_MAX_ACTS */
);

/*
 * Query: is the common subset decided?
 */
int
acsComplete(
  const struct acs *
);

/*
 * Query: get the decided common subset.
 * Returns count of included origins.
 * Fills origins[] with the included origin indices (caller provides n entries).
 * Only valid after acsComplete() returns true.
 */
unsigned int
acsSubset(
  const struct acs *
 ,unsigned char *          /* origins out, n entries */
);

/*
 * Query: get the accepted proposal value for an origin.
 * Returns pointer to the vLen + 1 byte value, or 0 if not yet accepted.
 */
const unsigned char *
acsProposalValue(
  const struct acs *
 ,unsigned char            /* origin */
);

#endif /* _ACS_H */
