/*
 * Tests for ct04Dsp.[hc] + ct04Rtv.[hc] + ct04DspRbc.[hc] - CT04
 * AVID-H Disperse + Retrieve + AVID-RBC outer wrap.
 *
 * Simulates n=4, t=1 dispersal and retrieval over both the
 * thrDspRsec (RS-coded) and thrDspSss (Shamir-coded) plugins,
 * plus the §3.7 AVID-RBC outer wrap with a flat-RS encoder and
 * a plain hash list.
 *
 * White-box: drives messages through the library API; inspects
 * struct flags and delivered payloads.  Pairs with the plugin's
 * own roundtrip test (thrDspRsecTest / thrDspSssTest) which
 * covers the plugin contract; this test covers the Disperse +
 * Retrieve protocol on top of the plugin.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ct04Dsp.h"
#include "ct04Rtv.h"
#include "ct04DspRbc.h"
#include "thrDsp.h"
#include "thrDspRsec.h"
#include "thrDspSss.h"
#include "rsecMk.h"
#include "sssMk.h"
#include "rsec.h"
#include "rsecMk.h"
#include "rmd128.h"

static int Fail;

/*************************************************************************/
/*  Plugin / hash wiring                                                 */
/*************************************************************************/

static void *
rmd128Allocate(
  void
){
  return (malloc(rmd128tsize()));
}

/*
 * Convenience: build an rsecMkHsh_t over RMD128.
 */
static void
buildRsecHshRmd(
  rsecMkHsh_t *H
){
  H->a = rmd128Allocate;
  H->i = (void(*)(void *))rmd128init;
  H->u = (void(*)(void *, const unsigned char *, unsigned int))rmd128update;
  H->f = (void(*)(void *, unsigned char *))rmd128final;
  H->d = free;
  H->h = 4;  /* 2^4 = 16 bytes */
}

static void
buildSssHshRmd(
  sssMkHsh_t *H
){
  H->a = rmd128Allocate;
  H->i = (void(*)(void *))rmd128init;
  H->u = (void(*)(void *, const unsigned char *, unsigned int))rmd128update;
  H->f = (void(*)(void *, unsigned char *))rmd128final;
  H->d = free;
  H->h = 4;
}

/* SSS deterministic share-point map: piece-index i -> i + 1
 * (avoid 0 since that's the secret point in this test). */
static unsigned char
sharePt(
  unsigned int i
){
  return ((unsigned char)(i + 1));
}

/* SSS deterministic random for tests: hash the piece-index modulo
 * the request so the output is reproducible across runs. */
static unsigned long SssRandSeed = 0xC0DECAFEu;
static void
detRandBytes(
  void *ctx
 ,unsigned char *buf
 ,unsigned int len
){
  unsigned int i;
  (void)ctx;
  for (i = 0; i < len; ++i) {
    SssRandSeed = SssRandSeed * 1103515245UL + 12345UL;
    buf[i] = (unsigned char)(SssRandSeed >> 16);
  }
}

/*************************************************************************/
/*  Simulator                                                            */
/*************************************************************************/

#define MAX_N      8
#define MAX_MSGS   4096
#define MAX_TICKS  256

struct msgQ {
  unsigned char type;
  unsigned char from;
  unsigned char to;
  unsigned char pieceIdx;
  unsigned int  rootOff;     /* offsets into Blob to fetch payload */
  unsigned int  proofOff;
  unsigned int  pieceOff;
};

static struct msgQ MsgQ[MAX_MSGS];
static unsigned int Qhead;
static unsigned int Qtail;

/* Blob buffer accumulating the byte content of each in-flight
 * message.  Avoids pointer aliasing into instance state since
 * borrowed pointers go stale on the next call. */
static unsigned char Blob[1 << 20];
static unsigned int  BlobLen;

static unsigned int
blobPush(
  const unsigned char *bytes
 ,unsigned int len
){
  unsigned int off;
  off = BlobLen;
  if (BlobLen + len > sizeof (Blob)) {
    fprintf(stderr, "Blob overflow (len=%u, used=%u)\n", len, BlobLen);
    exit(1);
  }
  memcpy(Blob + off, bytes, len);
  BlobLen += len;
  return (off);
}

static void
simReset(
  void
){
  Qhead = Qtail = 0;
  BlobLen = 0;
}

