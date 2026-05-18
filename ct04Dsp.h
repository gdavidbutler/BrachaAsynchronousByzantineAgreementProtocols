/*
 * asynchronousByzantineAgreementProtocols - CT04 Disperse primitive
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
 * Cachin and Tessaro 2004
 * "Asynchronous Verifiable Information Dispersal"
 * IEEE / IFIP DSN 2005
 *
 * Disperse protocol (Figure 1).  Plugin-abstract: AVID (§3.3),
 * AVID-H (§3.5), and the inner Disperse of AVID-RBC (§3.7) share
 * this wire-rule structure and differ only in the per-block
 * validity predicate (a thrDsp plugin choice) and in what the
 * outer wrap does post-delivery.
 *
 * Pure state machine: no I/O, no threads, no dynamic allocation,
 * no callbacks for delivery.  Caller provides memory, supplies a
 * thrDsp plugin instance, delivers wire messages, executes
 * output actions, runs Pump on its application tick.
 *
 * BPR (Bracha Phase Re-emitter) is intrinsic: replay state lives
 * in the F_ORIGIN / F_ECHOED / F_RDSENT bits the dispatch itself
 * sets.  Per-destination SEND replay uses struct bracha87Pump's
 * sendDest cursor.
 *
 * Operational limits:
 *   n:         unsigned char, encodes process count 1..256 (n + 1)
 *   t:         unsigned char, max 85 (n + 1 > 3t required)
 *   self:      unsigned char, this server's piece index in [0, n)
 *   payloadLen: unsigned int, |F| in bytes
 */

#ifndef CT04DSP_H
#define CT04DSP_H

#include "thrDsp.h"
#include "bracha87.h"            /* for struct bracha87Pump */

/* Figure 1 wire-message kinds (input). */
#define CT04_DSP_SEND  0
#define CT04_DSP_ECHO  1
#define CT04_DSP_READY 2

/* Output actions (see struct ct04DspAct).  Numeric values are
 * stable application contract; do not renumber. */
#define CT04_DSP_ACT_SEND   1  /* per-destination dealer broadcast */
#define CT04_DSP_ACT_ECHO   2  /* echo to all peers */
#define CT04_DSP_ACT_READY  3  /* ready to all peers */
#define CT04_DSP_ACT_STORED 4  /* local: instance delivered */
#define CT04_DSP_ACT_ABORT  5  /* local: dealer rejected */

/* State flags (bitmap). */
#define CT04_DSP_F_ORIGIN     0x01  /* this server is the dealer */
#define CT04_DSP_F_ECHOED     0x02  /* R1 fired (sent echo) */
#define CT04_DSP_F_RDSENT     0x04  /* R2a/R3a fired (sent ready) */
#define CT04_DSP_F_TERMINATED 0x08  /* stored or abort fired */

/*
 * Maximum actions written to out[] per ct04DspInput call.
 *
 * On a single arrival the dispatch can fire at most one of
 * (send-echo) and at most one of (send-ready) and at most one of
 * (stored).  At t >= 1 the (ready, send-ready) and (ready, stored)
 * gates are mutually exclusive by counter values, so stored and
 * send-ready do not co-occur on one call.  abort and send-ready
 * are mutually exclusive (same decode outcome).  Three slots
 * cover every reachable combination with margin.
 *
 * Per ct04DspBpr / ct04DspPumpStep call the cap is also 3:
 * one per-destination SEND replay + one ECHO replay + one READY
 * replay.
 */
#define CT04_DSP_MAX_ACTS       3
#define CT04_DSP_PUMP_MAX_ACTS  3

/*
 * One action emitted by the library.
 *
 * Field usage by act:
 *   ACT_SEND   .dest, .pieceIdx (= .dest), .root, .proof, .piece
 *   ACT_ECHO   .pieceIdx (= self),         .root, .proof, .piece
 *   ACT_READY  .pieceIdx (= self),         .root, .proof, .piece
 *   ACT_STORED .pieceIdx (= self),         .root, .proof, .piece
 *              (.piece = the delivered F-bar_self)
 *   ACT_ABORT  (no fields)
 *
 * .root, .proof, .piece are borrowed pointers into instance
 * storage; valid until the next call that mutates the instance.
 * Caller copies if persistence is required past that boundary.
 *
 * .root, .proof, .piece sizes are reported by the plugin:
 *   rootSz  = plugin->vt->rootSz(plugin)
 *   proofSz = plugin->vt->proofSz(plugin, n_actual)
 *   pieceSz = plugin->vt->pieceSz(plugin, payloadLen, n_actual, t)
 */
struct ct04DspAct {
  const unsigned char *root;
  const unsigned char *proof;
  const unsigned char *piece;
  unsigned char act;           /* CT04_DSP_ACT_* */
  unsigned char dest;          /* ACT_SEND: destination peer index */
  unsigned char pieceIdx;      /* piece index this action carries */
};

