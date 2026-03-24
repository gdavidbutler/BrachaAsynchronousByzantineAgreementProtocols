# BrachaAsynchronousByzantineAgreementProtocols

A C library implementing all four figures of Gabriel Bracha's 1987 paper as composable pure state machines, plus the Asynchronous Common Subset (ACS) protocol built from them.

## Overview

This is the only known implementation of all four figures of Bracha 1987 as composable pure state machines. ANSI C89, zero dependencies. No I/O, no threads, no dynamic allocation -- the caller provides memory and executes output actions.

Each module boundary matches the paper exactly, so the paper's proofs apply per-module: Lemmas 1-4 to Fig 1, Lemmas 5-7 to Fig 2/3, Lemmas 9-10 and Theorems 1-2 to Fig 4.

The ACS module composes these figures into multi-value agreement: N peers propose arbitrary values, and all honest peers agree on the same common subset of at least n-t proposals. This is the BKR construction (Ben-Or, Kelmer, Rabin 1994).

## The Paper

Gabriel Bracha, "Asynchronous Byzantine Agreement Protocols," *Information and Computation* 75, 130-143 (1987).

`Bracha87.txt` is a companion summary of the paper: figures, rules, VALID set definitions, all lemma/theorem statements, and a mapping from each lemma to the test that verifies it.

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

### Asynchronous Common Subset (acs)

```
N proposals -> N Fig1(n,t,vLen) -> accept -> vote 1 in BA
                                   n-t accepted -> vote 0 in remaining BAs
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

### ACS -- Asynchronous Common Subset

Composes Fig 1 and Fig 4 into multi-value agreement using the BKR construction (Ben-Or, Kelmer, Rabin 1994; also described in Miller et al. 2016 "HoneyBadger BFT").

Each of N peers proposes an arbitrary value (up to vLen+1 bytes). N Fig 1 instances reliably broadcast the proposals. N Fig 4 instances run binary consensus on "include this origin?" When a peer accepts origin j's proposal via Fig 1, it votes 1 in BA_j. When n-t proposals have been accepted, it votes 0 for all remaining BAs. The common subset is {j : BA_j decided 1}, guaranteed to contain at least n-t origins.

Two message classes flow on the network: proposal messages (Fig 1 carrying arbitrary values) and consensus messages (Fig 1 carrying binary values for per-origin BA instances). Consensus messages are routed internally by (origin, round, broadcaster) -- the broadcaster identifies whose Fig 1 broadcast within a consensus round, distinct from the message sender.

The ACS state machine knows its own peer index (self), which the bracha87 figures do not need. This is because ACS manages internal routing: when Fig 4 says BROADCAST, ACS must tag the outgoing INITIAL with self as the broadcaster.

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

### ACS Entry Points

| Function | Purpose |
|---|---|
| `acsSz(n, vLen, maxPhases)` | Compute allocation size for an ACS instance |
| `acsInit(...)` | Initialize with peer index, coin function, and closure |
| `acsProposalInput(acs, origin, type, from, value, out)` | Process a proposal broadcast message; returns action count |
| `acsConsensusInput(acs, origin, round, broadcaster, type, from, value, out)` | Process a consensus message; returns action count |
| `acsComplete(acs)` | Check if all N BAs have decided |
| `acsSubset(acs, origins)` | Retrieve the decided common subset |
| `acsProposalValue(acs, origin)` | Retrieve accepted proposal value for an origin |

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

6. **Subset majority threshold for even n-t.** The step 1 permissive check used `(nt+1)/2` as the threshold. For even n-t (when n=3t+2), integer division made this one too low, causing false permissiveness. Fix: `nt/2+1`. Verified by exhaustive enumeration for n=4..16.

## Building

```bash
make            # build tests and example
make check      # run tests
make clean      # remove build artifacts
make clobber    # remove all generated files
```

The consensus example demonstrates binary consensus (Fig1+Fig3+Fig4):

```bash
./consensus 4 1                          # 4 peers, 1 Byzantine fault
./consensus -s 42 7 2                    # shuffled delivery
./consensus -b 3 7 2                     # Byzantine peer 0 equivocates
./consensus -v 4 1 0 0 1 1              # verbose trace, split initial values
```

The ACS example demonstrates multi-value agreement on arbitrary strings:

```bash
./example_acs 4 1 joe sam sally tim      # 4 peers propose strings
./example_acs -s 42 4 1 joe sam sally tim  # shuffled delivery (different subset)
./example_acs 4 0 joe sam sally tim      # t=0: all proposals included
./example_acs -v 7 2 alpha bravo charlie delta echo foxtrot golf
```

Compiler flags: `-std=c89 -pedantic -Wall -Wextra -Os -g`

## License

LGPL v3 or later. See `COPYING.LESSER` and `COPYING`.
