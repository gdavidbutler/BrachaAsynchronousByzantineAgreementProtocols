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

These assumptions are not optional — they are load-bearing requirements of every lemma and theorem in the paper. This library is a pure state machine with no I/O. **The caller is responsible for building a transport layer that satisfies all three properties:**

1. **Reliable delivery.** Every message sent between correct processes must eventually arrive. The protocol is asynchronous (no timing assumptions), but it requires liveness: messages cannot be silently dropped. In practice this means retransmission, forward error correction, etc.

2. **No message fabrication.** The transport must not generate messages that were never sent. A Byzantine process may send arbitrary content, but the transport itself must not invent messages. In practice this means authenticated channels.

3. **Sender identification.** The receiver must know which process sent each message, and a Byzantine process must not be able to impersonate a correct one. In practice this means authentication bound to process identity.

Without these guarantees, the protocol's safety and liveness proofs do not hold.

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

## API Overview

### bracha87 Entry Points

| Function | Purpose |
|---|---|
| `bracha87Fig1Sz(n, vLen)` | Compute allocation size for a Fig 1 instance |
| `bracha87Fig1Init(...)` | Initialize a Fig 1 instance |
| `bracha87Fig1Input(f1, type, from, value, out)` | Process one incoming message; returns action count (0-3) |
| `bracha87Fig1Value(f1)` | Retrieve committed value |
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
| `bkr94acsProposalInput(acs, origin, type, from, value, out)` | Process a proposal broadcast message; returns action count |
| `bkr94acsConsensusInput(acs, origin, round, broadcaster, type, from, value, out)` | Process a consensus message; returns action count |
| `bkr94acsComplete(acs)` | Check if all N BAs have decided |
| `bkr94acsSubset(acs, origins)` | Retrieve the decided common subset |
| `bkr94acsProposalValue(acs, origin)` | Retrieve accepted proposal value for an origin |

### Caller Composition Pattern

```c
/* Per process: */
struct bracha87Fig1 *fig1[maxRounds * n];  /* one per (origin, round) */
struct bracha87Fig4 *fig4;                 /* embeds Fig3 */
struct bracha87Fig3 *fig3 = (struct bracha87Fig3 *)fig4->data;
unsigned char nextRound = 0;

/* On incoming message (round, type, from, origin, value): */
f1 = fig1[round * n + origin];  /* rounds are 0-based */
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
        broadcast fig4->value for nextRound as INITIAL to all peers
      if act & BRACHA87_DECIDE:
        deliver fig4->decision
      if act & BRACHA87_EXHAUSTED:
        consensus failed within operational limit
  if ECHO_ALL or READY_ALL:
    cv = bracha87Fig1Value(f1);
    send echo/ready(cv) for this round/origin to all peers
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

## Building

```bash
make            # build tests and example
make check      # run tests
make clean      # remove build artifacts
make clobber    # remove all generated files
```

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

### Post-decide continuation is mandatory

A decided peer must keep broadcasting (Implementation Note 1) so others can reach consensus. Two obvious exit mechanisms are both wrong:

- Exit on `BKR94ACS_ACT_COMPLETE` — violates post-decide continuation; peers deciding last can be stranded.
- Broadcast a "DONE" message and exit on a threshold of receipts — the DONE has no retransmit siblings in the typical ledger model; loss of the initial emission strands peers that never hear it before early completers exit.

The principled alternative is a **progress-silence quorum exit.** Each peer tracks the local tick at which every other peer last advanced its observable state (Fig 4 round, Fig 1 proposal/consensus phase transition, etc.). A peer whose state has not advanced for a chosen silence window is "done-silent." Exit when the local instance is complete AND at least `n-t-1` others are done-silent. The threshold is `n-t-1`, not `n-t` — self is implicit because completion is known locally. Using `n-t` is silently wrong at `t>0` and unreachable at `t=0`.

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