static void
queueAction(
  unsigned char from
 ,unsigned int n
 ,const struct ct04DspAct *a
 ,unsigned int rootSz
 ,unsigned int proofSz
 ,unsigned int pieceSz
){
  unsigned int rootOff;
  unsigned int proofOff;
  unsigned int pieceOff;
  unsigned int to;
  unsigned int firstTo;
  unsigned int lastTo;

  if (a->act != CT04_DSP_ACT_SEND
   && a->act != CT04_DSP_ACT_ECHO
   && a->act != CT04_DSP_ACT_READY)
    return;

  rootOff  = a->root  ? blobPush(a->root,  rootSz)  : 0;
  proofOff = a->proof ? blobPush(a->proof, proofSz) : 0;
  pieceOff = a->piece ? blobPush(a->piece, pieceSz) : 0;

  if (a->act == CT04_DSP_ACT_SEND) {
    firstTo = a->dest;
    lastTo  = a->dest;
  } else {
    firstTo = 0;
    lastTo  = n - 1;
  }

  for (to = firstTo; to <= lastTo; ++to) {
    if (Qtail >= MAX_MSGS) {
      fprintf(stderr, "MsgQ overflow\n");
      exit(1);
    }
    MsgQ[Qtail].type     = (a->act == CT04_DSP_ACT_SEND) ? CT04_DSP_SEND
                          : (a->act == CT04_DSP_ACT_ECHO) ? CT04_DSP_ECHO
                          : CT04_DSP_READY;
    MsgQ[Qtail].from     = from;
    MsgQ[Qtail].to       = (unsigned char)to;
    MsgQ[Qtail].pieceIdx = a->pieceIdx;
    MsgQ[Qtail].rootOff  = rootOff;
    MsgQ[Qtail].proofOff = proofOff;
    MsgQ[Qtail].pieceOff = pieceOff;
    ++Qtail;
  }
}

/*************************************************************************/
/*  Roundtrip test                                                       */
/*************************************************************************/

