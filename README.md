# asynchronousByzantineAgreementProtocols

Generated with Claude Code (https://claude.ai/code)

A C library implementing Gabriel Bracha's 1987 paper (Figures 1, 3, and 4 as composable pure state machines; Figure 2 captured for paper completeness and subsumed by Figure 3), plus the BKR94 Asynchronous Common Subset (ACS) protocol built from them.

## Overview

This is the only known implementation of Bracha 1987 as composable pure state machines, with module boundaries that match the paper's figures. ANSI C89, zero dependencies. No I/O, no threads, no dynamic allocation -- the caller provides memory and executes output actions.

Each module boundary matches the paper exactly, so the paper's proofs apply per-module: Lemmas 1-4 to Fig 1, Lemmas 5-7 to Fig 2/3, Lemmas 9-10 and Theorems 1-2 to Fig 4.

The `bkr94acs` module composes these figures into multi-value agreement: N peers propose arbitrary values, and all honest peers agree on the same common subset of at least n-t proposals. This is Ben-Or/Kelmer/Rabin 1994 Section 4 Figure 3 (Protocol Agreement[Q]).

This README serves two audiences. **If you are integrating the library**, the load-bearing sections are *When to Use What*, *System Model*, *API Overview*, *Examples*, and *Deployment Notes*. **If you are auditing the implementation or porting it to another language**, additionally read *Design Rationale*, *Architecture*, *Bracha Phase Re-emitter*, *Test Coverage*, *Correctness Audit*, *Implementation Notes*, and *Re-Implementing in Another Language*.

## When to Use What

This library provides two application-facing primitives. Pick by the shape of your problem.

**Reliable broadcast — `bracha87Fig1`.** One designated sender announces a value; all correct peers either accept the same value or none do, under up to `t` Byzantine faults at `n > 3t`. Use when you have a known originator per message: configuration distribution from a designated source, single-writer state replication, one-shot dissemination of a signed announcement, or as a reliable-channel building block inside your own outer protocol. See `example/bracha87Fig1.c`.

**Common subset — `bkr94acs`.** N peers each propose a value; all correct peers agree on the same common subset of at least `n-t` proposals. Use when you need leaderless agreement on a batch of contributions: HoneyBadger-style atomic broadcast batching, MPC input bundling, distributed candidate selection — anything shaped as "agree on the set" rather than "agree on a single value." See `example/bkr94acs.c`.

Fig 3 (VALID-set framework) and Fig 4 (binary Byzantine agreement) are exposed for completeness but exist primarily as internal mechanism feeding `bkr94acs`; raw single-bit binary BA has no realistic standalone caller (the seven-year gap between Bracha 1987 and BKR94 1994 is exactly that evidence).

## The Papers

Gabriel Bracha, "Asynchronous Byzantine Agreement Protocols," *Information and Computation* 75, 130-143 (1987). Implemented in `bracha87.[hc]`.

Michael Ben-Or, Boaz Kelmer, Tal Rabin, "Asynchronous Secure Computations with Optimal Resilience (Extended Abstract)," PODC '94, pages 183-192. Section 4 Figure 3 (Protocol Agreement[Q]) is implemented in `bkr94acs.[hc]`.

J. H. Saltzer, D. P. Reed, D. D. Clark, "End-To-End Arguments in System Design," *ACM Transactions on Computer Systems* 2(4), 277-288 (1984). Cited as the design rationale for placing the BPR (Bracha Phase Re-emitter) pump at the protocol endpoint rather than in a lower transport layer.

`Bracha87.txt` is a companion summary of the Bracha 1987 paper: figures, rules, VALID set definitions, all lemma/theorem statements, and a mapping from each lemma to the test that verifies it.

**Paper typo:** Fig. 1 says "(n+t)/2 (echo,v) messages" but the Lemma 1 proof says "more than (n+t)/2." The proof requires strict `>` for the pigeonhole argument to work. The code follows the proof, not the figure.

`BKR94ACS.txt` is the line-by-line extract of BKR94 Section 4 used as `bkr94acs.[hc]`'s reference.

`SRC84.txt` is the relevant extract of the End-to-End paper used as the design citation for BPR.

## Design Rationale — Why These Three Papers

This library exists because the alternatives we evaluated all required machinery we did not want to depend on. The three papers above were chosen as the smallest combination that satisfies our constraints — authenticated multi-value agreement under fair-loss asynchrony, no trusted setup, no pairing-based crypto, no DKG, embeddable in C89 with no dynamic allocation. The choice was load-bearing on each constraint; this section names the alternatives and the reason each was rejected.

### Why Bracha 1987 for reliable broadcast

The reliability primitive we needed is authenticated reliable broadcast (RBC) at `n > 3t`, signature-free, setup-free.

Bracha's three-phase counting-threshold mechanism (initial / echo / ready) is the only RBC primitive we found that works at `n > 3t` with neither signatures nor a setup ceremony — authenticated point-to-point channels (HMAC over a pre-shared key, TLS, mutual SSH) are sufficient. Equally important, the paper's module boundary is a crisp algebraic interface (Fig 1 is reliable broadcast, Fig 3 is VALID-set validation, Fig 4 is consensus), so each lemma applies per-module and the audit chain shown later in this document is possible. A signature-based RBC bundles cryptographic verification into the protocol logic and forecloses that decomposition.

### Why BKR94 for asynchronous common subset

The agreement primitive we needed is multi-value agreement on a common subset of `n-t` proposals, asynchronous, Byzantine-resilient, with no leader and no view-change machinery.

BKR94 alone is the smallest piece that does ACS with no setup, no leader, and no threshold cryptography — `n` Bracha Fig 1 instances feed `n` binary agreements, and the step-2 trigger ("`n-t` BAs decided 1, vote 0 in the rest") closes it out. We deliberately stopped at ACS: BKR94 itself continues to ASC (asynchronous secure computation, the MPC layer), but ASC has no caller in the stack we are building, and pulling it in would require a private-channels mesh that ACS itself cannot bootstrap.

### Why Saltzer-Reed-Clark 1984 for BPR placement

Bracha's correctness proofs presume reliable point-to-point channels between correct processes. Over fair-loss datagrams that assumption does not hold, and something must close the gap.

The end-to-end argument (Saltzer/Reed/Clark 1984) is exactly the principle that resolves this: the reliability function should live at the layer that has the complete information needed to perform it correctly. For our reliability function — "deliver INITIAL/ECHO/READY despite a fair-loss network until the protocol no longer owes them" — the complete information lives in the Bracha state machine itself (`F1_ORIGIN`, `F1_ECHOED`, `F1_RDSENT`, plus the BKR per-origin BA-decided byte). BPR is the smallest re-emitter that runs over exactly that state, with no parallel data structure and no timing predicate. Lower-layer wire optimizations (RSEC, batching, inter-shard delay) remain admissible under SRC84's performance carve-outs (P1, P2) — they tune retry frequency without taking on the correctness obligation.

### What we deliberately did not build

For comparison shoppers: the following are absent by design, not by oversight.

- **No DKG, no trusted setup, no threshold signatures, no pairing-based crypto, no verifiable random beacon.** Setup is one symmetric authentication credential per peer pair.
- **No bundled coin source.** The library is coin-agnostic — both `bracha87Fig4Init` and `bkr94acsInit` take a `bracha87CoinFn` callback that the caller supplies. The bundled examples use a deterministic alternating coin (demo only); adversarial deployments are expected to supply their own. See "Coin choice — caller responsibility" in Deployment Notes.
- **No transaction layer, no atomic broadcast wrapper, no application semantics.** BKR94 ACS produces a sorted subset of `n-t` arbitrary byte strings; what those bytes mean is the caller's choice.
- **No partial-synchrony assumption.** Correctness (safety and termination) holds under arbitrary asynchrony.
- **No leader, no view change, no pacemaker.** All peers are symmetric.
- **No dynamic allocation, no I/O, no threads.** The library is a pure state machine; the caller provides memory and a transport.

---

## System Model — What the Caller Must Provide

The paper's proofs depend on three assumptions about the communication system (Section 2):

> "We assume a reliable message system in which no messages are lost or generated. Each process can directly send messages to any other process, and can identify the sender of every message it receives."

These assumptions are not optional — they are load-bearing requirements of every lemma and theorem in the paper. This library is a pure state machine with no I/O. **The caller is responsible for building a transport layer that satisfies them:**

1. **Eventual delivery under fair-loss.** Every message sent between correct processes must eventually arrive — but messages may be silently dropped any finite number of times in transit. The Bracha Phase Re-emitter (BPR, see below) closes the gap from "fair-loss point-to-point" to "reliable delivery" intrinsically, so the transport need not provide retransmission of its own; the caller's pump tick is what drives BPR's replays. Per-message reliability is no longer the transport's job.

2. **No message fabrication.** The transport must not generate messages that were never sent. A Byzantine process may send arbitrary content, but the transport itself must not invent messages. In practice this means authenticated channels.

3. **Sender identification.** The receiver must know which process sent each message, and a Byzantine process must not be able to impersonate a correct one. In practice this means authentication bound to process identity.

The protocol's correctness (both safety and termination) does not depend on any timing assumption — that is the asynchronous-BFT model. Pump cadence, silence-quorum exit windows, and any other timing parameters in a deployment are operator-tuning, not protocol invariants.

The System Model deliberately does **not** require: a distributed key generation (DKG), a trusted setup, threshold signatures, pairing-based crypto, or a verifiable random beacon. Authenticated point-to-point channels (assumption 3 above) suffice — provisioned by whatever mechanism the deployment prefers (HMAC over a pre-shared key, TLS, mutual SSH, Noise, etc.). Setup cost is one symmetric authentication credential per peer pair.

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

The canonical Bracha reliable broadcast (RBC) — three-phase initial/echo/ready with `n > 3t`, no signatures, no setup. One instance per (sender, round) pair. Implements the six rules from the paper's table: incoming initial/echo/ready messages trigger echo, ready, and accept actions based on counting thresholds. Per-peer echo/ready values are stored (not just counts) so `echo_count[v]` is computed correctly for any v.

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

Decided processes continue participating so others can reach consensus. Bracha's Theorem 2 bounds the expected number of phases at O(1) under a common-coin assumption; the actual termination behavior depends on the coin function the caller supplies via `bracha87CoinFn` — see "Coin choice — caller responsibility" under Deployment Notes.

### bkr94acs -- BKR94 Asynchronous Common Subset

Composes Bracha87 Fig 1 and Fig 4 into multi-value agreement, implementing Ben-Or/Kelmer/Rabin 1994 Section 4 Figure 3 (Protocol Agreement[Q]). BKR94 ACS is the consensus core of HoneyBadgerBFT-family asynchronous BFT systems; this implementation provides it as a standalone primitive without bundling a transaction layer or a transport. See `BKR94ACS.txt` for the line-by-line extract used as the implementation's reference.

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

`bkr94acsPump` walks an internal cursor over (proposal phase, then consensus phase by origin × round × broadcaster), emits one Fig 1 instance's replays per call (≤ 3 actions), and returns 0 only when a full sweep finds nothing — the application's idle signal. Per-origin gating, indexed by `bkr94acsBaDecision(a, origin)`:

- **0xFF (undecided)** — pump; other honest peers may still need our echoes/readys to learn Q(j)=1.
- **0xFE (EXHAUSTED)** — pump; the local BA can't complete, but other peers may still benefit from our earlier-round echoes/readys.
- **0 (decided 0, excluded)** — skip; the origin is out of the common subset and no honest peer needs further evidence from us.
- **1 (decided 1, included)** — pump; Bracha post-decide continuation requires us to keep feeding peers that haven't yet reached n-t.

Only the decided-0 case stops pumping.

### Application loop

With BPR, the application loop is two operations: drain the network and tick the pump. No ledger, no per-record mask, no per-peer evidence.

```c
struct bkr94acsAct out[BKR94ACS_PUMP_MAX_ACTS];
struct bkr94acsAct propAct;
struct bracha87Pump pump;

bracha87PumpInit(&pump);

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

  /* Pump tick: BPR re-emits committed actions.  ONE call per tick
   * — see the network flood warning in bracha87.h. */
  n = bkr94acsPump(a, &pump, out);
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

bracha87 exposes paper-vocabulary state-machine operations (one rule cluster each), plus a Fig 1 array BPR Pump wrapper (one cursor sweep per call).  Each Fig 1 / Fig 3 / Fig 4 entry point takes the instance plus the rule's natural inputs and returns its actions; cascading across layers is the caller's responsibility (or, for the canonical composition, `bkr94acs`'s responsibility).

| Function | Purpose |
|---|---|
| `bracha87Fig1Sz(n, vLen)` | Compute allocation size for a Fig 1 instance |
| `bracha87Fig1Init(...)` | Initialize a Fig 1 instance |
| `bracha87Fig1Origin(f1, value)` | Mark this instance as broadcast originator and store the value (BPR — enables INITIAL replay) |
| `bracha87Fig1Input(f1, type, from, value, out)` | Process one incoming message; returns action count (0-3) |
| `bracha87Fig1Bpr(f1, out)` | Emit committed-action replays for one Fig 1 instance; returns action count (0-3); 0 = idle |
| `bracha87Fig1Value(f1)` | Retrieve committed value (returns non-null when ORIGIN or ECHOED) |
| `bracha87Fig3Sz(n, maxRounds)` | Compute allocation size for a Fig 3 instance |
| `bracha87Fig3Init(...)` | Initialize with N function and closure |
| `bracha87Fig3Accept(f3, round, sender, value, &vc)` | Submit an accepted message for validation |
| `bracha87Fig3RoundComplete(f3, round)` | Check if round k has n-t validated |
| `bracha87Fig3GetValid(f3, round, senders, values)` | Retrieve validated messages for round k |
| `bracha87Fig4Sz(n, maxPhases)` | Compute allocation size for a Fig 4 instance |
| `bracha87Fig4Init(...)` | Initialize with initial value, coin function, and closure |
| `bracha87Fig4Round(f4, round, n_msgs, senders, values)` | Process a completed round; returns action bitmask |

#### Fig 1 array BPR Pump

Wraps the per-instance `bracha87Fig1Bpr` with a cursor that walks an application-owned array of Fig 1 instances.  Useful for reliable-broadcast applications that own multiple Fig 1 instances of any shape (single-broadcast streaming, multi-origin reliable multicast, etc.).  Pump cursor lives in caller storage, initialized with `bracha87PumpInit`.

| Function | Purpose |
|---|---|
| `bracha87PumpInit(p)` | Initialize a shared Pump cursor |
| `bracha87Fig1PumpStep(instances, count, p, out, outCap)` | Walk a caller-owned Fig 1 array; one instance's BPR actions per call; returns 0 on full-sweep idle |
| `bracha87Fig1CommittedCount(instances, count)` | Count of instances with any committed flag (ORIGIN/ECHOED/RDSENT); for K-sweep cadence |

`struct bracha87Fig1Act` carries the act: `act` (INITIAL_ALL / ECHO_ALL / READY_ALL), `idx` (array index), `value` (borrowed).

The same `struct bracha87Pump` cursor type is also consumed by `bkr94acsPump` (cursor unification across the library).

### bkr94acs Entry Points

| Function | Purpose |
|---|---|
| `bkr94acsSz(n, vLen, maxPhases)` | Compute allocation size for a BKR94 ACS instance |
| `bkr94acsInit(...)` | Initialize with peer index, coin function, and closure |
| `bkr94acsPropose(acs, value, out)` | Mark local proposal Fig 1 as originator and emit one PROP_SEND/INITIAL action (BPR) |
| `bkr94acsProposalInput(acs, origin, type, from, value, out)` | Process a proposal broadcast message; returns action count |
| `bkr94acsConsensusInput(acs, origin, round, broadcaster, type, from, value, out)` | Process a consensus message; returns action count |
| `bkr94acsPump(acs, p, out)` | BPR pump tick using shared `struct bracha87Pump` cursor; one Fig 1 per call; returns action count (0-3); 0 = full-sweep no-commit (one-call-per-tick — see flood warning) |
| `bkr94acsSubset(acs, origins)` | Retrieve the decided common subset |
| `bkr94acsProposalValue(acs, origin)` | Retrieve accepted proposal value for an origin |
| `bkr94acsBaDecision(acs, origin)` | BA decision for origin: 0xFF undecided / 0xFE exhausted / 0 excluded / 1 included |
| `bkr94acsCommittedFig1Count(acs)` | Count of Fig1 instances with any committed flag (ORIGIN/ECHOED/RDSENT); useful for cadence sizing |

Test `acs->flags & BKR94ACS_F_COMPLETE` to check if all N BAs have decided (the `BKR94ACS_F_THRESHOLD` bit indicates step-2 vote-0 fanout has fired).  Pump cursor lives in caller storage (`struct bracha87Pump`); the application owns the cursor lifetime, the same way every other Pump entry point in this library works.

### Action Struct

`struct bkr94acsAct` carries one library emission. Field usage by `act.act`:

| act value | meaning | populated fields |
|---|---|---|
| `BKR94ACS_ACT_PROP_SEND` | broadcast a proposal Fig 1 message | `.origin`, `.type` (BRACHA87_INITIAL/ECHO/READY), `.value` (vLen+1 bytes, borrowed pointer) |
| `BKR94ACS_ACT_CON_SEND` | broadcast a consensus Fig 1 message | `.origin`, `.round`, `.broadcaster`, `.type`, `.conValue` (binary) |
| `BKR94ACS_ACT_BA_DECIDED` | a BA reached a decision | `.origin`, `.conValue` (0=excluded, 1=included) |
| `BKR94ACS_ACT_COMPLETE` | all N BAs decided | (none) |
| `BKR94ACS_ACT_BA_EXHAUSTED` | a BA's Fig 4 reached `maxPhases` with no decision; the local ACS instance cannot complete (BKR94 Lemma 2 Part B's BA-termination assumption violated). No safe in-protocol recovery — substituting a unilateral decision could disagree with another peer's actual decision and break SubSet agreement. Application must abort and (optionally) restart with fresh state. The library marks `bkr94acsBaDecision[origin] = 0xFE` and continues pumping replays for this origin (other peers may still benefit from earlier-round echoes / readys). Emitted exactly once per BA per ACS instance. | `.origin` |

`.value` is a borrowed pointer into the library's committed-value slot — populated as soon as ORIGIN, Rule 1, 2, or 3 commits a value (i.e. while ECHOED is set, before ACCEPT) so PROP_SEND emissions can carry the bytes during ECHO/READY traffic. Valid until the next library call that mutates state. Caller copies if persistence is needed past that boundary. Distinct from `bkr94acsProposalValue`, which queries the same slot but is ACCEPT-gated for non-self origins so application reads see only Bracha-Lemma-2-protected values.

### Caller Composition Pattern

For multi-value agreement, use `bkr94acs` and the application loop shown in the **Bracha Phase Re-emitter (BPR)** section above — it manages all Fig 1 instances internally and provides `bkr94acsPump` for BPR replays. See `example/bkr94acs.c` for a runnable version of this pattern.

For reliable broadcast (Fig 1 only), use `bracha87Fig1Input` per message plus `bracha87Fig1PumpStep` on the caller-owned Fig 1 array per tick. See `example/bracha87Fig1.c` for a runnable version.

Direct use of Fig 3 (`bracha87Fig3Accept` / `RoundComplete` / `GetValid`) and Fig 4 (`bracha87Fig4Round`) is supported but those layers exist primarily as internal mechanism feeding `bkr94acs`; the realistic application surfaces are the two above. The low-level Fig 3 and Fig 4 entry points remain public for callers that need them (and for `test_predicates.c`, which exercises the algorithmic predicates beneath the dispatch).

## Operational Limits

| Parameter | Encoding | Range | Notes |
|---|---|---|---|
| `n` | unsigned char | 1-256 (n+1) | Process count |
| `t` | unsigned char | 0-85 | Max Byzantine faults; n+1 > 3t required |
| `vLen` | unsigned char | 1-256 (vLen+1) | Value length in bytes |
| `maxPhases` | unsigned char | 1-85 | `85 * BRACHA87_ROUNDS_PER_PHASE = 255` rounds fits in unsigned char |

Round indices range from 0 to `BRACHA87_ROUNDS_PER_PHASE * maxPhases - 1` (max 254).  Fig 3 is keyed by round (paper's `round(k)`); Fig 4 by phase (paper's `Phase(i)` with sub-actions `3i+1, 3i+2, 3i+3`).  `BRACHA87_ROUNDS_PER_PHASE` (= 3) names the conversion at the boundary so callers do not write the bare `* 3` / `/ 3`.  The paper-vocabulary `(phase, subRound)` view of any `round` value is `phase = round / BRACHA87_ROUNDS_PER_PHASE`, `subRound = round % BRACHA87_ROUNDS_PER_PHASE`; Fig 4 also exposes `bracha87Fig4.phase` / `.subRound` directly for diagnostic use.

## Building

```bash
make            # build .o and examples
make check      # build and run all five test binaries (see Test Coverage below)
make clean      # remove build artifacts
make clobber    # remove DTC generated .c files
```

Building requires `../decisionTableCompiler/dtc` and `awk` to compile the `.dtc` rule tables to dispatch C snippets; the Makefile invokes both automatically. The generated `.psu` and `*Fig{1,3,4}.c` / `bkr94acsRules.c` files are reproducible artifacts (`make clobber` removes them, the next `make` regenerates).

Compiler flags: `-std=c89 -pedantic -Wall -Wextra -Os -g`

## Test Coverage

`make check` runs five test binaries that exercise the library at three different scopes. Each scope catches a different class of regression; together they form a defense in depth.

| Binary | Scope | What it asserts |
|---|---|---|
| `test_predicates` | Algorithmic primitives | Paper-direct correspondence at n=4, t=1: `fig4Nfn` (960 inputs), `fig3IsValid` (165 evaluations), Fig 3 cascade (4 delivery permutations). White-box: `#include`s `bracha87.c` and enumerates inputs against a paper-direct subset-enumeration reference. Anchors the algorithmic primitives that sit below the DTC dispatch. |
| `test_bracha87` | Protocol white-box (bracha87) | Unit tests on each Bracha rule, composed simulation, lemma assertions inline (Lemmas 1, 2, 3, 4, 9), Theorem 2, shuffled delivery, Byzantine equivocation, post-decide multi-phase + adversarial-majority preservation, value-switch tests, BPR replay invariants (originator INITIAL replay forever, post-accept READY replay), Fig 1 array Pump coverage (`bracha87PumpInit` / `bracha87Fig1PumpStep` / `bracha87Fig1CommittedCount`), defensive null-pointer guards. Reads internal flags directly. |
| `test_bracha87_blackbox` | Protocol black-box (bracha87) | 153 checks via the public bracha87.h surface only. Validity, agreement, totality, and Lemma-2 invariants at n=4 t=1; precise echo-threshold tests at n=4 and n=7; Fig 1 array Pump cursor walk, sparse-slot skip, multi-act emission; low-level `bracha87Fig4Round` post-EXHAUSTED safety. Tests are derived from the header contract and `Bracha87.txt` only — no `bracha87.c` reads. |
| `test_bkr94acs` | Protocol white-box (bkr94acs) | All-to-all simulation with shuffled delivery, multi-byte values, identical proposals, larger N, post-decide continuation regression, step-2 trigger regression, BPR pump tests (50% drop end-to-end, cursor coverage, decided-0 skip, Byzantine-silent canary at 50000+ sweeps for pitfall 11, 75%/87.5% drop convergence), EXHAUSTED single-emission + 0xFE sentinel, ProposalValue ACCEPT-gate transition (ECHOED-only → NULL, post-ACCEPT → value, ORIGIN-bit carve-out for self-origin). Reaches into `a->flags & BKR94ACS_F_THRESHOLD`, `a->nDecided`, and the `data[]` layout for setup and assertion. |
| `test_bkr94acs_blackbox` | Protocol black-box (bkr94acs) | Section A: Sz/Init contract, Propose round-trip + idempotency, defensive nulls. Section B: Lemma 2 Parts A/B/C/D explicit at n=4/n=7, identical proposals, multi-byte values, step-2 trigger uses BA-decision count not Fig1-ACCEPT count, single-input-per-BA-per-peer (paper Implementer remark), honest-exclusion contract. Section C: Pump idle on fresh peer, Pump after Propose, MAX_ACTS bound, CommittedFig1Count monotone, silence-quorum signal, 50% drop convergence, silent-Byzantine canary. Section D: BA_EXHAUSTED single emission + sentinel + permanent !complete, Pump continues post-EXHAUSTED. Section E: equivocating proposer (Bracha Lemma 2 inheritance). Tests derived from `bkr94acs.h`, `bracha87.h`, `BKR94ACS.txt`, `Bracha87.txt`, and the bracha87 black-box style — no `.c` reads. |

The white-box / black-box pairing surfaces a different class of bug at each layer. White-box catches internal-invariant regressions (a state-machine flag set wrong, a ledger field unbumped). Black-box catches API contract drift — header text and code behavior pulling apart over time. Recent contract-drift fix caught by the black-box suite: `bkr94acsProposalValue`'s ACCEPT-gate (header documented "0 if not yet accepted" but pre-fix returned ECHOED-stored bytes, exposing pre-Lemma-2 values to callers).

The black-box suites stay strict about scope: only `*.h`, paper-extract `.txt`, and the matching black-box-style sibling are read while writing tests. When a test fails, the contract sources alone determine whether to tighten the code or rewrite the comment.

## Examples

Two runnable examples sit in `example/`, one per application-facing API surface. Each runs in a single process with a synchronous in-memory queue (no loss, no reordering, no asynchrony) — they exercise the protocol state machines and the BPR pump but do **not** exercise the deployment-time termination policies (silence-quorum + K-sweep gate, abandonment) needed under real asynchronous transport.

The low-level Fig 3 and Fig 4 entry points (`bracha87Fig3Accept`, `bracha87Fig4Round`) have no dedicated examples — those layers exist as internal mechanism feeding `bkr94acs`.  Fig 4 (raw single-bit binary BA) has no realistic standalone caller, evidenced by the seven-year gap between Bracha 1987 and BKR94 1994; Fig 3 (the VALID-set framework) exists to feed Fig 4 and inherits the same "no standalone need" through it.  Their behaviour is exercised through `bkr94acs` and through the test suites.

`example/bracha87Fig1.c` — reliable broadcast (Theorem 1). One designated origin broadcasts a multi-byte value; all correct peers either accept the same value or none accept (Lemmas 3 and 4):

```bash
./example_bracha87Fig1 4 1 hello                # 4 peers, 1 Byzantine fault, broadcast "hello"
./example_bracha87Fig1 -s 42 7 2 transactionXYZ # shuffled delivery
./example_bracha87Fig1 -b 2 4 1 hello           # Byzantine origin equivocates (split=2 stalls; split=1 or 3 converges)
./example_bracha87Fig1 -v -o 1 4 1 ping         # verbose trace, peer 1 is origin
```

`example/bkr94acs.c` — multi-value agreement on arbitrary strings:

```bash
./example_bkr94acs 4 1 joe sam sally tim        # 4 peers propose strings
./example_bkr94acs -s 42 4 1 joe sam sally tim  # shuffled delivery (different subset)
./example_bkr94acs 4 0 joe sam sally tim        # t=0: all proposals included
./example_bkr94acs -v 7 2 alpha bravo charlie delta echo foxtrot golf
```

## Deployment Notes

This library is protocol-only. A working deployment needs a transport layer satisfying the three System Model assumptions above, plus identity, keys, a coin source, and a termination policy. The points below are load-bearing; do not "optimize" them away.

### BPR replaces the application-layer ledger

A previous deployment pattern wrapped this library in a per-record ledger: `peerHasPsk[]` / `peerOriginAck[]` / `peerRound[]` evidence tracking, `destMask` per-record bitmaps, a cursor over the record list, per-record `pumpNs` floors. With BPR, that machinery is no longer needed — the library's own `bkr94acsPump` (or `bracha87Fig1Bpr` at the bare-bracha87 layer) is the only mechanism that guarantees eventual delivery, and replay state is intrinsic to the protocol's own committed flags. The application loop is two operations: drain ingress, tick the pump (see the BPR section above for the loop sketch).

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

### Coin choice — caller responsibility

**The library is coin-agnostic.** Both `bracha87Fig4Init` and `bkr94acsInit` take a `bracha87CoinFn` callback plus closure; the caller supplies the coin and owns the consequences of that choice. The bundled `example/bkr94acs.c` uses a **deterministic alternating coin** chosen for reproducible demo runs — the example source explicitly notes this is for demonstration only. This section is reference material to inform the caller's choice.

Fig 4 step 3 case (iii) — when neither decision-count rule fires — calls the coin. The coin is how Bracha escapes FLP impossibility: deterministic asynchronous consensus is impossible, and randomization buys probabilistic termination. Bracha's own Theorem 2 bounds the expected number of phases at O(1) under a **common-coin** assumption; under other coin choices the theoretical bound depends on the choice. (This is the same FLP-escape mechanism as other randomized async BFT protocols; partial-synchrony designs like PBFT/Tendermint/HotStuff escape FLP through timing assumptions instead.) Options the caller may supply via `bracha87CoinFn`:

- **Common coin** (same value across all peers per phase): a verifiable random beacon, a distributed coin protocol, or a threshold-signature-based scheme. Brings additional setup (DKG, threshold keys, etc.) that the library does not require for any other reason.
- **Local coin** (each peer flips independently): e.g. `arc4random_buf` per peer. The simplest adversarial-safe option; no shared-randomness infrastructure required. Termination is provably constant in expectation when `t = O(√n)` (Ben-Or 1983 Theorem 3, "Another Advantage of Free Choice (Extended Abstract): Completely Asynchronous Agreement Protocols," PODC '83); outside that regime the theoretical bound degrades toward `O(2^(n-t-1))`.
- **Deterministic coin** (e.g. `phase & 1`): zero entropy under an adversarial scheduler. Useful for reproducible tests and for non-adversarial deployments — used by the bundled examples for demo reproducibility, not safe under an adaptive adversary.

A slow-converging coin drives Fig 4 step 3 case (iii) ties round after round, advancing the phase counter toward the encoding-imposed `maxPhases=85` ceiling (Operational Limits). Hitting that ceiling raises `BRACHA87_EXHAUSTED`, which is fatal at the BKR94 layer — Lemma 2 Part C admits no unilateral substitute, so the local epoch must abort (Implementation Note 12).

### No timing in the protocol

The protocol's correctness — both safety and eventual termination — depends on no timing assumption; this is the asynchronous-BFT model. Any timing parameters in a deployment (retransmit cadence, silence thresholds, pump tick) govern the transport wrapper and termination policy, not the state machines in this library. Correctness holds under arbitrary asynchrony; termination speed depends on the operator's tuning.

---

## Correctness Audit

The audit story is a four-link chain from paper to running code, with one human inspection step (boundary I/O wiring) and one exhaustive test step (the algorithmic predicates).

```
paper rules            <-> .dtc files                human, rule-by-rule comments
.dtc files              -> compiled dispatch         dtc, exhaustive/exclusive
C wrapper boundary I/O                               human inspection
fig3IsValid, fig4Nfn, Fig 3 cascade                  test/test_predicates.c —
                                                     exhaustive enumeration vs
                                                     paper-direct reference at
                                                     n=4, t=1
```

The decision-table layer (`*.dtc`) is paper vocabulary, rule-by-rule commented with the paper's rule numbers. `dtc` enforces exhaustiveness and exclusivity at compile time and emits depth-optimal dispatch. The C wrapper sits below the dispatch and is one line per boundary input/output — each line is either a flag/count/bit-test mapping or a boolean-to-side-effect; small enough to read.

The two algorithmic predicates that the dispatch delegates to — `fig3IsValid` (recursive existential), `fig4Nfn` (case analysis with permissive D_FLAG encoding) — and the Fig 3 cascade (iterative re-validation) are the only places where search/recursion/iteration sits below the bridge. They are anchored by `test_predicates.c`: 960 `fig4Nfn` inputs, 165 `fig3IsValid` evaluations, 4 cascade delivery permutations, all at n=4 t=1, against a paper-direct subset-enumeration reference. All agree.

`fig3IsValid` is paper-correct **given a caller's N that exposes the existential subset quantifier via `rc > 0`**. The Fig 3 dispatch invokes N once on the full validated set; N's responsibility is to answer "could some n-t subset legitimately produce this value?" If a caller supplies an N whose permissive return is suppressed, `fig3IsValid` correctly rejects values the paper definition would admit via a strict subset. `fig4Nfn` is the canonical N for Fig 4 and exposes the existential analytically; the 960-input correspondence test against paper-direct subset enumeration anchors that delegation. The two predicates verify each other transitively: `fig4Nfn` ↔ paper at all bounded inputs, and `fig3IsValid` ↔ paper *given* that delegation.

## Implementation Notes

Each item below is a paper-vs-code divergence that any from-scratch implementation will encounter. We caught them by reading the paper rule-by-rule against composed-simulation runs and against fair-loss replay; isolation testing missed almost all of them, because the divergences only manifest under multi-figure interaction or under network conditions that simulated reliable channels never produce. They are the cost of building this from the papers — listed here so a porter does not pay it twice, and so a reader evaluating "should I trust this implementation?" can see what was actually verified and what regression test catches each one.

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

12. **Fig 4 EXHAUSTED is fatal at the BKR94 layer; no unilateral substitute.** When `bracha87Fig4Round` returns `BRACHA87_EXHAUSTED` (probabilistic termination did not converge within the unsigned-char round encoding's 85-phase ceiling), the local BA has no decision. BKR94 Lemma 2 Part B's "all BAs terminate" assumption is violated, and Part C (SubSet agreement) is unrecoverable locally — any unilateral substitute (decide 0 or 1) could disagree with another peer's actual decision (different local-coin sequence or message ordering). The library surfaces `BKR94ACS_ACT_BA_EXHAUSTED`, marks the affected origin's BA state as exhausted (`bkr94acsBaDecision(acs, origin)` returns 0xFE thereafter), and does NOT increment `nDecided` or `nDecidedOne` (no decision was made); `BKR94ACS_F_COMPLETE` stays clear in `acs->flags`. The application must abort the local epoch and surface this as a distinct outcome (see Abandonment under Deployment Notes). BPR continues pumping replays for that origin so other peers may still benefit from earlier-round echoes/readys. EXHAUSTED is mutually exclusive with DECIDE per Fig 4 semantics, so single emission is structural — no dedup guard needed. Regression check: `testExhausted`.

---

## Re-Implementing in Another Language

A port that wants to preserve this library's correctness story has two pieces of machinery to either reproduce or replace:

1. The **decision-table compilation pipeline** (described below), which lifts paper rules into depth-optimal dispatch.
2. The **trap list and predicate corpus** for cross-checking the result. Implementation Notes #1–#12 above are paper-vs-code traps; `test/test_predicates.c` is the exhaustive paper-direct reference for the algorithmic predicates that sit below the dispatch.

### Paper-Faithful Dispatch via DTC

Each module's per-call decision logic is captured in a CSV decision table written in the paper's vocabulary (`bracha87Fig{1,3,4}.dtc`, `bkr94acs.dtc`). A small bridge per module (`*ToC.dtc`) maps domain names and values to C identifiers and constants. The decisionTableCompiler (`../decisionTableCompiler/dtc`) co-compiles each pair to an optimal-depth pseudocode dispatch, which a local `psu.awk` translates to a C snippet the entry-point function `#include`s.

| Source | Bridge | Generated snippet | Entry point | Depth |
|--------|--------|-------------------|-------------|-------|
| `bracha87Fig1.dtc` | `bracha87Fig1ToC.dtc` | `bracha87Fig1.c` | `bracha87Fig1Input` | 7 |
| `bracha87Fig2.dtc` | (none — Fig 3 subsumes) | — | — | — |
| `bracha87Fig3.dtc` | `bracha87Fig3ToC.dtc` | `bracha87Fig3.c` | `bracha87Fig3Accept` | 4 |
| `bracha87Fig4.dtc` | `bracha87Fig4ToC.dtc` | `bracha87Fig4.c` | `bracha87Fig4Round` | 6 |
| `bkr94acs.dtc` | `bkr94acsToC.dtc` | `bkr94acsRules.c` | both bkr94acs entry points (one snippet, two `#include`s) | 7 |

`dtc` enforces exhaustiveness and exclusivity of the rules at compile time. Depths are full-optimum; full search confirms each is depth-minimal for its boundary-input set). The C wrapper computes boundary inputs, `#include`s the dispatch, and applies the boolean outputs as side effects in an order that is the API contract (e.g. for Fig 1: `echo` before `ready` before `accept`). See `decisionTableCompiler/README.md` for the bridge mechanism.

A re-implementation that does not want a DTC dependency can transcribe the dispatch by hand from each `.dtc`'s rule table — the `.dtc` files are the readable source of record, and the generated `.c` snippets are large nested `if`/`switch` ladders that a competent developer can read directly. The constraint is that the transcription must preserve exhaustiveness and exclusivity (every input combination has exactly one matching rule), which `dtc` proves at compile time and a hand-port must prove by inspection.

### Where to start

- **`Bracha87.txt`** and **`BKR94ACS.txt`** are the paper extracts. Start here.
- **`bracha87Fig{1,3,4}.dtc`** and **`bkr94acs.dtc`** are the paper-vocabulary decision tables, rule-by-rule commented to the paper. These are the API contract for the dispatch.
- **`test/test_predicates.c`** is the paper-direct reference for `fig3IsValid`, `fig4Nfn`, and the Fig 3 cascade — exhaustive enumeration at n=4, t=1. A port should pass this corpus.
- **`test/test_bracha87.c`** and **`test/test_bkr94acs.c`** are the integration-test corpus, including the regression checks named in Implementation Notes #9–#12.
- **Implementation Notes #1–#12 above** are the traps. Each one names a specific paper-vs-code divergence and (where applicable) the regression test that catches it.

## License

LGPL v3 or later. See `COPYING.LESSER` and `COPYING`.
