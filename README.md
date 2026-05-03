# asynchronousByzantineAgreementProtocols

A C library implementing all four figures of Gabriel Bracha's 1987 paper as composable pure state machines, plus the BKR94 Asynchronous Common Subset (ACS) protocol built from them.

## Overview

This is the only known implementation of all four figures of Bracha 1987 as composable pure state machines. ANSI C89, zero dependencies. No I/O, no threads, no dynamic allocation -- the caller provides memory and executes output actions.

Each module boundary matches the paper exactly, so the paper's proofs apply per-module: Lemmas 1-4 to Fig 1, Lemmas 5-7 to Fig 2/3, Lemmas 9-10 and Theorems 1-2 to Fig 4.

The `bkr94acs` module composes these figures into multi-value agreement: N peers propose arbitrary values, and all honest peers agree on the same common subset of at least n-t proposals. This is Ben-Or/Kelmer/Rabin 1994 Section 4 Figure 3 (Protocol Agreement[Q]).

## The Papers

Gabriel Bracha, "Asynchronous Byzantine Agreement Protocols," *Information and Computation* 75, 130-143 (1987). Implemented in `bracha87.[hc]`.

Michael Ben-Or, Boaz Kelmer, Tal Rabin, "Asynchronous Secure Computations with Optimal Resilience (Extended Abstract)," PODC '94, pages 183-192. Section 4 Figure 3 (Protocol Agreement[Q]) is implemented in `bkr94acs.[hc]`.

`Bracha87.txt` is a companion summary of the Bracha 1987 paper: figures, rules, VALID set definitions, all lemma/theorem statements, and a mapping from each lemma to the test that verifies it.

`BKR94ACS.txt` is the line-by-line extract of BKR94 Section 4 used as `bkr94acs.[hc]`'s reference.

**Paper typo:** Fig. 1 says "(n+t)/2 (echo,v) messages" but the Lemma 1 proof says "more than (n+t)/2." The proof requires strict `>` for the pigeonhole argument to work. The code follows the proof, not the figure.

## System Model — What the Caller Must Provide

The paper's proofs depend on three assumptions about the communication system (Section 2):

> "We assume a reliable message system in which no messages are lost or generated. Each process can directly send messages to any other process, and can identify the sender of every message it receives."

These assumptions are not optional — they are load-bearing requirements of every lemma and theorem in the paper. This library is a pure state machine with no I/O. **The caller is responsible for building a transport layer that satisfies them:**

1. **Eventual delivery under fair-loss.** Every message sent between correct processes must eventually arrive — but messages may be silently dropped any finite number of times in transit. The Bracha Phase Re-emitter (BPR, see below) closes the gap from "fair-loss point-to-point" to "reliable delivery" intrinsically, so the transport need not provide retransmission of its own; the caller's pump tick is what drives BPR's replays. Per-message reliability is no longer the transport's job.

2. **No message fabrication.** The transport must not generate messages that were never sent. A Byzantine process may send arbitrary content, but the transport itself must not invent messages. In practice this means authenticated channels.

3. **Sender identification.** The receiver must know which process sent each message, and a Byzantine process must not be able to impersonate a correct one. In practice this means authentication bound to process identity.

The protocol's correctness (both safety and termination) does not depend on any timing assumption — that is the asynchronous-BFT model. Pump cadence, silence-quorum exit windows, and any other timing parameters in a deployment are operator-tuning, not protocol invariants.

## Paper-Faithful Dispatch via DTC

Each module's per-call decision logic is captured in a CSV decision table written in the paper's vocabulary (`bracha87Fig{1,3,4}.dtc`, `bkr94acs.dtc`). A small bridge per module (`*ToC.dtc`) maps domain names and values to C identifiers and constants. The decisionTableCompiler (`../decisionTableCompiler/dtc`) co-compiles each pair to an optimal-depth pseudocode dispatch, which a local `psu.awk` translates to a C snippet the entry-point function `#include`s.

| Source | Bridge | Generated snippet | Entry point | Depth |
|--------|--------|-------------------|-------------|-------|
| `bracha87Fig1.dtc` | `bracha87Fig1ToC.dtc` | `bracha87Fig1.c` | `bracha87Fig1Input` | 7 |
| `bracha87Fig2.dtc` | (none — Fig 3 subsumes) | — | — | — |
| `bracha87Fig3.dtc` | `bracha87Fig3ToC.dtc` | `bracha87Fig3.c` | `bracha87Fig3Accept` | 4 |
| `bracha87Fig4.dtc` | `bracha87Fig4ToC.dtc` | `bracha87Fig4.c` | `bracha87Fig4Round` | 6 |
| `bkr94acs.dtc` | `bkr94acsToC.dtc` | `bkr94acsRules.c` | both bkr94acs entry points (one snippet, two `#include`s) | 7 |