static int
runRoundtrip(
  const char *label
 ,const struct thrDsp *plugin
 ,unsigned int n
 ,unsigned int t
 ,unsigned int payloadLen
 ,unsigned int silentDealerBit  /* bitmask: peers whose OUTBOUND messages drop */
 ,unsigned int dropSendMask     /* bitmask: dealer SEND to these destinations drops */
){
  struct ct04Disperse *inst[MAX_N];
  struct bracha87Pump pump;
  unsigned long sz;
  unsigned int i;
  unsigned int tick;
  unsigned int pieceSz;
  unsigned int proofSz;
  unsigned int rootSz;
  unsigned int storedCnt;
  unsigned int totalEmitted;
  unsigned char *payload;
  unsigned char *encWork;
  unsigned char *vfWork;
  unsigned char *decWork;
  struct ct04DspAct outActs[CT04_DSP_MAX_ACTS > CT04_DSP_PUMP_MAX_ACTS
                            ? CT04_DSP_MAX_ACTS : CT04_DSP_PUMP_MAX_ACTS];
  struct ct04Disperse *instArr[MAX_N];

  printf("  %s n=%u t=%u L=%u silent=0x%x dropSend=0x%x\n",
   label, n, t, payloadLen, silentDealerBit, dropSendMask);

  pieceSz = plugin->vt->pieceSz(plugin, payloadLen, n, t);
  proofSz = plugin->vt->proofSz(plugin, n);
  rootSz = plugin->vt->rootSz(plugin);

  sz = ct04DspSz(plugin, (unsigned int)(n - 1), t, payloadLen);
  payload = malloc(payloadLen);
  encWork = malloc(plugin->vt->encWaSz(plugin, n, t, payloadLen) + 1);
  vfWork = malloc(plugin->vt->vfWaSz(plugin) + 1);
  decWork = malloc(plugin->vt->derivedRootWaSz(plugin, n, t, payloadLen) + 1);
  if (!payload || !encWork || !vfWork || !decWork) {
    fprintf(stderr, "    malloc\n");
    return (1);
  }
  for (i = 0; i < payloadLen; ++i)
    payload[i] = (unsigned char)((i * 131 + 7) & 0xff);

  for (i = 0; i < n; ++i) {
    inst[i] = malloc(sz);
    if (!inst[i]) {
      fprintf(stderr, "    malloc inst\n");
      return (1);
    }
    ct04DspInit(inst[i], plugin, (unsigned char)(n - 1), (unsigned char)t,
     (unsigned char)i, payloadLen);
  }

  /* Peer 0 is the dealer. */
  if (ct04DspOrigin(inst[0], payload, encWork)) {
    printf("    ct04DspOrigin: FAIL\n");
    return (1);
  }

  bracha87PumpInit(&pump);
  for (i = 0; i < n; ++i)
    instArr[i] = inst[i];

  simReset();

  /* Per-peer cursor; pump must persist across ticks so sendDest
   * actually walks the destination ring. */
  {
    struct bracha87Pump peerPumps[MAX_N];
    for (i = 0; i < n; ++i)
      bracha87PumpInit(&peerPumps[i]);


  /* Tick loop: each tick walks pump for one instance, then drains
   * all queued messages.  Repeat until no progress. */
  totalEmitted = 0;
  storedCnt = 0;
  for (tick = 0; tick < MAX_TICKS && storedCnt < n; ++tick) {
    unsigned int npump;
    unsigned int p;

    /*
     * Pump phase: each peer ticks once.  Peer p that the silent
     * bitmask names drops its outbound messages (the queueAction
     * call is skipped). */
    for (p = 0; p < n; ++p) {
      npump = ct04DspBpr(inst[p], &peerPumps[p], outActs);
      if (silentDealerBit & (1u << p))
        continue;
      for (i = 0; i < npump; ++i) {
        /* Dealer SEND to a dropped destination is lost on the wire. */
        if (p == 0
         && outActs[i].act == CT04_DSP_ACT_SEND
         && (dropSendMask & (1u << outActs[i].dest)))
          continue;
        queueAction((unsigned char)p, n, &outActs[i], rootSz, proofSz, pieceSz);
        ++totalEmitted;
      }
    }

    /* Drain queue for this tick. */
    while (Qhead < Qtail) {
      struct msgQ *m = &MsgQ[Qhead++];
      const unsigned char *root  = Blob + m->rootOff;
      const unsigned char *proof = Blob + m->proofOff;
      const unsigned char *piece = Blob + m->pieceOff;
      unsigned int n_acts;
      unsigned int a;

      n_acts = ct04DspInput(inst[m->to], m->type, m->from, m->pieceIdx,
       root, proof, piece, vfWork, decWork, outActs);
      for (a = 0; a < n_acts; ++a) {
        if (outActs[a].act == CT04_DSP_ACT_STORED) {
          /* count this peer as done if not already */
        } else if (outActs[a].act == CT04_DSP_ACT_ABORT) {
          printf("    peer %u ABORTED unexpectedly\n", m->to);
          return (1);
        } else if (silentDealerBit & (1u << m->to)) {
          /* peer m->to is outbound-silent; drop its emissions */
        } else {
          queueAction(m->to, n, &outActs[a], rootSz, proofSz, pieceSz);
          ++totalEmitted;
        }
      }
    }

    /* Count stored peers. */
    storedCnt = 0;
    for (p = 0; p < n; ++p)
      if (ct04DspStored(inst[p]))
        ++storedCnt;
  }
  }  /* end per-peer pump scope */

  if (storedCnt != n) {
    printf("    convergence FAIL: storedCnt=%u/%u after %u ticks (totalEmitted=%u)\n",
     storedCnt, n, tick, totalEmitted);
    Fail = 1;
    goto cleanup;
  }

  /* Verify every peer's stored F-bar_self verifies against its own
   * pieceIdx and the committed root. */
  for (i = 0; i < n; ++i) {
    const unsigned char *piece = ct04DspStored(inst[i]);
    const unsigned char *proof = ct04DspStoredProof(inst[i]);
    const unsigned char *root = ct04DspRoot(inst[i]);
    if (!piece || !proof || !root) {
      printf("    peer %u: missing stored data\n", i);
      Fail = 1;
      continue;
    }
    if (plugin->vt->verify(plugin, i, n, t, payloadLen,
     piece, proof, root, vfWork)) {
      printf("    peer %u: stored piece fails verify\n", i);
      Fail = 1;
    }
  }
  /* All peers' committed roots agree. */
  for (i = 1; i < n; ++i) {
    if (memcmp(ct04DspRoot(inst[0]), ct04DspRoot(inst[i]), rootSz) != 0) {
      printf("    peer 0 vs %u: root mismatch\n", i);
      Fail = 1;
    }
  }

  /* Retrieve roundtrip: client collects k blocks from arbitrary
   * peers, decodes. */
  {
    struct ct04RtvClient *cli;
    unsigned long cliSz;
    unsigned int k;
    struct ct04RtvServerResp resp;
    struct ct04RtvClientAct rcact;
    unsigned int nrtv;
    unsigned char *cliDecWork;

    k = plugin->vt->threshold(n, t);
    cliSz = ct04RtvClientSz(plugin, (unsigned int)(n - 1), t, payloadLen);
    cli = malloc(cliSz);
    cliDecWork = malloc(plugin->vt->decWaSz(plugin, n, t, payloadLen) + 1);
    if (!cli || !cliDecWork) {
      fprintf(stderr, "    rtv malloc\n");
      return (1);
    }
    ct04RtvClientInit(cli, plugin, (unsigned char)(n - 1), (unsigned char)t,
     payloadLen);

    nrtv = 0;
    rcact.act = 0;
    for (i = 0; i < n; ++i) {
      if (!ct04RtvServerRespond(inst[i], &resp))
        continue;
      nrtv = ct04RtvClientInput(cli, (unsigned char)i, resp.pieceIdx,
       resp.root, resp.proof, resp.piece,
       vfWork, cliDecWork, &rcact);
      if (nrtv && rcact.act == CT04_RTV_ACT_RETRIEVED)
        break;
    }
    if (rcact.act != CT04_RTV_ACT_RETRIEVED) {
      printf("    retrieve: never reached RETRIEVED (k=%u)\n", k);
      Fail = 1;
    } else if (memcmp(rcact.payload, payload, payloadLen) != 0) {
      printf("    retrieve: payload mismatch\n");
      Fail = 1;
    }
    free(cli);
    free(cliDecWork);
  }

cleanup:
  for (i = 0; i < n; ++i)
    free(inst[i]);
  free(payload);
  free(encWork);
  free(vfWork);
  free(decWork);
  return (Fail);
}

