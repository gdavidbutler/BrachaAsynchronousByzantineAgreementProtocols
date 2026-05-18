/*
 * asynchronousByzantineAgreementProtocols - CT04 Retrieve primitive
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
 * Cachin and Tessaro 2004, "Asynchronous Verifiable Information
 * Dispersal," DSN 2005, Figure 2 - Retrieve protocol.
 *
 * Two rules, paper-direct (see ct04Dsp.dtc inventory document Q1):
 *
 *   R4. Client C upon (retrieve, ID') input action:
 *         send (retrieve, ID') to all servers.
 *   R5. Server P_i upon (retrieve, ID') from C_m:
 *         if Data[ID'] is bound (this server's Disperse instance
 *         for ID' has reached STORED), send
 *         (block, Data[ID'], Verify[ID']) to C_m.
 *
 * Client wait clause (collect):
 *   Receive (block, F'_j, D) messages from distinct servers.  If
 *   verify(j, F'_j, FP'_j, D) (or H(F'_j) = D_j for AVID), record
 *   the (j, F'_j) pair.  When k distinct same-root validated pairs
 *   are collected, interpolate f' of degree <= k - 1 from the set
 *   and output payload F' = [f'(1), ..., f'(k)].
 *
 * No bridge / no dispatch / no BPR: the rules have no test-
 * ordering insight, no joint optimisation with Disperse state,
 * and the client's wait clause is a simple "collect until k" loop.
 * Captured in ct04Rtv.dtc as a paper-completeness artifact (the
 * bracha87Fig2.dtc pattern).
 *
 * Pure state machine: no I/O, no threads, no dynamic allocation.
 * Caller provides memory; client and server roles are separate API
 * surfaces sharing only the plugin contract.
 */

#ifndef CT04RTV_H
#define CT04RTV_H

#include "thrDsp.h"

/* ===================================================================
 * Server side
 * =================================================================== */

/*
 * The server side is stateless given a struct ct04Disperse for the
 * requested ID' (see ct04Dsp.h).  This header exposes no server-side
 * state struct; the wrapper is a one-shot function that consults the
 * caller's existing Disperse instance.
 *
 * On (retrieve, ID') from client C_m:
 *   If Data[ID'] is bound (the Disperse instance for ID' has reached
 *   STORED), the wrapper composes a (block) response from
 *   ct04DspStored / ct04DspRoot / the stashed FP-bar_i in the
 *   instance.
 *
 * Caller does the actual sending; the wrapper returns a borrowed-
 * pointer view of the response bytes.
 */

#define CT04_RTV_ACT_SEND_BLOCK  1   /* Server: send block to client */

struct ct04RtvServerResp {
  const unsigned char *root;
  const unsigned char *proof;
  const unsigned char *piece;
  unsigned char act;       /* 0 = no response (Data[ID'] unbound),
                            * CT04_RTV_ACT_SEND_BLOCK otherwise. */
  unsigned char pieceIdx;
};

/*
 * Compose a server-side response to a (retrieve, ID') request.
 *
 *   d:    the Disperse instance for ID' (must have reached STORED;
 *         the wrapper checks via ct04DspStored).
 *   out:  caller-provided struct; on return, out->act is
 *         CT04_RTV_ACT_SEND_BLOCK and the borrowed pointers are
 *         valid until the next call into d that mutates its state.
 *
 * Returns 0 on Data[ID'] unbound (the paper's R5' no-op), 1 on
 * response composed.  Caller transmits (out->root, out->proof,
 * out->piece) of the plugin-reported sizes.
 *
 * No idempotence concerns: the call is read-only on d.
 */
struct ct04Disperse;  /* forward declaration */

unsigned int
ct04RtvServerRespond(
  const struct ct04Disperse *
 ,struct ct04RtvServerResp *  /* out */
);

/* ===================================================================
 * Client side
 * =================================================================== */

/*
 * Client state for one outstanding Retrieve(ID').  Caller allocates
 * ct04RtvClientSz bytes and calls Init before use.
 *
 * State flags (bitmap):
 */
#define CT04_RTV_F_HAVEROOT   0x01  /* expected root committed */
#define CT04_RTV_F_RETRIEVED  0x02  /* k validated blocks collected */

struct ct04RtvClient {
  const struct thrDsp *plugin;
  unsigned int payloadLen;
  unsigned short collected;     /* count of distinct valid blocks */
  unsigned char n;              /* actual = n + 1 */
  unsigned char t;
  unsigned char flags;
  unsigned char data[1];        /* variable tail; see ct04Rtv.c */
};

/* Output of ct04RtvClientInput. */
#define CT04_RTV_ACT_RETRIEVED  2  /* k blocks collected; payload ready */

struct ct04RtvClientAct {
  const unsigned char *payload;  /* CT04_RTV_ACT_RETRIEVED only */
  unsigned char act;
};

unsigned long
ct04RtvClientSz(
  const struct thrDsp *
 ,unsigned int  /* n: actual = n + 1 */
 ,unsigned int  /* t */
 ,unsigned int  /* payloadLen */
);

void
ct04RtvClientInit(
  struct ct04RtvClient *
 ,const struct thrDsp *
 ,unsigned char  /* n: actual = n + 1 */
 ,unsigned char  /* t */
 ,unsigned int   /* payloadLen */
);

/*
 * Optionally pin the expected root commitment (e.g. when the client
 * learned D / h_r out of band).  Future block arrivals with a
 * different root are rejected.  If never called, the first
 * piece-valid arrival commits its root and subsequent arrivals
 * must match.
 */
void
ct04RtvClientPinRoot(
  struct ct04RtvClient *
 ,const unsigned char *  /* root, rootSz bytes */
);

/*
 * Process one arriving (block, F'_j, FP'_j, D) message from a server.
 *
 *   pieceIdx, root, proof, piece - as transmitted by the server
 *   vfWork: caller scratch, plugin->vt->vfWaSz(plugin) bytes
 *   decWork: caller scratch, plugin->vt->decWaSz(plugin, n, t,
 *            payloadLen) bytes; consulted only when this arrival
 *            completes the k-block threshold
 *   out: caller buffer; on return out->act is 0 (no event yet) or
 *        CT04_RTV_ACT_RETRIEVED with out->payload pointing into
 *        instance state (borrowed, payloadLen bytes, valid until
 *        the next mutating call).
 *
 * Returns 1 on RETRIEVED, 0 otherwise.
 */
unsigned int
ct04RtvClientInput(
  struct ct04RtvClient *
 ,unsigned char  /* from: server peer index */
 ,unsigned char  /* pieceIdx in [0, n) */
 ,const unsigned char *  /* root */
 ,const unsigned char *  /* proof */
 ,const unsigned char *  /* piece */
 ,unsigned char *  /* vfWork */
 ,unsigned char *  /* decWork */
 ,struct ct04RtvClientAct *  /* out */
);

/*
 * Borrowed pointer to the decoded payload after RETRIEVED.  Returns
 * 0 before retrieval completes.
 */
const unsigned char *
ct04RtvClientPayload(
  const struct ct04RtvClient *
);

#endif /* CT04RTV_H */
