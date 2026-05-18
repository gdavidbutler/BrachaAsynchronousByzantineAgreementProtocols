/*
 * Q7 regression for ct04Dsp: originator's ORIGIN does not retire on
 * self-abort (CT04 inventory Q7, resolved correctness-over-cheapness).
 *
 * Constructs a Byzantine originator state directly inside a
 * ct04Disperse instance - the public ct04DspOrigin entry point only
 * accepts honest plugin encodings, so this test reaches the private
 * D_PENDINGPC / D_PENDINGPR / D_ROOTCMT layout macros by #include'ing
 * ct04Dsp.c into this translation unit (same pattern as
 * test_predicates.c <- bracha87.c).  The test binary is linked
 * WITHOUT ct04Dsp.o to avoid duplicate symbol definitions.
 *
 * Scenario.  Frankenstein splice (same technique as Q5 and
 * thrDspRsecTest::runFrankenstein): encode payloads A and B, splice
 * pieces[i] = piecesA[i] for i < k, piecesB[i] otherwise; compute
 * Merkle root + proofs over the splice.  Inject this Byzantine
 * commitment into peer 0's pendingPc / pendingPr / rootCmt and set
 * F_ORIGIN | F_HAVEROOT.  Drive peer 0's own input cascade (loopback
 * SEND from self, plus injected ECHOes from peers 1..n-1 carrying
 * the Frankenstein leaves).  When e_D crosses the interpolation
 * threshold, doInterpolate's derived root != committed Frankenstein
 * root, so R2b fires output (abort).
 *
 * Verify:
 *   - F_TERMINATED set (abort fired)
 *   - F_ORIGIN still set (Q7 claim - NOT retired by F_TERMINATED)
 *   - F_ECHOED set (R1 fired on the loopback SEND)
 *   - ct04DspBpr emits an ACT_SEND (Q7) carrying frankenLeaves[sendDest]
 *     and an ACT_ECHO (Q5 retention applied to the originator)
 *   - across n successive Bpr calls, sendDest walks all n destinations
 *     and SEND replay never stops while F_ORIGIN holds
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Bring the private ct04Dsp implementation (and its layout macros)
 * into this translation unit.  Same precedent as test_predicates.c. */
#include "ct04Dsp.c"

#include "thrDspRsec.h"
#include "rsecMk.h"
#include "rsec.h"
#include "rmd128.h"

static int Fail;

static void *
rmd128Allocate(
  void
){
  return (malloc(rmd128tsize()));
}

