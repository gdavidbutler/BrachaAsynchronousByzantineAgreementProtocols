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
 * Four composable pure state machines, one per figure.
 * No I/O, no threads, no dynamic allocation.
 * Caller provides memory and executes output actions.
 *
 * Composition:
 *   message -> Fig1(n,t) -> accept -> Fig2/3(N) -> round complete -> Fig4(coin) -> decision
 *
 * Fig 2 defines the abstract protocol round (send, receive n-t, v := N(k,S)).
 * Fig 3 refines Fig 2: replaces receive with validate (VALID sets).
 *
 * Each module boundary matches the paper exactly.
 * Proofs apply per-module: Lemmas 1-4 to Fig1, Lemmas 5-7 to Fig2/3,
 * Lemma 9-10 and Theorems 1-2 to Fig4.
 *
 * Operational limits:
 *   n:         unsigned char, encodes process count 1..256 (n + 1)
 *   t:         unsigned char, max 85 (n + 1 > 3t required)
 *   vLen:      unsigned char, encodes value length 1..256 (vLen + 1 bytes)
 *   maxPhases: unsigned char, max BRACHA87_MAX_PHASES (85)
 *   rounds:    unsigned char, 0-based, max 3 * BRACHA87_MAX_PHASES - 1 (254)
 */

#ifndef BRACHA87_H
#define BRACHA87_H

/*
 * Maximum phases for Figure 4 consensus.
 * Each phase uses 3 rounds. 85 * 3 = 255 rounds, which is
 * the maximum that fits in an unsigned char round count.
 * Round indices range from 0 to 3 * maxPhases - 1 (max 254).
 * The protocol decides in expected O(1) phases with a random coin;
 * 85 phases is far beyond any practical execution.
 */
#define BRACHA87_MAX_PHASES 85

/*************************************************************************/
/*                                                                       */
/*  Figure 1 — Reliable broadcast primitive                              */
/*                                                                       */
/*  One instance per (sender, broadcast) pair.                           */
/*  Caller maintains the set of instances.                               */
/*  Pure state machine: (state, input) -> (state', actions).             */
/*  n > 3t required.                                                     */
/*                                                                       */
/*  in                      condition                         out        */
/*  -------------------------------------------------------------------- */
/*  in(initial, v) from p  !echoed                            echo  all  */
/*  in(echo,    v) from j  !echoed && ecCnt[v]>(n+t)/2        echo  all  */
/*  in(ready,   v) from j  !echoed && rdCnt[v]>=t+1           echo  all  */
/*  in(echo,    v) from j   echoed && !rdSent                            */
/*                                 && ecCnt[v]>(n+t)/2        ready all  */
/*  in(ready,   v) from j   echoed && !rdSent                            */
/*                                 && rdCnt[v]>=t+1           ready all  */
/*  in(ready,   v) from j   rdSent && rdCnt[v]>=2t+1          accept     */
/*                                                                       */
/*  Paper typo: Fig. 1 says "(n+t)/2 (echo,v) messages" but the          */
/*  Lemma 1 proof says "more than (n+t)/2." The proof requires           */
/*  strict > for the pigeonhole argument. Code follows the proof.        */
/*                                                                       */
/*************************************************************************/

/* Figure 1 message types (input) */
#define BRACHA87_INITIAL 0
#define BRACHA87_ECHO    1
#define BRACHA87_READY   2

/* Figure 1 output actions */
#define BRACHA87_INITIAL_ALL 4  /* send (initial, v) to all peers (BPR) */
#define BRACHA87_ECHO_ALL    1  /* send echo(v) to all peers */
#define BRACHA87_READY_ALL   2  /* send ready(v) to all peers */
#define BRACHA87_ACCEPT      3  /* accept(v) */

/* Figure 1 state flags (bitmap) */
#define BRACHA87_F1_ECHOED   0x01
#define BRACHA87_F1_RDSENT   0x02
#define BRACHA87_F1_ACCEPTED 0x04
#define BRACHA87_F1_ORIGIN   0x08  /* this peer is the broadcast originator */

/*
 * Figure 1 state.
 *
 * Caller allocates bracha87Fig1Sz(n, vLen) bytes and calls
 * bracha87Fig1Init before use. No dynamic allocation.
 *
 * The value v is a fixed-length byte string of vLen + 1 bytes.
 * vLen encodes the value length: 0 = 1 byte, 255 = 256 bytes.
 * Per-peer echo/ready values are stored so echo_count[v]
 * is computed correctly for any v, avoiding the liveness bug
 * where committing to the first value seen blocks the honest one.
 */