dtc enforces exhaustiveness and exclusivity of the rules at compile time. Depths are full-optimum (the `-q` heuristic flag is not used; full search confirms each is depth-minimal for its boundary-input set). The C wrapper computes boundary inputs, `#include`s the dispatch, and applies the boolean outputs as side effects in an order that is the API contract (e.g. for Fig 1: `echo` before `ready` before `accept`). See `decisionTableCompiler/README.md` for the bridge mechanism.

**Audit chain:**

```
paper rules            <-> .dtc files                human, rule-by-rule comments
.dtc files              -> compiled dispatch         dtc, exhaustive/exclusive
C wrapper boundary I/O                               human inspection
fig3IsValid, fig4Nfn, Fig 3 cascade                  test/test_predicates.c —
                                                     exhaustive enumeration vs
                                                     paper-direct reference at
                                                     n=4, t=1
```

`fig3IsValid` is paper-correct **given a caller's N that exposes the existential subset quantifier via `rc > 0`**. The Fig 3 dispatch invokes N once on the full validated set; N's responsibility is to answer "could some n-t subset legitimately produce this value?" If a caller supplies an N whose permissive return is suppressed, `fig3IsValid` correctly rejects values the paper definition would admit via a strict subset. `fig4Nfn` is the canonical N for Fig 4 and exposes the existential analytically; its correspondence test (all 960 bounded inputs across the three sub-rounds) anchors that delegation.

## Architecture

### Binary Consensus Pipeline (bracha87)

```
message -> Fig1(n,t) -> accept -> Fig3(N) -> round complete -> Fig4(coin) -> decision
```

### BKR94 Asynchronous Common Subset (bkr94acs)

```
N proposals -> N Fig1(n,t,vLen) -> accept -> vote 1 in BA
                                   n-t BAs decided 1 -> vote 0 in remaining BAs
                                   N BA instances -> Fig1+Fig3+Fig4 each -> common subset
```

### Figure 1 -- Reliable Broadcast Primitive

One instance per (sender, round) pair. Implements the six rules from the paper's table: incoming initial/echo/ready messages trigger echo, ready, and accept actions based on counting thresholds. Per-peer echo/ready values are stored (not just counts) so `echo_count[v]` is computed correctly for any v. Requires n > 3t.

### Figure 2 -- Abstract Protocol Round

The generic form of any asynchronous protocol round: send, receive n-t messages, apply N(k, S). Exists for completeness -- Figure 3 subsumes it in practice.

### Figure 3 -- VALID Sets with Recursive Conformance Checking

Wraps Fig 1 Accept with a recursive conformance check. Parameterized by N, the protocol function. Messages are validated against VALID sets: round 0 accepts any binary value; round k requires that the sender's value is consistent with some n-t subset of round k-1 validated messages under N. When round k reaches n-t validated, stored messages from round k+1 onward are re-evaluated, cascading as needed.

N receives all validated messages (not just the first n-t) and returns whether different n-t subsets could produce different results, implementing the paper's existential quantifier correctly.

### Figure 4 -- Consensus Protocol

Three rounds per phase. Embeds a Fig 3 instance internally. Parameterized by a coin function.

- Step 1: Broadcast value, wait for n-t validated, take majority.
- Step 2: Broadcast value. If >n/2 agree on v, mark as decision candidate (d,v).
- Step 3: Broadcast value. If >2t decision candidates for v, decide v. Else if >t, adopt v. Else take coin.

Decided processes continue participating so others can reach consensus. The protocol decides in expected O(1) phases with a random coin.

### bkr94acs -- BKR94 Asynchronous Common Subset

Composes Bracha87 Fig 1 and Fig 4 into multi-value agreement, implementing Ben-Or/Kelmer/Rabin 1994 Section 4 Figure 3 (Protocol Agreement[Q]). See `BKR94ACS.txt` for the line-by-line extract used as the implementation's reference.

Each of N peers proposes an arbitrary value (up to vLen+1 bytes). N Bracha87 Fig 1 instances reliably broadcast the proposals. N Bracha87 Fig 4 instances run binary consensus on "include this origin?" When a peer accepts origin j's proposal via Fig 1, it votes 1 in BA_j. When n-t BAs have *decided 1*, it votes 0 for every BA in which it has not yet voted. The common subset is {j : BA_j decided 1}, guaranteed to contain at least n-t origins.

The step-2 trigger is "n-t BAs decided with output 1," not "n-t Fig 1 ACCEPTs." The two coincide in benign runs but diverge under asynchrony or Byzantine scheduling, and only the decide-1 trigger satisfies Part A case (i) of the BKR94 Lemma 2 proof.