/*
 * Instance state.  Caller allocates ct04DspSz(plugin, n, t,
 * payloadLen) bytes and calls ct04DspInit before use.
 *
 * Variable tail layout (see ct04Dsp.c for offsets):
 *   adBmp     [BIT_SZ(n)]            A_D membership bitmap (echo+ready dedup)
 *   sndBmp    [BIT_SZ(n)]            per-tag SEND-from-dealer dedup
 *   ecFrom    [BIT_SZ(n)]            per-sender first-time ECHO dedup
 *   rdFrom    [BIT_SZ(n)]            per-sender first-time READY dedup
 *   pieceSelf [pieceSz]              committed piece for this server
 *                                    (F_i after R1; F-bar_i after R2a/R3a)
 *   proofSelf [proofSz]              committed proof for this server
 *   rootCmt   [rootSz]               committed root commitment D / h_r
 *   adPieces  [n * pieceSz]          A_D piece store, one slot per sender
 *   pendingPc [n * pieceSz]          dealer-only: all n SEND payloads
 *   pendingPr [n * proofSz]          dealer-only: all n SEND proofs
 *
 * For non-dealer instances the pendingPc / pendingPr regions are
 * allocated but unused; ct04DspSz includes them unconditionally
 * to keep the layout uniform.
 */
struct ct04Disperse {
  const struct thrDsp *plugin;       /* caller-owned, outlives this */
  unsigned int payloadLen;           /* |F| in bytes */
  unsigned short eD;                 /* echo count (across distinct senders) */
  unsigned short rD;                 /* ready count (across distinct senders) */
  unsigned char n;                   /* actual = n + 1 */
  unsigned char t;
  unsigned char self;                /* this server's piece index in [0, n) */
  unsigned char flags;               /* CT04_DSP_F_* */
  unsigned char data[1];             /* variable tail */
};

/*
 * Bytes needed for an instance.  Same encoding convention as
 * bracha87: n is the encoded byte (actual = n + 1).
 */
unsigned long
ct04DspSz(
  const struct thrDsp *    /* plugin */
 ,unsigned int             /* n: actual = n + 1 */
 ,unsigned int             /* t */
 ,unsigned int             /* payloadLen */
);

/*
 * Initialise.  Plugin lifetime must >= instance lifetime.
 * self is this peer's piece index in [0, n_actual).
 */
void
ct04DspInit(
  struct ct04Disperse *
 ,const struct thrDsp *    /* plugin */
 ,unsigned char            /* n: actual = n + 1 */
 ,unsigned char            /* t */
 ,unsigned char            /* self */
 ,unsigned int             /* payloadLen */
);

/*
 * Dealer-side origin.  Encodes payload into n pieces + per-piece
 * Merkle proofs + root via the plugin, stashes them in the
 * instance for per-destination SEND replay, and sets F_ORIGIN.
 *
 * encWork: caller-supplied scratch buffer of size
 *          plugin->vt->encWaSz(plugin, n_actual, t, payloadLen)
 *
 * Returns 0 on success, -1 if the plugin's encode fails (invalid
 * (n, t, payloadLen), too few shards, etc.).  Caller broadcasts
 * the first SEND via ct04DspBpr/PumpStep thereafter; this entry
 * point does NOT itself emit actions (the Pump owns SEND replay
 * pacing per the anti-flood discipline).
 *
 * Idempotent on the payload pointer: re-calling overwrites the
 * stashed pieces.  Intended use is a single call per tag.
 */
int
ct04DspOrigin(
  struct ct04Disperse *
 ,const unsigned char *    /* payload, payloadLen bytes */
 ,unsigned char *          /* encWork */
);

/*
 * Process one arriving wire message.
 *
 *   type, from, pieceIdx, root, proof, piece - the arrived
 *     (type, from, pieceIdx, root, proof, piece) message.  pieceIdx
 *     names which piece this message carries: for SEND it is the
 *     intended destination's index (which the dealer chose); for
 *     ECHO / READY it is the broadcaster's piece index (the
 *     broadcaster's identity in the piece-index space, typically
 *     equal to .from but the library doesn't require that mapping).
 *   vfWork: caller scratch, plugin->vt->vfWaSz(plugin) bytes.
 *           Used for the per-arrival verify call.
 *   decWork: caller scratch, plugin->vt->derivedRootWaSz(
 *           plugin, n_actual, t, payloadLen) bytes.  Consulted
 *           only when the threshold gate indicates interpolation
 *           (R2a/b or R3a/b); may be unused on most calls.
 *   out:    caller buffer, room for CT04_DSP_MAX_ACTS entries.
 *
 * Returns the number of actions written to out[].  Borrowed
 * pointers in struct ct04DspAct point into instance state.
 *
 * Inbound message integrity (sender authentication, well-formed
 * framing, root-bytes / proof-bytes / piece-bytes sized per the
 * plugin's reported sizes) is the caller's responsibility.  The
 * library verifies the per-piece Merkle proof via the plugin and
 * rejects unverifiable pieces silently (no action).  Messages
 * carrying a root that disagrees with a previously-committed root
 * are rejected (single dealer per tag; once any peer commits a
 * root for this tag, subsequent arrivals must match bit-for-bit).
 */