struct bracha87Fig1 {
  unsigned short ecCnt[2];/* incremental echo counts for binary (vLen==0) */
  unsigned short rdCnt[2];/* incremental ready counts for binary (vLen==0) */
  unsigned char n;        /* process count encoding: actual = n + 1 */
  unsigned char t;        /* max Byzantine (n + 1 > 3t) */
  unsigned char vLen;     /* value length encoding: actual = vLen + 1 */
  unsigned char flags;    /* BRACHA87_F1_ECHOED/RDSENT/ACCEPTED */
  unsigned char data[1];  /* variable: see bracha87Fig1Sz */
};

/* data[] is the variable tail; see bracha87.c for layout. */

/* Size in bytes needed for a Fig1 instance */
unsigned long
bracha87Fig1Sz(
  unsigned int             /* n: actual process count = n + 1 */
 ,unsigned int             /* vLen: actual value length = vLen + 1 */
);

/* Initialize a Fig1 instance. Caller has allocated bracha87Fig1Sz bytes. */
void
bracha87Fig1Init(
  struct bracha87Fig1 *
 ,unsigned char            /* n: actual process count = n + 1 */
 ,unsigned char            /* t */
 ,unsigned char            /* vLen: actual value length = vLen + 1 */
);

/*
 * Mark this Fig1 instance as the broadcast originator and store
 * the value to be broadcast.  Sets BRACHA87_F1_ORIGIN; copies
 * value into the committed-value slot so bracha87Fig1Value
 * returns it (without setting BRACHA87_F1_ECHOED -- Rule 1 still
 * fires from the receipt of (initial, v) via bracha87Fig1Input).
 *
 * BPR replays BRACHA87_INITIAL_ALL on every bracha87Fig1Bpr
 * call as long as ORIGIN is set, regardless of ECHOED state.
 * Stopping INITIAL replay at "we ECHOED locally" leaves a
 * stall-able boundary case: in the n = 3t + 1 regime, the
 * (n+t)/2+1 echo threshold equals the count of honest peers,
 * so any honest peer that missed the bootstrap INITIAL can
 * leave the cascade one echo short forever.  Symmetric with
 * the "READY replay continues post-accept" rule (gap 3 above)
 * -- both refuse local-state-as-saturation arguments because
 * BPR's purpose is helping OTHER peers, not the local one.
 *
 * Caller emits BRACHA87_INITIAL_ALL once at origin time and
 * relies on BPR thereafter.
 *
 * Idempotent: re-calling overwrites the stored value.  The
 * intended use is one call at proposal time.
 */
void
bracha87Fig1Origin(
  struct bracha87Fig1 *
 ,const unsigned char *    /* value: vLen + 1 bytes */
);

/*
 * Process one incoming message. Returns number of actions (0..3).
 * Actions written to out[] in order (echo, ready, accept).
 * Caller provides out[] with room for 3 entries.
 *
 * On any action, the committed value is available via bracha87Fig1Value.
 *
 * Deduplication: at most one echo and one ready per sender.
 */
unsigned int
bracha87Fig1Input(
  struct bracha87Fig1 *
 ,unsigned char            /* type: BRACHA87_INITIAL/ECHO/READY */
 ,unsigned char            /* from: sender index */
 ,const unsigned char *    /* value: vLen + 1 bytes */
 ,unsigned char *          /* out: actions, room for 3 */
);

/*
 * Committed value.
 *
 * Returns non-null when either:
 *   - this instance is the originator (BRACHA87_F1_ORIGIN set via
 *     bracha87Fig1Origin), or
 *   - Rule 1, 2, or 3 has fired (BRACHA87_F1_ECHOED set).
 *
 * Originator-only state returns the value supplied at
 * bracha87Fig1Origin so BPR can re-broadcast it before any
 * loopback or echo-cascade has set ECHOED.
 */
const unsigned char *
bracha87Fig1Value(
  const struct bracha87Fig1 *
);