Two message classes flow on the network: proposal messages (Fig 1 carrying arbitrary values) and consensus messages (Fig 1 carrying binary values for per-origin BA instances). Consensus messages are routed internally by (origin, round, broadcaster) -- the broadcaster identifies whose Fig 1 broadcast within a consensus round, distinct from the message sender.

The bkr94acs state machine knows its own peer index (self), which the bracha87 figures do not need. This is because bkr94acs manages internal routing: when Fig 4 says BROADCAST, bkr94acs must tag the outgoing INITIAL with self as the broadcaster.

## Bracha Phase Re-emitter (BPR)

Bracha's correctness proofs presume reliable point-to-point channels between correct processes. Over fair-loss datagrams that assumption is not satisfied. **BPR is the smallest rule set that, joined with the paper rules, restores eventual delivery — replacing the application-layer ledger entirely.**

### Why BPR exists

BPR is the end-to-end argument (Saltzer, Reed, Clark 1984 — see `SRC84.txt`) applied to Bracha. The reliability function — "deliver INITIAL/ECHO/READY despite a fair-loss network" — depends on knowledge that lives only at the Bracha state-machine endpoint (the `F1_ORIGIN`/`F1_ECHOED`/`F1_RDSENT` flags and the BKR per-origin BA-decided state). A lower link layer cannot decide when to retire without being told by Bracha — i.e., without folding the function back into the endpoint anyway — so the principled placement is at the endpoint itself.

Without BPR, an application using this library would need its own ledger to track per-record destinations, per-peer evidence of receipt, and a cursor-driven retransmit pump — the standard "deployment-layer reliability" pattern. Three properties make BPR a better fit:

1. **All replay state is already in the protocol.** The committed-action flags (`F1_ECHOED`, `F1_RDSENT`) plus a one-bit `F1_ORIGIN` flag (set by `bracha87Fig1Origin`) encode everything needed to decide which actions to re-emit. No parallel data structure.
2. **The pump is event-driven, not wall-clock-driven.** The application's pump tick is the only event; no `pumpNs` floor, no per-record evidence tracking, no destination masks.
3. **Asynchrony is preserved.** No timing predicate appears anywhere in the protocol; the application's tick *is* the event, and silent ticks emit nothing.

### Replay rules

Three replay outputs, each gated on a single committed-state flag. None short-circuit on local saturation (e.g. "we accepted, so stop replaying ready") — BPR's purpose is helping *other* peers progress, not the local one.

| Replay action | Flag gate | Notes |
|---|---|---|
| `BRACHA87_INITIAL_ALL` | `F1_ORIGIN` | Originator emits its own (initial, v) on every Bpr call. The "stop once we ECHOed locally" optimisation is incorrect: in the n = 3t+1 boundary regime the echo-cascade threshold (n+t)/2+1 equals the count of honest peers, so any honest peer that missed the bootstrap can leave the cascade one echo short forever — only the originator can break the deadlock. |
| `BRACHA87_ECHO_ALL` | `F1_ECHOED` | Chained DTC rule: fires whenever Bracha is not firing send-echo on this dispatch and have-echoed is set. |
| `BRACHA87_READY_ALL` | `F1_RDSENT` | Chained DTC rule: fires whenever have-sent-ready is set. Continues post-accept (the "ACCEPTED → stop replay" optimisation strands slow peers). |

Two of the three (`replay (echo, v)` and `replay (ready, v)`) are captured as DTC sub-tables at the bottom of `bracha87Fig1.dtc`, chaining on existing paper-rule outputs and inputs. The third (origin INITIAL replay) is a single-bit C early-emit; the design intent is documented in the .dtc's BPR text section. This split follows the "trivial guards stay in C" principle from `decisionTableCompiler/README.md` — BPR rules whose ordering benefits from joint optimisation with paper rules go in DTC; single-input single-output guards stay in C.

### Where the pump is called

The pump is exposed at the layer that owns the Fig 1 instances:

| Layer | Pump entry point? | Why |
|---|---|---|
| Fig 1 | `bracha87Fig1Bpr` | Broadcast state lives here. |
| Fig 2 | n/a | Reference-only. |
| Fig 3 | none | Validator over already-accepted messages; no message state. |
| Fig 4 | none | Round-driven; Fig 4 broadcasts are new Fig 1 instantiations in the caller. |
| bkr94acs | `bkr94acsPump` | Owns N proposal Fig 1 instances + N×R×N consensus Fig 1 instances + N Fig 4 instances internally; only the bkr94acs pump can reach them. |

`bkr94acsPump` walks an internal cursor over (proposal phase, then consensus phase by origin × round × broadcaster), emits one Fig 1 instance's replays per call (≤ 3 actions), and returns 0 only when a full sweep finds nothing — the application's idle signal. Per-origin gating skips proposal replays for BA instances that decided 0 (the origin is excluded from the common subset and no honest peer needs further evidence); BAs decided 1 keep replaying per Bracha post-decide continuation.