/*************************************************************************/
/*  Q5 regression: abort doesn't retire BPR echo / ready replay          */
/*                                                                       */
/*  CT04 inventory Q5 (correctness-over-cheapness): an honest server     */
/*  that fires output (abort) via R2b/R3b - decode-and-verify-all-       */
/*  pieces failure on a Byzantine dealer's inconsistent commitment -     */
/*  must continue to replay (echo, FP_i, F_i) and (ready, ...) on        */
/*  every Bpr call.  The committed echo was about (F_i, FP_i) bound to   */
/*  the agreed root, which is independent of the dealer-level commitment */
/*  failure that triggered abort.  Other peers' echo / ready counters    */
/*  legitimately consume the replay.                                     */
/*                                                                       */
/*  This regression constructs a Byzantine dealer via the same           */
/*  Frankenstein technique as thrDspRsecTest's runFrankenstein: encode   */
/*  payloads A and B, splice the first k pieces from A with the rest     */
/*  from B, build a Merkle tree over the spliced leaves so each piece    */
/*  individually verifies, but interpolation from any k recovers a       */
/*  polynomial whose re-derived root disagrees with the committed        */
/*  Frankenstein root - the wrapper aborts.  After abort, ct04DspBpr     */
/*  must still emit ECHO replay for every peer that committed F_ECHOED.  */
/*                                                                       */
/*  rsec-only: the test builds the Frankenstein Merkle tree directly     */
/*  via rsecMkHash / rsecMkProof.  The SSS adapter does not change the   */
/*  abort-vs-replay semantics; the rsec test is sufficient regression    */
/*  for the wrapper's BPR retention rule.                                */
/*                                                                       */
/*  Q7 scope note: testing "originator's ORIGIN does not retire on       */
/*  self-abort" needs the test to inject a Byzantine-committed origin    */
/*  state directly into peer 0's pendingPc / pendingPr / rootCmt - the   */
/*  ct04DspOrigin entry point only accepts honest encodings.  The Q5    */
/*  retention rule is the load-bearing one for liveness (aborted peers   */
/*  helping slow peers cross threshold); Q7 is symmetric in spirit but   */
/*  rarer in practice (a Byzantine dealer would not typically aim to     */
/*  self-abort) and is left for a follow-up that exposes a private-      */
/*  state injection hook or includes ct04Dsp.c directly the way          */
/*  test_predicates.c includes bracha87.c.                               */
/*************************************************************************/