/*
 * BPR - Bracha Phase Re-emitter.
 *
 * End-to-end argument applied to Bracha (Saltzer/Reed/Clark
 * 1984; see SRC84.txt and the BPR section of README.md): the
 * "still owed" predicate lives at the Bracha endpoint, so
 * retransmission is placed here.
 *
 * Re-emit the broadcast actions this instance has committed to
 * (initial if origin and not yet echoed, echo if echoed, ready
 * if rdSent) so eventual delivery holds under fair-loss without
 * an application-layer ledger.  Returns the number of actions
 * (0..3) for the caller to broadcast.
 *
 * Reactive: rules fire only when called.  No wall-clock predicate
 * appears anywhere; the application's pump tick IS the event, so
 * asynchrony is preserved.  The rate of pump calls bounds replay
 * volume.
 *
 * Does NOT short-circuit on accepted: an honest peer that has
 * accepted owes its (ready, v) to peers still below the 2t+1
 * threshold, and replay is the only mechanism Bracha provides
 * for getting it there under loss.  The application's silence-
 * quorum exit is what eventually retires the instance.
 *
 * Returns 0 only when there is nothing committed to replay
 * (a non-origin instance that has not echoed -- the natural
 * "idle" signal at the Fig1 level).
 *
 * Out actions reuse BRACHA87_INITIAL_ALL / BRACHA87_ECHO_ALL /
 * BRACHA87_READY_ALL; the committed value is read via
 * bracha87Fig1Value, same as after Input.  Order of actions in
 * out[]: initial, echo, ready.
 */
unsigned int
bracha87Fig1Bpr(
  struct bracha87Fig1 *
 ,unsigned char *          /* out: actions, room for 3 */
);

/*************************************************************************/
/*                                                                       */
/*  Figure 2 — Abstract protocol round                                   */
/*                                                                       */
/*  The generic form of any asynchronous protocol round.                 */
/*  Figure 3 refines this by replacing receive with validate.            */
/*  Rounds are 0-based.                                                  */
/*                                                                       */
/*  round(k) by process p                                                */
/*    Send (p, k, v) to all processes                                    */
/*    Wait until a set S of n - t k-messages received                    */
/*    v := N(k, S)                                                       */
/*                                                                       */
/*************************************************************************/

/* Figure 2 output action */
#define BRACHA87_ROUND_COMPLETE 2  /* n-t messages received for round k */

/*
 * Figure 2 state.
 *
 * Tracks received messages per round for up to n peers, maxRounds rounds.
 * Caller allocates bracha87Fig2Sz(n, maxRounds) bytes and calls
 * bracha87Fig2Init. No dynamic allocation.
 *
 * Pure accumulation: any received message counts toward n-t.
 * For validated accumulation, use Figure 3 which adds VALID checking.
 */
struct bracha87Fig2 {
  unsigned char n;        /* process count encoding: actual = n + 1 */
  unsigned char t;        /* max Byzantine (n + 1 > 3t) */
  unsigned char maxRounds;
  unsigned char data[1];  /* variable: see bracha87Fig2Sz */
};

/* data[] is the variable tail; see bracha87.c for layout. */

/* Size in bytes needed for a Fig2 instance */
unsigned long
bracha87Fig2Sz(
  unsigned int             /* n: actual process count = n + 1 */
 ,unsigned int             /* maxRounds */
);

/* Initialize a Fig2 instance. Caller has allocated bracha87Fig2Sz bytes. */
void
bracha87Fig2Init(
  struct bracha87Fig2 *
 ,unsigned char            /* n: actual process count = n + 1 */
 ,unsigned char            /* t */
 ,unsigned char            /* maxRounds */
);

/*
 * Record a received message for round k.
 * Returns BRACHA87_ROUND_COMPLETE if this causes n-t received, 0 otherwise.
 * Deduplication: one message per sender per round.
 */
unsigned int
bracha87Fig2Receive(
  struct bracha87Fig2 *
 ,unsigned char            /* round k (0-based) */
 ,unsigned char            /* sender */
 ,unsigned char            /* value */
);

/* Query received count for round k */
unsigned int
bracha87Fig2RecvCount(
  const struct bracha87Fig2 *
 ,unsigned char            /* round k (0-based) */
);

/*
 * Retrieve received messages for round k.
 * Returns count, fills senders[] and values[] (caller-provided, n entries).
 */
unsigned int
bracha87Fig2GetReceived(
  const struct bracha87Fig2 *
 ,unsigned char            /* round k (0-based) */
 ,unsigned char *          /* senders out, n entries */
 ,unsigned char *          /* values out, n entries */
);

/*************************************************************************/
/*                                                                       */
/*  Figure 3 — Correctness enforcement (VALID sets)                      */
/*                                                                       */
/*  Refines Figure 2: replaces receive with validate (VALID sets).       */
/*  Wraps Figure 1 Accept with a recursive conformance check.            */
/*  Parameterized by N: the protocol function.                           */
/*  Rounds are 0-based.                                                  */
/*                                                                       */
/*  When n-t messages validate for round k (Fig 2 threshold), stored     */
/*  messages from round k+1 are re-evaluated, cascading as needed.       */
/*                                                                       */
/*  round(k) by process p                                                */
/*    Broadcast(p, k, v)                                                 */
/*    Wait till n-t k-messages validated                                 */
/*    v := N(k, S)                                                       */
/*                                                                       */
/*  VALID^0_p = {(q,0,v) | accepted, v in {0,1}}                         */
/*  VALID^k_p = {(q,k,v) | accepted, exists n-t in VALID^{k-1}           */
/*               s.t. v = N(k-1, {m1..m_{n-t}})}                         */
/*                                                                       */
/*************************************************************************/