### Application loop

With BPR, the application loop is two operations: drain the network and tick the pump. No ledger, no per-record mask, no per-peer evidence.

```c
struct bkr94acsAct out[BKR94ACS_PUMP_MAX_ACTS];
struct bkr94acsAct propAct;

/* Self-initiation: mark the local proposal Fig 1 as origin and emit
 * one PROP_SEND action (.type = BRACHA87_INITIAL) for the application
 * to broadcast.  The act's .value points into library storage. */
bkr94acsPropose(a, my_value, &propAct);
broadcast_action(propAct);

while (!silenceQuorumExit) {
  /* Drain ingress: Input handles paper rules + cascades. */
  while (network_recv(&msg)) {
    n = (msg.cls == BKR94ACS_CLS_PROPOSAL)
      ? bkr94acsProposalInput(a, ...)
      : bkr94acsConsensusInput(a, ...);
    for (k = 0; k < n; ++k) broadcast_action(actions[k]);
  }

  /* Pump tick: BPR re-emits committed actions. */
  n = bkr94acsPump(a, out);
  for (k = 0; k < n; ++k) broadcast_action(out[k]);

  sleep(tickMs);
}
```

`broadcast_action(act)` switches on `act.act` and sends:
- `BKR94ACS_ACT_PROP_SEND` — broadcast a proposal Fig 1 message of `act.type` (INITIAL/ECHO/READY) for `act.origin`, with bytes `act.value` (vLen+1, borrowed pointer into library state — copy if persisting).
- `BKR94ACS_ACT_CON_SEND` — broadcast a consensus Fig 1 message: `act.origin`, `act.round`, `act.broadcaster`, `act.type`, binary `act.conValue`.
- `BKR94ACS_ACT_BA_DECIDED` / `BKR94ACS_ACT_COMPLETE` — observability signals; no wire emission.
- `BKR94ACS_ACT_BA_EXHAUSTED` — fatal protocol-level event for `act.origin` (no decision within `maxPhases`); the local ACS instance cannot complete. Application aborts; treat as "did not complete" plus a specific cause.

`silenceQuorumExit` is the application's termination policy — see Deployment Notes below.

## API Overview

### bracha87 Entry Points

| Function | Purpose |
|---|---|
| `bracha87Fig1Sz(n, vLen)` | Compute allocation size for a Fig 1 instance |
| `bracha87Fig1Init(...)` | Initialize a Fig 1 instance |
| `bracha87Fig1Origin(f1, value)` | Mark this instance as broadcast originator and store the value (BPR — enables INITIAL replay) |
| `bracha87Fig1Input(f1, type, from, value, out)` | Process one incoming message; returns action count (0-3) |
| `bracha87Fig1Bpr(f1, out)` | Emit committed-action replays (BPR pump tick); returns action count (0-3); 0 = idle |
| `bracha87Fig1Value(f1)` | Retrieve committed value (returns non-null when ORIGIN or ECHOED) |
| `bracha87Fig3Sz(n, maxRounds)` | Compute allocation size for a Fig 3 instance |
| `bracha87Fig3Init(...)` | Initialize with N function and closure |
| `bracha87Fig3Accept(f3, round, sender, value, &vc)` | Submit an accepted message for validation |
| `bracha87Fig3RoundComplete(f3, round)` | Check if round k has n-t validated |
| `bracha87Fig3GetValid(f3, round, senders, values)` | Retrieve validated messages for round k |
| `bracha87Fig4Sz(n, maxPhases)` | Compute allocation size for a Fig 4 instance |
| `bracha87Fig4Init(...)` | Initialize with initial value, coin function, and closure |
| `bracha87Fig4Round(f4, round, n_msgs, senders, values)` | Process a completed round; returns action bitmask |

### bkr94acs Entry Points

| Function | Purpose |
|---|---|
| `bkr94acsSz(n, vLen, maxPhases)` | Compute allocation size for a BKR94 ACS instance |
| `bkr94acsInit(...)` | Initialize with peer index, coin function, and closure |
| `bkr94acsPropose(acs, value, out)` | Mark local proposal Fig 1 as originator and emit one PROP_SEND/INITIAL action (BPR) |
| `bkr94acsProposalInput(acs, origin, type, from, value, out)` | Process a proposal broadcast message; returns action count |
| `bkr94acsConsensusInput(acs, origin, round, broadcaster, type, from, value, out)` | Process a consensus message; returns action count |
| `bkr94acsPump(acs, out)` | BPR pump tick; cursor walks one Fig 1 per call; returns action count (0-3); 0 = full-sweep idle |
| `bkr94acsSubset(acs, origins)` | Retrieve the decided common subset |
| `bkr94acsProposalValue(acs, origin)` | Retrieve accepted proposal value for an origin |
| `bkr94acsActIdentity(act, out, outCap)` | Fixed-length [act, origin, round, broadcaster, type] identity tuple for use as a wire correlator on transports that need a uniqueness key; returns BKR94ACS_ACT_IDENTITY_LEN (5) for SEND acts, 0 otherwise |
| `bkr94acsBaDecision(acs, origin)` | BA decision for origin: 0xFF undecided / 0xFE exhausted / 0 excluded / 1 included |
| `bkr94acsCommittedFig1Count(acs)` | Count of Fig1 instances with any committed flag (ORIGIN/ECHOED/RDSENT); useful for cadence sizing |