static int
runAbortContinueReplay(
  const char *label
 ,const struct thrDsp *plugin
 ,const rsecMkHsh_t *h
 ,unsigned int n
 ,unsigned int t
 ,unsigned int payloadLen
){
  struct ct04Disperse *inst[MAX_N];
  unsigned long sz;
  unsigned int k;
  unsigned int pieceSz;
  unsigned int proofSz;
  unsigned int rootSz;
  unsigned int treeWa;
  unsigned int encWa;
  unsigned int vfWa;
  unsigned int derWa;
  unsigned int i;
  unsigned int aborted;
  unsigned int replayCount;
  unsigned char *payloadA;
  unsigned char *payloadB;
  unsigned char **piecesA;
  unsigned char **piecesB;
  unsigned char **proofsTmp;
  const unsigned char **frankenLeaves;
  unsigned char **frankenProofs;
  unsigned char *rootA;
  unsigned char *rootB;
  unsigned char *frankenRoot;
  unsigned char *encWork;
  unsigned char *mkWork;
  unsigned char *vfWork;
  unsigned char *decWork;
  unsigned char *fr;
  struct ct04DspAct outActs[CT04_DSP_MAX_ACTS > CT04_DSP_PUMP_MAX_ACTS
                            ? CT04_DSP_MAX_ACTS : CT04_DSP_PUMP_MAX_ACTS];
  int ok;

  printf("  %s frankenstein n=%u t=%u L=%u\n", label, n, t, payloadLen);

  k = plugin->vt->threshold(n, t);
  pieceSz = plugin->vt->pieceSz(plugin, payloadLen, n, t);
  proofSz = plugin->vt->proofSz(plugin, n);
  rootSz = plugin->vt->rootSz(plugin);
  encWa = plugin->vt->encWaSz(plugin, n, t, payloadLen);
  vfWa = plugin->vt->vfWaSz(plugin);
  derWa = plugin->vt->derivedRootWaSz(plugin, n, t, payloadLen);
  treeWa = rsecMkWaSz(h->h, n);
  if (n <= k) {
    printf("    skip: need n > k\n");
    return (0);
  }

  /* --- Build the Frankenstein scenario via rsec primitives. --- */
  payloadA = malloc(payloadLen);
  payloadB = malloc(payloadLen);
  piecesA = malloc(n * sizeof (*piecesA));
  piecesB = malloc(n * sizeof (*piecesB));
  proofsTmp = malloc(n * sizeof (*proofsTmp));
  frankenLeaves = malloc(n * sizeof (*frankenLeaves));
  frankenProofs = malloc(n * sizeof (*frankenProofs));
  rootA = malloc(rootSz);
  rootB = malloc(rootSz);
  frankenRoot = malloc(rootSz);
  encWork = malloc(encWa ? encWa : 1);
  mkWork = malloc(treeWa ? treeWa : 1);
  vfWork = malloc(vfWa ? vfWa : 1);
  decWork = malloc(derWa ? derWa : 1);
  for (i = 0; i < n; ++i) {
    piecesA[i] = malloc(pieceSz);
    piecesB[i] = malloc(pieceSz);
    proofsTmp[i] = malloc(proofSz ? proofSz : 1);
    frankenProofs[i] = malloc(proofSz ? proofSz : 1);
  }
  for (i = 0; i < payloadLen; ++i) {
    payloadA[i] = (unsigned char)((i * 131 + 7) & 0xff);
    payloadB[i] = (unsigned char)((i * 251 + 19) & 0xff);
  }

  if (plugin->vt->encode(plugin, payloadA, payloadLen, n, t,
   piecesA, proofsTmp, rootA, encWork)
   || plugin->vt->encode(plugin, payloadB, payloadLen, n, t,
   piecesB, proofsTmp, rootB, encWork)) {
    printf("    encode(A/B): FAIL\n");
    return (1);
  }

  for (i = 0; i < k; ++i)
    frankenLeaves[i] = piecesA[i];
  for (i = k; i < n; ++i)
    frankenLeaves[i] = piecesB[i];

  fr = rsecMkHash(h, frankenLeaves, pieceSz, n, mkWork);
  if (!fr) {
    printf("    rsecMkHash: FAIL\n");
    return (1);
  }
  memcpy(frankenRoot, fr, rootSz);
  for (i = 0; i < n; ++i)
    if (!rsecMkProof(h, n, i, mkWork, frankenProofs[i])) {
      printf("    rsecMkProof[%u]: FAIL\n", i);
      return (1);
    }

  /* Sanity: per-piece verify against frankenRoot passes (Byzantine
   * dealer's commitment looks valid wire-by-wire). */
  for (i = 0; i < n; ++i)
    if (plugin->vt->verify(plugin, i, n, t, payloadLen,
     frankenLeaves[i], frankenProofs[i], frankenRoot, vfWork)) {
      printf("    sanity verify[%u]: FAIL (Frankenstein piece should pass)\n", i);
      return (1);
    }

  /* --- Stand up n ct04Disperse honest-server instances. --- */
  sz = ct04DspSz(plugin, n - 1, t, payloadLen);
  for (i = 0; i < n; ++i) {
    inst[i] = malloc(sz);
    if (!inst[i]) {
      fprintf(stderr, "    malloc inst\n");
      return (1);
    }
    ct04DspInit(inst[i], plugin, (unsigned char)(n - 1), (unsigned char)t,
     (unsigned char)i, payloadLen);
  }

  /* --- Inject Byzantine SENDs from "dealer 0" to each peer. --- */
  simReset();
  for (i = 0; i < n; ++i) {
    unsigned int na;
    unsigned int a;
    na = ct04DspInput(inst[i], CT04_DSP_SEND, 0, (unsigned char)i,
     frankenRoot, frankenProofs[i], frankenLeaves[i],
     vfWork, decWork, outActs);
    for (a = 0; a < na; ++a)
      queueAction((unsigned char)i, n, &outActs[a], rootSz, proofSz, pieceSz);
  }

  /* --- Drain the ECHO cascade.  At threshold, R2a's interpolation
   * gate fires; the Frankenstein scenario forces decodeOk = no; R2b
   * fires output (abort). --- */
  while (Qhead < Qtail) {
    struct msgQ *m = &MsgQ[Qhead++];
    const unsigned char *root  = Blob + m->rootOff;
    const unsigned char *proof = Blob + m->proofOff;
    const unsigned char *piece = Blob + m->pieceOff;
    unsigned int na;
    unsigned int a;
    na = ct04DspInput(inst[m->to], m->type, m->from, m->pieceIdx,
     root, proof, piece, vfWork, decWork, outActs);
    for (a = 0; a < na; ++a) {
      /* Don't propagate ABORT / STORED; queue ECHO / READY for further
       * delivery so the abort cascade can propagate. */
      if (outActs[a].act != CT04_DSP_ACT_STORED
       && outActs[a].act != CT04_DSP_ACT_ABORT)
        queueAction(m->to, n, &outActs[a], rootSz, proofSz, pieceSz);
    }
  }

  /* --- All peers should have aborted (F_TERMINATED), echoed
   * (F_ECHOED), and NOT sent ready (R2a's send-ready and R2b's abort
   * are mutually exclusive per the Frankenstein scenario). --- */
  ok = 1;
  aborted = 0;
  for (i = 0; i < n; ++i) {
    if (inst[i]->flags & CT04_DSP_F_TERMINATED) ++aborted;
    if (!(inst[i]->flags & CT04_DSP_F_ECHOED)) {
      printf("    peer %u: F_ECHOED not set after cascade\n", i);
      ok = 0;
    }
    if (inst[i]->flags & CT04_DSP_F_RDSENT) {
      printf("    peer %u: F_RDSENT set unexpectedly\n", i);
      ok = 0;
    }
  }
  if (aborted != n) {
    printf("    aborted=%u/%u (expected n)\n", aborted, n);
    ok = 0;
  }

  /* --- Q5 regression: ct04DspBpr on each terminated+echoed instance
   * still emits exactly one ECHO replay action (no SEND - not the
   * dealer; no READY - F_RDSENT clear). --- */
  replayCount = 0;
  for (i = 0; i < n; ++i) {
    struct bracha87Pump p;
    unsigned int npump;
    unsigned int a;
    int sawEcho;
    bracha87PumpInit(&p);
    npump = ct04DspBpr(inst[i], &p, outActs);
    sawEcho = 0;
    for (a = 0; a < npump; ++a) {
      if (outActs[a].act == CT04_DSP_ACT_ECHO) {
        sawEcho = 1;
        if (memcmp(outActs[a].piece, frankenLeaves[i], pieceSz) != 0) {
          printf("    peer %u: replay echo piece != original\n", i);
          ok = 0;
        }
        if (memcmp(outActs[a].proof, frankenProofs[i], proofSz) != 0) {
          printf("    peer %u: replay echo proof != original\n", i);
          ok = 0;
        }
        if (memcmp(outActs[a].root, frankenRoot, rootSz) != 0) {
          printf("    peer %u: replay echo root != committed\n", i);
          ok = 0;
        }
      }
      if (outActs[a].act == CT04_DSP_ACT_READY) {
        printf("    peer %u: unexpected READY replay (RDSENT clear)\n", i);
        ok = 0;
      }
      if (outActs[a].act == CT04_DSP_ACT_SEND) {
        printf("    peer %u: unexpected SEND replay (not dealer)\n", i);
        ok = 0;
      }
    }
    if (!sawEcho) {
      printf("    peer %u: ECHO replay NOT emitted after abort (Q5 regression)\n", i);
      ok = 0;
    } else {
      ++replayCount;
    }
  }
  if (ok)
    printf("    abort-but-continue-echo: PASS (replays=%u/%u)\n",
     replayCount, n);
  else
    Fail = 1;

  for (i = 0; i < n; ++i) {
    free(piecesA[i]);
    free(piecesB[i]);
    free(proofsTmp[i]);
    free(frankenProofs[i]);
    free(inst[i]);
  }
  free(piecesA);
  free(piecesB);
  free(proofsTmp);
  free(frankenLeaves);
  free(frankenProofs);
  free(payloadA);
  free(payloadB);
  free(rootA);
  free(rootB);
  free(frankenRoot);
  free(encWork);
  free(mkWork);
  free(vfWork);
  free(decWork);
  return (ok ? 0 : 1);
}