static int
runQ7(
  const struct thrDsp *plugin
 ,const rsecMkHsh_t *h
 ,unsigned int n
 ,unsigned int t
 ,unsigned int payloadLen
){
  struct ct04Disperse *d;
  unsigned long sz;
  unsigned int k;
  unsigned int pieceSz;
  unsigned int proofSz;
  unsigned int rootSz;
  unsigned int encWa;
  unsigned int vfWa;
  unsigned int decWa;
  unsigned int treeWa;
  unsigned int i;
  unsigned int j;
  unsigned int totalActs;
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
  struct ct04DspAct out[CT04_DSP_MAX_ACTS > CT04_DSP_PUMP_MAX_ACTS
                        ? CT04_DSP_MAX_ACTS : CT04_DSP_PUMP_MAX_ACTS];
  struct bracha87Pump pump;
  unsigned char *seenDest;
  int ok;

  printf("  Q7 self-abort retains ORIGIN  n=%u t=%u L=%u\n", n, t, payloadLen);
  ok = 1;

  k = plugin->vt->threshold(n, t);
  pieceSz = plugin->vt->pieceSz(plugin, payloadLen, n, t);
  proofSz = plugin->vt->proofSz(plugin, n);
  rootSz = plugin->vt->rootSz(plugin);
  encWa = plugin->vt->encWaSz(plugin, n, t, payloadLen);
  vfWa = plugin->vt->vfWaSz(plugin);
  decWa = plugin->vt->derivedRootWaSz(plugin, n, t, payloadLen);
  treeWa = rsecMkWaSz(h->h, n);
  if (n <= k) {
    printf("    skip: need n > k\n");
    return (0);
  }

  /* --- Build the Frankenstein splice. --- */
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
  decWork = malloc(decWa ? decWa : 1);
  seenDest = malloc(n);
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

  /* --- Init dealer instance and inject Byzantine ORIGIN state. --- */
  sz = ct04DspSz(plugin, n - 1, t, payloadLen);
  d = malloc(sz);
  if (!d) {
    fprintf(stderr, "    malloc inst\n");
    return (1);
  }
  ct04DspInit(d, plugin, (unsigned char)(n - 1), (unsigned char)t,
   0 /* self = 0 (dealer) */, payloadLen);

  /* Reach into private layout (made visible by including ct04Dsp.c). */
  for (i = 0; i < n; ++i) {
    memcpy(D_PENDINGPC(d) + (unsigned long)i * pieceSz,
     frankenLeaves[i], pieceSz);
    memcpy(D_PENDINGPR(d) + (unsigned long)i * proofSz,
     frankenProofs[i], proofSz);
  }
  memcpy(D_ROOTCMT(d), frankenRoot, rootSz);
  d->flags |= CT04_DSP_F_ORIGIN | CT04_DSP_F_HAVEROOT;

  /* --- Simulate loopback SEND from self at peer 0 (R1 fires). --- */
  totalActs = ct04DspInput(d, CT04_DSP_SEND, 0, 0,
   frankenRoot, frankenProofs[0], frankenLeaves[0],
   vfWork, decWork, out);
  if (!totalActs || out[0].act != CT04_DSP_ACT_ECHO) {
    printf("    loopback SEND: expected ECHO emission, got act=%u (n=%u)\n",
     totalActs ? out[0].act : 0, totalActs);
    ok = 0;
  }
  if (!(d->flags & CT04_DSP_F_ECHOED)) {
    printf("    loopback SEND: F_ECHOED not set\n");
    ok = 0;
  }

  /* --- Inject ECHOes from peers 1..n-1 carrying frankenLeaves[j];
   * the threshold-crossing arrival triggers R2a / R2b interpolation.
   * Cross-polynomial pieces (first k from A, rest from B) make
   * derivedRoot != frankenRoot, so R2b fires output (abort). --- */
  for (j = 1; j < n; ++j) {
    unsigned int na;
    unsigned int a;
    na = ct04DspInput(d, CT04_DSP_ECHO, (unsigned char)j, (unsigned char)j,
     frankenRoot, frankenProofs[j], frankenLeaves[j],
     vfWork, decWork, out);
    for (a = 0; a < na; ++a) {
      if (out[a].act == CT04_DSP_ACT_STORED) {
        printf("    unexpected STORED on ECHO from peer %u\n", j);
        ok = 0;
      }
    }
  }

  /* --- Verify post-abort flag state. --- */
  if (!(d->flags & CT04_DSP_F_TERMINATED)) {
    printf("    F_TERMINATED not set (abort did not fire)\n");
    ok = 0;
  }
  if (!(d->flags & CT04_DSP_F_ECHOED)) {
    printf("    F_ECHOED cleared by abort (should persist)\n");
    ok = 0;
  }
  if (!(d->flags & CT04_DSP_F_ORIGIN)) {
    printf("    F_ORIGIN cleared by abort - Q7 regression FAIL\n");
    ok = 0;
  }
  if (!(d->flags & CT04_DSP_F_HAVEROOT)) {
    printf("    F_HAVEROOT cleared by abort\n");
    ok = 0;
  }
  if (d->flags & CT04_DSP_F_RDSENT) {
    printf("    F_RDSENT set unexpectedly (decode failed; never READY-broadcast)\n");
    ok = 0;
  }

  /* --- Q7: ct04DspBpr emits ACT_SEND despite F_TERMINATED. --- */
  bracha87PumpInit(&pump);
  {
    unsigned int na;
    unsigned int a;
    int sawSend;
    int sawEcho;
    na = ct04DspBpr(d, &pump, out);
    sawSend = 0;
    sawEcho = 0;
    for (a = 0; a < na; ++a) {
      if (out[a].act == CT04_DSP_ACT_SEND) {
        sawSend = 1;
        if (out[a].dest != 0) {
          printf("    first SEND replay dest=%u (expected 0)\n", out[a].dest);
          ok = 0;
        }
        if (memcmp(out[a].piece, frankenLeaves[0], pieceSz) != 0) {
          printf("    SEND replay piece != frankenLeaves[0]\n");
          ok = 0;
        }
        if (memcmp(out[a].proof, frankenProofs[0], proofSz) != 0) {
          printf("    SEND replay proof != frankenProofs[0]\n");
          ok = 0;
        }
        if (memcmp(out[a].root, frankenRoot, rootSz) != 0) {
          printf("    SEND replay root != frankenRoot\n");
          ok = 0;
        }
      }
      if (out[a].act == CT04_DSP_ACT_ECHO)
        sawEcho = 1;
    }
    if (!sawSend) {
      printf("    Bpr did not emit SEND despite F_ORIGIN set - Q7 FAIL\n");
      ok = 0;
    }
    if (!sawEcho) {
      printf("    Bpr did not emit ECHO despite F_ECHOED set (Q5 for dealer)\n");
      ok = 0;
    }
  }

  /* --- Drive Bpr n more times; sendDest must walk all n destinations
   * and SEND replay must persist on every call. --- */
  memset(seenDest, 0, n);
  for (i = 0; i < n; ++i) {
    unsigned int na;
    unsigned int a;
    int sawSend;
    na = ct04DspBpr(d, &pump, out);
    sawSend = 0;
    for (a = 0; a < na; ++a)
      if (out[a].act == CT04_DSP_ACT_SEND) {
        sawSend = 1;
        if (out[a].dest < n)
          seenDest[out[a].dest] = 1;
        /* Each SEND must carry the matching frankenLeaves[dest] +
         * frankenProofs[dest] pair from pendingPc / pendingPr. */
        if (memcmp(out[a].piece, frankenLeaves[out[a].dest], pieceSz) != 0
         || memcmp(out[a].proof, frankenProofs[out[a].dest], proofSz) != 0) {
          printf("    SEND replay at dest=%u: piece/proof mismatch\n",
           out[a].dest);
          ok = 0;
        }
      }
    if (!sawSend) {
      printf("    Bpr call %u: SEND not emitted (sendDest stalled?)\n", i);
      ok = 0;
    }
  }
  /* After n+1 total Bpr calls (the initial one plus this loop), we've
   * cycled through all n destinations at least once.  Note: the first
   * Bpr above visited dest=0; this loop visits dest=1..n-1 then 0
   * again.  All n bits should be set. */
  for (i = 0; i < n; ++i)
    if (!seenDest[i]) {
      printf("    sendDest never reached %u after %u Bpr calls\n", i, n);
      ok = 0;
    }

  if (ok)
    printf("    Q7 origin-retains-through-abort: PASS\n");

  for (i = 0; i < n; ++i) {
    free(piecesA[i]);
    free(piecesB[i]);
    free(proofsTmp[i]);
    free(frankenProofs[i]);
  }
  free(d);
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
  free(seenDest);
  return (ok ? 0 : 1);
}

int
main(
  void
){
  rsecMkHsh_t Hrsec;
  struct thrDspRsecCfg cfgR;
  struct thrDsp d;

  Hrsec.a = rmd128Allocate;
  Hrsec.i = (void(*)(void *))rmd128init;
  Hrsec.u = (void(*)(void *, const unsigned char *, unsigned int))rmd128update;
  Hrsec.f = (void(*)(void *, unsigned char *))rmd128final;
  Hrsec.d = free;
  Hrsec.h = 4;
  cfgR.h = &Hrsec;
  if (thrDspRsecInit(&d, &cfgR)) {
    fprintf(stderr, "thrDspRsecInit\n");
    return (1);
  }

  printf("== ct04Dsp Q7 regression (originator self-abort retains ORIGIN) ==\n");
  if (runQ7(&d, &Hrsec, 4, 1, 64))
    Fail = 1;
  if (runQ7(&d, &Hrsec, 7, 2, 1000))
    Fail = 1;

  thrDspRsecFini(&d);
  printf("\nQ7 tests completed%s.\n", Fail ? " with FAILURES" : "");
  return (Fail);
}