Read `acs->complete` directly to check if all N BAs have decided. Read `acs->cursorPhase` / `cursorOrigin` / `cursorRound` / `cursorBroadcaster` directly to inspect the pump cursor position.

### Action Struct

`struct bkr94acsAct` carries one library emission. Field usage by `act.act`:

| act value | meaning | populated fields |
|---|---|---|
| `BKR94ACS_ACT_PROP_SEND` | broadcast a proposal Fig 1 message | `.origin`, `.type` (BRACHA87_INITIAL/ECHO/READY), `.value` (vLen+1 bytes, borrowed pointer) |
| `BKR94ACS_ACT_CON_SEND` | broadcast a consensus Fig 1 message | `.origin`, `.round`, `.broadcaster`, `.type`, `.conValue` (binary) |
| `BKR94ACS_ACT_BA_DECIDED` | a BA reached a decision | `.origin`, `.conValue` (0=excluded, 1=included) |
| `BKR94ACS_ACT_COMPLETE` | all N BAs decided | (none) |
| `BKR94ACS_ACT_BA_EXHAUSTED` | a BA's Fig 4 reached `maxPhases` with no decision; the local ACS instance cannot complete (BKR94 Lemma 2 Part B's BA-termination assumption violated). No safe in-protocol recovery — substituting a unilateral decision could disagree with another peer's actual decision and break SubSet agreement. Application must abort and (optionally) restart with fresh state. The library marks `bkr94acsBaDecision[origin] = 0xFE` and continues pumping replays for this origin (other peers may still benefit from earlier-round echoes / readys). Emitted exactly once per BA per ACS instance. | `.origin` |

`.value` is a borrowed pointer into the library's accepted-value slot; valid until the next library call that mutates state. Caller copies if persistence is needed past that boundary.

### Caller Composition Pattern

For multi-value agreement, use `bkr94acs` and the application loop shown in the **Bracha Phase Re-emitter (BPR)** section above — it manages all Fig 1 instances internally and provides `bkr94acsPump` for BPR replays.

For raw binary consensus (Fig 1 + Fig 3 + Fig 4 directly), the caller manages the Fig 1 instances per (origin, round). On every tick, the application iterates its Fig 1 array and calls `bracha87Fig1Bpr` per entry.

```c
/* Per process: */
struct bracha87Fig1 *fig1[maxRounds * n];  /* one per (origin, round) */
struct bracha87Fig4 *fig4;                 /* embeds Fig3 as fig4->fig3 */
struct bracha87Fig3 *fig3 = &fig4->fig3;
unsigned char nextRound = 0;

/* Self-initiation: mark our own (origin=self, round=0) Fig 1 as originator. */
bracha87Fig1Origin(fig1[0 * n + self], &my_initial_value);
broadcast INITIAL(my_initial_value) for round 0, origin self, to all peers

/* On incoming message (round, type, from, origin, value): */
f1 = fig1[round * n + origin];
nout = bracha87Fig1Input(f1, type, from, &value, out);
for each action in out:
  if ACCEPT:
    cv = bracha87Fig1Value(f1);
    bracha87Fig3Accept(fig3, round, origin, cv[0], &vc);
    while nextRound < max && bracha87Fig3RoundComplete(fig3, nextRound):
      rcnt = bracha87Fig3GetValid(fig3, nextRound, rsnd, rval);
      act = bracha87Fig4Round(fig4, nextRound, rcnt, rsnd, rval);
      ++nextRound;
      if act & BRACHA87_BROADCAST:
        bracha87Fig1Origin(fig1[nextRound * n + self], &fig4->value);
        broadcast INITIAL(fig4->value) for nextRound, origin self
      if act & BRACHA87_DECIDE:
        deliver fig4->decision
      if act & BRACHA87_EXHAUSTED:
        consensus failed within operational limit
  if INITIAL_ALL or ECHO_ALL or READY_ALL:
    cv = bracha87Fig1Value(f1);
    send the named action(cv) for this round/origin to all peers

/* Pump tick (BPR): iterate every owned Fig 1 instance. */
for each fig1 instance:
  nout = bracha87Fig1Bpr(fig1, out);
  for each action in out:
    cv = bracha87Fig1Value(fig1);
    send the named action(cv) for the corresponding (round, origin) to all peers
```