/*************************************************************************/
/*  AVID-RBC outer wrap test                                             */
/*************************************************************************/

/* Caller-supplied (n-t, n) RS encoder using flat rsecEncode (no
 * Merkle tree, since the outer wrap stores hashes flat).  Encodes
 * the first kOuter shards as systematic, the rest as parity.  Pads
 * the input with zeros to a multiple of kOuter * shardSz. */
static int
rbcRsecEncode(
  void *ctx
 ,const unsigned char *file
 ,unsigned int fileLen
 ,unsigned int n
 ,unsigned int t
 ,unsigned char *const *blocks
 ,unsigned int blockSz
){
  unsigned int kOuter;
  unsigned int m;
  unsigned int i;
  unsigned int copyLen;
  unsigned int padOff;
  unsigned char **dataPtrs;
  unsigned char **parityPtrs;
  static unsigned char Pad[8192];

  (void)ctx;
  if (n <= t)
    return (-1);
  kOuter = n - t;
  m = t;
  if (kOuter > sizeof (Pad) / 1) {
    fprintf(stderr, "rbcRsecEncode kOuter too large\n");
    return (-1);
  }

  dataPtrs = (unsigned char **)malloc(n * sizeof (*dataPtrs));
  parityPtrs = (unsigned char **)malloc(n * sizeof (*parityPtrs));
  if (!dataPtrs || !parityPtrs) {
    free(dataPtrs);
    free(parityPtrs);
    return (-1);
  }
  for (i = 0; i < kOuter; ++i)
    dataPtrs[i] = blocks[i];
  for (i = 0; i < m; ++i)
    parityPtrs[i] = blocks[kOuter + i];

  /* Copy systematic data shards from file. */
  padOff = 0;
  for (i = 0; i < kOuter; ++i) {
    copyLen = blockSz;
    if (padOff >= fileLen)
      copyLen = 0;
    else if (padOff + copyLen > fileLen)
      copyLen = fileLen - padOff;
    if (copyLen)
      memcpy(blocks[i], file + padOff, copyLen);
    if (copyLen < blockSz)
      memset(blocks[i] + copyLen, 0, blockSz - copyLen);
    padOff += blockSz;
  }

  if (rsecEncode((const unsigned char *const *)dataPtrs, parityPtrs,
   blockSz, kOuter, m)) {
    free(dataPtrs);
    free(parityPtrs);
    return (-1);
  }

  free(dataPtrs);
  free(parityPtrs);
  return (0);
}