unsigned int
ct04DspInput(
  struct ct04Disperse *
 ,unsigned char            /* type: CT04_DSP_SEND / ECHO / READY */
 ,unsigned char            /* from: sender peer index */
 ,unsigned char            /* pieceIdx: piece index in [0, n) */
 ,const unsigned char *    /* root, rootSz bytes */
 ,const unsigned char *    /* proof, proofSz bytes */
 ,const unsigned char *    /* piece, pieceSz bytes */
 ,unsigned char *          /* vfWork */
 ,unsigned char *          /* decWork */
 ,struct ct04DspAct *      /* out: room for CT04_DSP_MAX_ACTS */
);

/*
 * Per-instance BPR pump.  Re-emits broadcast actions this
 * instance has committed to, paced by the cursor.
 *
 *   p: cursor; sendDest field advances one destination per call
 *      while F_ORIGIN.  pos / sweepActs untouched (those are the
 *      array-walking pump's concern).  Init with bracha87PumpInit.
 *
 * Per call: at most one SEND replay (to the destination at
 * p->sendDest, if F_ORIGIN), one ECHO replay (if F_ECHOED), one
 * READY replay (if F_RDSENT) - bounded at CT04_DSP_PUMP_MAX_ACTS.
 *
 * Replay continues post-abort and post-stored: the application's
 * silence-quorum exit retires the instance, not local terminal
 * events.  See README "Anti-flood discipline" and "BPR" sections.
 *
 * Returns 0 when nothing is committed to replay (a non-origin,
 * non-echoed, non-rdsent instance - the natural "idle" signal at
 * the per-instance level).
 */
unsigned int
ct04DspBpr(
  struct ct04Disperse *
 ,struct bracha87Pump *
 ,struct ct04DspAct *      /* out: room for CT04_DSP_PUMP_MAX_ACTS */
);

/*
 * Array-walking variant: BPR sweep over a caller-owned array of
 * ct04Disperse instances.  Analogue of bracha87Fig1PumpStep.
 *
 *   p: shared cursor; pos walks instances, sendDest walks SEND
 *      destinations within the currently-visited dealer instance.
 *      sendDest resets to 0 when pos advances.
 *   outCap: must be >= CT04_DSP_PUMP_MAX_ACTS.
 *
 * Walks the cursor forward to the next committed instance and
 * returns its actions.  Null entries in instances[] are skipped.
 *
 * Call ONCE per application tick.  Do NOT loop -- see the
 * network flood warning in bracha87.h.  Returns 0 only when a
 * full sweep found nothing committed (pre-broadcast or fully-
 * shutdown state, NOT a per-tick termination signal).
 *
 * Termination is the application's responsibility, via silence-
 * quorum + K-sweep.
 */
unsigned int
ct04DspPumpStep(
  struct ct04Disperse *const *  /* instances */
 ,unsigned int                  /* count */
 ,struct bracha87Pump *
 ,struct ct04DspAct *           /* out */
 ,unsigned int                  /* outCap, >= CT04_DSP_PUMP_MAX_ACTS */
);

/*
 * Count of instances with any committed flag (F_ORIGIN, F_ECHOED,
 * or F_RDSENT).  Useful for sweep-cadence calibration in the
 * silence-quorum K-sweep gate.
 */
unsigned int
ct04DspCommittedCount(
  struct ct04Disperse *const *
 ,unsigned int                  /* count */
);

/*
 * Locally-delivered piece F-bar_self once F_RDSENT and ACT_STORED
 * have fired (the §3.5 "Data[ID]" entry for this server).
 * Returns 0 before delivery.  Borrowed pointer.
 */
const unsigned char *
ct04DspStored(
  const struct ct04Disperse *
);

/*
 * Per-piece Merkle proof FP-bar_self for the stored piece.  Returns
 * 0 before delivery.  Borrowed pointer, proofSz bytes.  Used by the
 * Retrieve server side to compose (block, F'_j, FP'_j, D) responses.
 */
const unsigned char *
ct04DspStoredProof(
  const struct ct04Disperse *
);

/*
 * Committed root commitment for this tag (D in AVID, h_r in
 * AVID-H).  Returns 0 before any first valid SEND / ECHO / READY
 * commits a root.  Borrowed pointer, rootSz bytes.
 */
const unsigned char *
ct04DspRoot(
  const struct ct04Disperse *
);

#endif /* CT04DSP_H */