## Operational Limits

| Parameter | Encoding | Range | Notes |
|---|---|---|---|
| `n` | unsigned char | 1-256 (n+1) | Process count |
| `t` | unsigned char | 0-85 | Max Byzantine faults; n+1 > 3t required |
| `vLen` | unsigned char | 1-256 (vLen+1) | Value length in bytes |
| `maxPhases` | unsigned char | 1-85 | 85 * 3 = 255 rounds fits in unsigned char |

Round indices range from 0 to 3 * maxPhases - 1 (max 254).

## Implementation Notes

Issues discovered by reading the paper against the code. Most were missed by isolation testing and only caught through composed simulation.

1. **Post-decide continuation.** The paper says "Go to round 1 of phase i+1" after all three step 3 cases. A decided process must continue broadcasting so others can reach consensus. `BRACHA87_DECIDE | BRACHA87_BROADCAST` is returned exactly once; subsequent rounds return `BRACHA87_BROADCAST` only.

2. **D_FLAG leak.** After deciding, step 2 may set the D_FLAG on the value. Step 3's decided path restores the plain decision value to prevent D_FLAG from leaking into step 1 broadcasts of the next phase.

3. **N function existential quantifier.** The paper defines VALID^k with "there exist n-t messages..." Passing only the first n-t to N rejects messages that a correct process produced from a different subset. Fix: pass all validated messages; N returns permissive when subsets could disagree.

4. **Dead cascade after INITIAL.** The cascade after INITIAL could never fire -- if any threshold were met, `echoed` would already be set via Rule 2/3. Removed; comment explains the proof.

5. **Committed value memcpy.** The memcpy on Rules 4/5/6 appears redundant but is essential. A Byzantine initial can cause commitment to the wrong value; the memcpy corrects it when the threshold-reaching value differs from the committed value.

6. **Subset-majority reachability threshold (step 1).** Under N's tie-break-to-0, value 0 is reachable in some n-t subset iff `cnt[0] >= (nt+1)/2` (unified formula: equals `nt/2` for even n-t, `nt/2+1` for odd); value 1 is reachable iff `cnt[1] >= nt/2+1` (strict majority). Permissive iff both reachable. Using the symmetric `>= nt/2+1` test on both sides wrongly rejects honest tie-subset 0s when n-t is even. Verified by exhaustive enumeration for n=4..16.

7. **Forward cascade fires on every growth past n-t, not only first crossing.** `VALID^r_p` is existential over n-t subsets of `VALID^{r-1}_p` and monotone in it (paper definition + Lemma 6), so new validated messages at round k unlock stored unvalidated messages at k+1 even after round k first reached n-t. Gating the forward re-check on "first crossing only" strands honest round-(k+1) messages when validation of them depended on subsets that only exist after k grew.

8. **Permissive D_FLAG permission conveyed via `*result`.** On permissive return from Fig 4's N function (`rc > 0`), `*result & BRACHA87_D_FLAG` is set only when some n-t subset legitimately produces a decision candidate. Fig 3 rejects incoming D_FLAG when that bit is clear, preventing Byzantine d-injection in the no-majority windows of step 3 cases 1 and 2.

9. **Post-decide value preservation across sub-rounds.** During post-decide continuation (Note 1), `b->value` is preserved as the decision through every sub-round of subsequent phases. The .dtc-faithful Fig 4 dispatch zeroes the `setMajority` and `setDMajority` outputs when `have_decided = yes`, so adversarial inputs whose majority disagrees with the decision cannot drift the broadcast value away from it. Verified by `testFig4PostDecideAdversarial` (which would have failed against a pre-DTC version that overwrote `b->value` with majority/(d, majority) at sub-rounds 0 and 1 of post-decide phases).

10. **BPR (ready, v) replay must NOT short-circuit on accepted.** An accepted peer owes its READY to peers still below the 2t+1 threshold; replay is the only mechanism Bracha provides for getting it there under loss. The "ACCEPTED → stop replay" optimisation strands slow peers — the very purpose of BPR is helping *other* peers' progress, not the local one. Regression check: `testFig1Bpr` post-accept assertions.

11. **BPR (initial, v) replay must NOT short-circuit on echoed.** An originator that has locally echoed (via Rule 1 from loopback or via Rule 2 from echoes received from other peers) cannot stop replaying INITIAL. In the n = 3t+1 boundary regime the Rule 2 echo threshold (n+t)/2 + 1 equals the count of honest peers, so any honest peer that missed the bootstrap can leave the cascade one echo short forever — only the originator can break the deadlock. Symmetric with Note 10: both pitfalls reject local-state-as-saturation arguments. Regression check: `testBprByzantineSilent` (n=4 t=1 with one silent Byzantine peer; the original gap-4 design with the `!ECHOED` gate stalled at |SubSet|=1 over 50000+ pump sweeps; the corrected "always replay INITIAL while ORIGIN" rule converges in 1 sweep).