/*
 * Protocol function N.
 * Given round k and a set of n-t validated messages from round k,
 * compute the output value.
 *
 *   closure: caller context
 *   k: round number (0-based)
 *   n_msgs: number of messages (at least n-t, may exceed n-t)
 *   senders: n_msgs sender IDs
 *   values: n_msgs values, each 1 byte (binary consensus)
 *   result: output value written here (1 byte)
 *
 * Returns 0: result is set, exact match required for VALID.
 * Returns >0: any binary value is valid (non-deterministic path).
 *   When n_msgs > n-t, N should return >0 if different n-t subsets
 *   could produce different results (paper's existential quantifier).
 * Returns <0: error, message is invalid.
 */
typedef int (*bracha87Nfn)(
  void *                   /* closure */
 ,unsigned char            /* k */
 ,unsigned int             /* n_msgs */
 ,const unsigned char *    /* senders */
 ,const unsigned char *    /* values */
 ,unsigned char *          /* result */
);

/* Figure 3 output action */
#define BRACHA87_VALIDATED 1  /* message is in VALID^k */

/*
 * Figure 3 state.
 *
 * Manages VALID sets for up to maxRounds rounds, n peers.
 * Caller allocates bracha87Fig3Sz(n, maxRounds) bytes and
 * calls bracha87Fig3Init.
 */
struct bracha87Fig3 {
  bracha87Nfn N;
  void *Nclosure;
  unsigned char n;
  unsigned char t;
  unsigned char maxRounds;
  unsigned char data[1];   /* variable: see bracha87Fig3Sz */
};

/* data[] is the variable tail; see bracha87.c for layout. */

unsigned long
bracha87Fig3Sz(
  unsigned int             /* n: actual process count = n + 1 */
 ,unsigned int             /* maxRounds */
);

void
bracha87Fig3Init(
  struct bracha87Fig3 *
 ,unsigned char            /* n: actual process count = n + 1 */
 ,unsigned char            /* t */
 ,unsigned char            /* maxRounds */
 ,bracha87Nfn              /* N */
 ,void *                   /* Nclosure */
);

/*
 * Submit an accepted message (from Fig1) for validation.
 *
 * Returns:
 *   BRACHA87_VALIDATED if message is in VALID^k
 *   0 if not valid or round out of range
 *
 * validCount receives the number of validated messages for round k
 * (so caller can check for n-t completion).
 */
unsigned int
bracha87Fig3Accept(
  struct bracha87Fig3 *
 ,unsigned char            /* round k (0-based) */
 ,unsigned char            /* sender */
 ,unsigned char            /* value */
 ,unsigned int *           /* validCount out, 0 to skip */
);

/*
 * Query VALID^k count for a specific round.
 */
unsigned int
bracha87Fig3ValidCount(
  const struct bracha87Fig3 *
 ,unsigned char            /* round k (0-based) */
);

/*
 * Retrieve validated messages for round k.
 * Returns count, fills senders[] and values[] (caller-provided, n entries).
 */
unsigned int
bracha87Fig3GetValid(
  const struct bracha87Fig3 *
 ,unsigned char            /* round k (0-based) */
 ,unsigned char *          /* senders out, n entries */
 ,unsigned char *          /* values out, n entries */
);

/*
 * Check if round k has reached n-t validated (Fig 2 round completion).
 * Includes rounds completed by cascaded re-evaluation.
 */
int
bracha87Fig3RoundComplete(
  const struct bracha87Fig3 *
 ,unsigned char            /* round k (0-based) */
);

/*************************************************************************/
/*                                                                       */
/*  Figure 4 — Consensus protocol                                        */
/*                                                                       */
/*  Instantiates Figure 3 with three specific N functions.               */
/*  Three rounds per phase. Parameterized by coin.                       */
/*                                                                       */
/*  maxPhases <= BRACHA87_MAX_PHASES (85).                               */
/*  85 * 3 = 255 rounds fits in unsigned char round count (0..254).      */
/*  The protocol decides in expected O(1) phases with a random coin.     */
/*  If all phases are exhausted without decision, Fig4Round returns      */
/*  BRACHA87_EXHAUSTED.                                                  */
/*                                                                       */
/*  Phase(i) by process p:                                               */
/*                                                                       */
/*  1. Broadcast(p, 3i, value_p).                                        */
/*     Wait n-t validated. value_p := majority.                          */
/*                                                                       */
/*  2. Broadcast(p, 3i+1, value_p).                                      */
/*     If >n/2 same v: value_p := (d,v). Else unchanged.                 */
/*                                                                       */
/*  3. Broadcast(p, 3i+2, value_p).                                      */
/*     If >2t (d,v): decide v.                                           */
/*     Else if >t (d,v): value_p := v.                                   */
/*     Else: value_p := coin.                                            */
/*                                                                       */
/*************************************************************************/