/* Plain RMD128 callback. */
static void
rbcRmdHash(
  void *ctx
 ,const unsigned char *data
 ,unsigned int len
 ,unsigned char *out
){
  void *h;
  (void)ctx;
  h = malloc(rmd128tsize());
  if (!h) {
    memset(out, 0, 16);
    return;
  }
  rmd128init(h);
  rmd128update(h, data, len);
  rmd128final(h, out);
  free(h);
}

static int
runRbcRoundtrip(
  unsigned int n
 ,unsigned int t
 ,unsigned int fileLen
){
  struct ct04DspRbc *r;
  unsigned long sz;
  unsigned int hashSz;
  unsigned int i;
  unsigned char *file;
  struct ct04DspRbcAct out;

  hashSz = 16;
  sz = ct04DspRbcSz(n - 1, t, fileLen, hashSz);
  printf("  rbc outer n=%u t=%u L=%u sz=%lu\n", n, t, fileLen, sz);

  r = malloc(sz);
  file = malloc(fileLen);
  if (!r || !file) {
    fprintf(stderr, "  malloc\n");
    return (1);
  }
  for (i = 0; i < fileLen; ++i)
    file[i] = (unsigned char)((i * 17 + 3) & 0xff);

  ct04DspRbcInit(r, rbcRsecEncode, 0, rbcRmdHash, 0,
   (unsigned char)(n - 1), (unsigned char)t,
   2 /* self */, fileLen, hashSz);

  if (!ct04DspRbcDeliver(r, file, &out)) {
    printf("    rbc deliver FAIL\n");
    free(r); free(file);
    Fail = 1;
    return (1);
  }
  if (out.act != CT04_DSP_RBC_ACT_STORED) {
    printf("    rbc deliver bad act %u\n", out.act);
    Fail = 1;
  }
  if (!ct04DspRbcBlock(r) || !ct04DspRbcHashList(r)) {
    printf("    rbc accessors return 0 after STORED\n");
    Fail = 1;
  }
  /* Idempotence: second Deliver returns 0. */
  if (ct04DspRbcDeliver(r, file, &out)) {
    printf("    rbc idempotence FAIL (second deliver emitted)\n");
    Fail = 1;
  }

  free(r);
  free(file);
  return (0);
}