## Building

```bash
make            # build .o and example
make check      # build and run tests (test_bracha87, test_bkr94acs, test_predicates)
make clean      # remove build artifacts
make clobber    # remove DTC generated .c files
```

Building requires `../decisionTableCompiler/dtc` and `awk` to compile the `.dtc` rule tables to dispatch C snippets; the Makefile invokes both automatically. The generated `.psu` and `*Fig{1,3,4}.c` / `bkr94acsRules.c` files are reproducible artifacts (`make clobber` removes them, the next `make` regenerates).

The bracha87 example demonstrates binary consensus (Fig1+Fig3+Fig4):

```bash
./example_bracha87 4 1                          # 4 peers, 1 Byzantine fault
./example_bracha87 -s 42 7 2                    # shuffled delivery
./example_bracha87 -b 3 7 2                     # Byzantine peer 0 equivocates
./example_bracha87 -v 4 1 0 0 1 1              # verbose trace, split initial values
```

The bkr94acs example demonstrates multi-value agreement on arbitrary strings:

```bash
./example_bkr94acs 4 1 joe sam sally tim      # 4 peers propose strings
./example_bkr94acs -s 42 4 1 joe sam sally tim  # shuffled delivery (different subset)
./example_bkr94acs 4 0 joe sam sally tim      # t=0: all proposals included
./example_bkr94acs -v 7 2 alpha bravo charlie delta echo foxtrot golf
```

Compiler flags: `-std=c89 -pedantic -Wall -Wextra -Os -g`

## Deployment Notes

This library is protocol-only. A working deployment needs a transport layer satisfying the three System Model assumptions above, plus identity, keys, a coin source, and a termination policy. The points below are load-bearing; do not "optimize" them away.

### BPR replaces the application-layer ledger

A previous deployment pattern wrapped this library in a per-record ledger: `peerHasPsk[]` / `peerOriginAck[]` / `peerRound[]` evidence tracking, `destMask` per-record bitmaps, a cursor over the record list, per-record `pumpNs` floors. With BPR, that machinery is no longer needed — the library's own `bkr94acsPump` (or `bracha87Fig1Bpr` at the bare-bracha87 layer) is the only mechanism that guarantees eventual delivery, and replay state is intrinsic to the protocol's own committed flags. The application loop is two operations: drain ingress, tick the pump (see the BPR section above for the loop sketch).

Wire-level efficiencies like RSEC (Reed-Solomon erasure coding) and inter-shard delay are still useful as efficiency tuning under steady drop, but they are not reliability mechanisms; only BPR is. RSEC reduces the per-shard miss rate; BPR closes the residual gap.

### Termination policy

A decided peer must keep broadcasting (Implementation Note 1) so others can reach consensus. Two obvious exit mechanisms are both wrong:

- Exit on `BKR94ACS_ACT_COMPLETE` — violates post-decide continuation; peers deciding last can be stranded.
- Broadcast a "DONE" message and exit on a threshold of receipts — the DONE has no retransmit siblings in the typical ledger model; loss of the initial emission strands peers that never hear it before early completers exit.

The principled alternative is a **silence-quorum exit gated on a sweep count.** Each peer tracks (a) the local tick at which every other peer last advanced its observable state — Fig 4 round, Fig 1 proposal/consensus phase transition, BA decision — and (b) how many BPR sweeps it has performed. A peer whose state has not advanced for a chosen silence window is "done-silent." Exit when at least `n-t-1` others are done-silent **and** at least K BPR sweeps have completed. The threshold is `n-t-1`, not `n-t` — self is implicit because completion is known locally. Using `n-t` is silently wrong at `t>0` and unreachable at `t=0`.

Both clauses are load-bearing. The silence clause alone has two failure modes that must be repaired by the sweep clause:

1. **Premature exit cuts post-decide feeding short.** Once a decided peer's local state stops advancing, the natural silence window (a small multiple of declared max RTT, e.g. 8 ticks) is shorter than one full BPR pass — `bkr94acsPump` walks one Fig1 per call, so a single pass through every committed Fig1 takes `bkr94acsCommittedFig1Count(a)` Pump calls, typically dozens to hundreds. If the silence window expires before that pass completes, slow honest peers are stranded with un-emitted committed Fig1s they still need.
2. **Honest-slow looks like done.** A slow peer's BPR replays of its own committed flags arrive at fast peers as duplicates — `bkr94acsConsensusInput` returns `nacts == 0` for already-known content, so `peerProgressTick[slow_peer]` does not refresh. Without the sweep clause, fast peers conclude the slow peer is silent and exit while the slow peer is still trying to catch up.