/* Coin function: return 0 or 1 for given phase */
typedef unsigned char (*bracha87CoinFn)(
  void *                   /* closure */
 ,unsigned char            /* phase */
);

/* Figure 4 output actions (bitmask) */
#define BRACHA87_BROADCAST 1  /* broadcast value_p for current round */
#define BRACHA87_DECIDE    2  /* decided: value is final */
#define BRACHA87_EXHAUSTED 4  /* all phases exhausted without decision */

/*
 * Figure 4 decision-candidate flag.
 * Paper's "(d, v)" encoding: high bit marks that
 * the sender saw >n/2 agreement in step 2.
 * value & 1 = the binary value, value & D_FLAG = decision candidate.
 * Present only in round 3i+2 broadcasts.
 */
#define BRACHA87_D_FLAG    0x80

/*
 * Figure 4 state.
 *
 * Caller allocates bracha87Fig4Sz(n, maxPhases) bytes and calls
 * bracha87Fig4Init.  Embeds a Fig3 instance as the trailing fig3
 * field; its variable tail extends past sizeof (struct bracha87Fig4)
 * into the bytes Sz() reserves for it.  Caller reads the embedded
 * Fig3 directly as &fig4->fig3 — no cast.
 *
 * maxPhases must be >= 1 and <= BRACHA87_MAX_PHASES (85).
 * Fig 4 instantiates Fig 3 with maxRounds = maxPhases * 3.
 */
struct bracha87Fig4 {
  bracha87CoinFn coin;
  void *coinClosure;
  unsigned char n;        /* process count encoding: actual = n + 1 */
  unsigned char t;        /* max Byzantine (n + 1 > 3t) */
  unsigned char maxPhases;
  unsigned char phase;     /* current phase (0-based) */
  unsigned char subRound;  /* 0, 1, or 2 within phase */
  unsigned char value;     /* current estimate */
  unsigned char decided;
  unsigned char decision;
  struct bracha87Fig3 fig3;/* embedded Fig 3; variable tail extends past */
};

unsigned long
bracha87Fig4Sz(
  unsigned int             /* n: actual process count = n + 1 */
 ,unsigned int             /* maxPhases, <= BRACHA87_MAX_PHASES (85) */
);

void
bracha87Fig4Init(
  struct bracha87Fig4 *
 ,unsigned char            /* n: actual process count = n + 1 */
 ,unsigned char            /* t */
 ,unsigned char            /* maxPhases, <= BRACHA87_MAX_PHASES (85) */
 ,unsigned char            /* initialValue: 0 or 1 */
 ,bracha87CoinFn           /* coin */
 ,void *                   /* coinClosure */
);

/*
 * Process a validated round message (from Fig3).
 * Returns a bitmask of actions: 0, BRACHA87_BROADCAST,
 * BRACHA87_DECIDE | BRACHA87_BROADCAST, or BRACHA87_EXHAUSTED.
 *
 * On BRACHA87_BROADCAST: caller reads fig4->value for the broadcast value
 *   and fig4->phase/fig4->subRound for the round number.
 * On BRACHA87_DECIDE: caller reads fig4->decision.
 * On BRACHA87_EXHAUSTED: all phases consumed without decision; terminal.
 *
 * BRACHA87_DECIDE is returned exactly once (the first time >2t d-messages
 * are seen), combined with BRACHA87_BROADCAST. Per the paper, a decided
 * process continues participating so others can reach consensus.
 *
 * The round k maps to phase/subRound:
 *   phase = k / 3
 *   subRound = k % 3
 *
 * This function is called when Fig3 reports n-t validated messages
 * for round k. The caller passes the validated set.
 */
unsigned int
bracha87Fig4Round(
  struct bracha87Fig4 *
 ,unsigned char            /* round k (0-based) */
 ,unsigned int             /* n_msgs */
 ,const unsigned char *    /* senders */
 ,const unsigned char *    /* values */
);

#endif /* BRACHA87_H */