/*************************************************************************/
/*  Main                                                                 */
/*************************************************************************/

int
main(
  void
){
  rsecMkHsh_t Hrsec;
  sssMkHsh_t Hsss;
  struct thrDspRsecCfg cfgRsec;
  struct thrDspSssCfg cfgSss;
  struct thrDsp dRsec;
  struct thrDsp dSss;

  buildRsecHshRmd(&Hrsec);
  buildSssHshRmd(&Hsss);
  cfgRsec.h = &Hrsec;
  cfgSss.h = &Hsss;
  cfgSss.sharePt = sharePt;
  cfgSss.randBytes = detRandBytes;
  cfgSss.randCtx = 0;
  cfgSss.secretPoint = 0;
  if (thrDspRsecInit(&dRsec, &cfgRsec)) {
    fprintf(stderr, "thrDspRsecInit\n");
    return (1);
  }
  if (thrDspSssInit(&dSss, &cfgSss)) {
    fprintf(stderr, "thrDspSssInit\n");
    return (1);
  }

  printf("== ct04Dsp roundtrip: rsec (n=4, t=1) ==\n");
  runRoundtrip("rsec/rmd", &dRsec, 4, 1, 64, 0, 0);

  printf("\n== ct04Dsp roundtrip: sss (n=4, t=1) ==\n");
  runRoundtrip("sss/rmd",  &dSss,  4, 1, 64, 0, 0);

  printf("\n== ct04Dsp roundtrip: rsec (n=7, t=2, payload=1000) ==\n");
  runRoundtrip("rsec/rmd", &dRsec, 7, 2, 1000, 0, 0);

  /*
   * SEND-loss regression (CT04 inventory Q4 analogue): dealer SEND
   * to peer N-1 is dropped.  Tests that the lost-SEND server
   * reaches STORED by interpolating F-bar_self from the polynomial
   * (post thrDsp.h piece-extraction extension).  Pre-extension this
   * test would have failed at the verify step on peer N-1's stored
   * piece (empty pieceSelf shipped with a re-derived proof).
   */
  printf("\n== ct04Dsp SEND-loss to peer 3 (rsec, n=4, t=1) ==\n");
  runRoundtrip("rsec/rmd", &dRsec, 4, 1, 64, 0, 1u << 3);

  printf("\n== ct04Dsp SEND-loss to peer 3 (sss, n=4, t=1) ==\n");
  runRoundtrip("sss/rmd",  &dSss,  4, 1, 64, 0, 1u << 3);

  printf("\n== ct04Dsp SEND-loss to peers 5,6 (rsec, n=7, t=2) ==\n");
  runRoundtrip("rsec/rmd", &dRsec, 7, 2, 1000, 0,
   (1u << 5) | (1u << 6));

  printf("\n== ct04Dsp abort-but-continue-echo (Q5 regression, rsec) ==\n");
  runAbortContinueReplay("rsec/rmd", &dRsec, &Hrsec, 4, 1, 64);
  runAbortContinueReplay("rsec/rmd", &dRsec, &Hrsec, 7, 2, 1000);

  printf("\n== ct04DspRbc outer wrap ==\n");
  runRbcRoundtrip(4, 1, 128);
  runRbcRoundtrip(7, 2, 1000);

  thrDspRsecFini(&dRsec);
  thrDspSssFini(&dSss);

  printf("\nAll tests completed%s.\n", Fail ? " with FAILURES" : "");
  return (Fail);
}