#### What "one sweep" means

One sweep = `bkr94acsCommittedFig1Count(a)` Pump calls. After that many calls, every currently-committed Fig1 instance has been visited and its committed actions emitted at least once. K sweeps gives every committed Fig1 K emissions, so a slow peer's expected received-emissions per Fig1 is `K × (1 − loss)`. K is the deployment knob calibrated against assumed loss rate (NOT against RTT).

Recompute the threshold each Pump call: `bkr94acsCommittedFig1Count(a)` grows during the run as new rounds advance, and a peer that picks up new owned Fig1s late must not shortcut the gate.

**Do not measure sweeps by cursor-position wraparound.** The full cursor space is `N + N × maxRounds × N` (with `maxRounds = maxPhases × 3`, default 255). When committed Fig1s are dense — every consensus round each peer reached has its own committed Fig1 per broadcaster — the cursor advances ~1 position per Pump call, and a position-space sweep takes thousands of ticks even at small N. The Pump-count metric is the actual coverage signal; the cursor space is an implementation detail.

#### Symmetric application: pre-decide patience and post-decide feeding

Apply the K-sweep gate symmetrically:

- **Decided peers** (done or exhausted): K sweeps post-decide is the feeding obligation — K retransmissions of every committed Fig1 to slow peers.
- **Undecided peers**: K sweeps from epoch start is the patience obligation — K opportunities to RECEIVE missing pieces from peers whose own pumps are wrapping at similar rates. Without symmetric patience, an undecided peer with high apparent silence abandons before any peer's first sweep, defeating the post-decide feeding obligation at the receiving end.

Both clauses are the same K; by peer similarity (similar Fig1 counts → similar sweep durations), K of own sweeps ≈ K of every other peer's sweeps.

#### Use `peerProgressTick`, not `lastIngressTick`

Track silence by state-advancing inputs (an Input call returning `nacts > 0`, or an out-of-band first arrival like a key delivery), not by raw incoming bytes. State-advance is the actual progress signal; raw bytes are not. Refreshing on raw bytes also exposes an informed-DoS surface — an attacker sending HMAC-valid garbage at any cadence delays exit indefinitely. The sweep clause makes peerProgressTick safe even under the duplicate-suppression failure mode above (failure mode 2): pre-decide patience holds the loop open until the slow peer can actually catch up.

#### Abandonment

A peer that satisfies `silence quorum AND K sweeps AND !done AND !exhausted` is **abandoned**: no honest quorum is reachable for this epoch. The application must surface abandonment as a distinct outcome from `BKR94ACS_ACT_COMPLETE` and from `BKR94ACS_ACT_BA_EXHAUSTED`, and **must not commit any unilateral decision** — any substitute could disagree with another peer's actual decision (Lemma 2 Part C). The decision membership for the local epoch is empty; the application falls back to a higher-level recovery (next epoch, membership reconfiguration, etc.).

### Coin choice

Fig 4 step 3 case (iii) — when neither decision-count rule fires — requires a coin. Options:

- **Common coin** (same value across all peers per phase): requires a shared randomness source such as a verifiable random beacon or a distributed coin protocol.
- **Local coin** (each peer flips independently): e.g. `arc4random_buf` per peer.
- **Deterministic coin** (e.g. `phase & 1`): effectively a cheap common coin under a non-adversarial scheduler. Useful for deterministic tests, not recommended for adversarial threat models.

Mostéfaoui, Perrin, and Weibel (PODC 2024, *"Randomized Consensus: Common Coins Are Not the Holy Grail!"*) prove common coin is optimal **only when `t > n/3`**; in Bracha's `t < n/3` regime local coin actually outperforms. The naïve "all honest peers must flip identically" lower bound ignores Fig 4's convergence dynamics: most phases terminate via step 3 case (i) (`>2t` agreement, no coin used) or case (ii) (`>t` agreement, adopt and amplify); case (iii) is the tie-breaker. At practical `n` values within `maxPhases = 85`, local coin works well.

Guidance: **for `t < n/3`, use a local coin.** Reach for a common coin only when pushing beyond `t = n/3` (which Bracha itself does not cover).

### No timing in the protocol

The protocol's correctness — both safety and eventual termination — depends on no timing assumption; this is the asynchronous-BFT model. Any timing parameters in a deployment (retransmit cadence, silence thresholds, pump tick) govern the transport wrapper and termination policy, not the state machines in this library. Correctness holds under arbitrary asynchrony; termination speed depends on the operator's tuning.

## License

LGPL v3 or later. See `COPYING.LESSER` and `COPYING`.
