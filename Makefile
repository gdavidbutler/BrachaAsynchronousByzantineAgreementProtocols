CFLAGS = -I. -Os -g
DTC = ../decisionTableCompiler/dtc
AWK = awk

all: example_bracha87Fig1 example_bkr94acs example_ct04Dsp

bracha87.o: bracha87.c bracha87.h bracha87Fig1.c bracha87Fig3.c bracha87Fig4.c
	$(CC) $(CFLAGS) -c -o $@ bracha87.c

bracha87Fig1.psu: bracha87Fig1.dtc bracha87Fig1ToC.dtc
	$(DTC) bracha87Fig1.dtc bracha87Fig1ToC.dtc > $@

bracha87Fig1.c: bracha87Fig1.psu psu.awk
	$(AWK) -f psu.awk bracha87Fig1.psu > $@

bracha87Fig3.psu: bracha87Fig3.dtc bracha87Fig3ToC.dtc
	$(DTC) bracha87Fig3.dtc bracha87Fig3ToC.dtc > $@

bracha87Fig3.c: bracha87Fig3.psu psu.awk
	$(AWK) -f psu.awk bracha87Fig3.psu > $@

bracha87Fig4.psu: bracha87Fig4.dtc bracha87Fig4ToC.dtc
	$(DTC) bracha87Fig4.dtc bracha87Fig4ToC.dtc > $@

bracha87Fig4.c: bracha87Fig4.psu psu.awk
	$(AWK) -f psu.awk bracha87Fig4.psu > $@

bkr94acs.o: bkr94acs.c bkr94acs.h bracha87.h bkr94acsRules.c
	$(CC) $(CFLAGS) -c -o $@ bkr94acs.c

bkr94acs.psu: bkr94acs.dtc bkr94acsToC.dtc
	$(DTC) bkr94acs.dtc bkr94acsToC.dtc > $@

bkr94acsRules.c: bkr94acs.psu psu.awk
	$(AWK) -f psu.awk bkr94acs.psu > $@

ct04Dsp.o: ct04Dsp.c ct04Dsp.h thrDsp.h bracha87.h ct04DspRules.c
	$(CC) $(CFLAGS) -c -o $@ ct04Dsp.c

ct04Dsp.psu: ct04Dsp.dtc ct04DspToC.dtc
	$(DTC) ct04Dsp.dtc ct04DspToC.dtc > $@

ct04DspRules.c: ct04Dsp.psu psu.awk
	$(AWK) -f psu.awk ct04Dsp.psu > $@

ct04Rtv.o: ct04Rtv.c ct04Rtv.h ct04Dsp.h thrDsp.h
	$(CC) $(CFLAGS) -c -o $@ ct04Rtv.c

ct04DspRbc.o: ct04DspRbc.c ct04DspRbc.h ct04Dsp.h
	$(CC) $(CFLAGS) -c -o $@ ct04DspRbc.c

# test_ct04Dsp / example_ct04Dsp link both thrDsp adapters; each adapter
# now lives with its codec in the sibling repo (../ReedSolomonErasureCoding/
# and ../ShamirSecretSharing/).  thrDsp.h stays in this repo as the
# protocol-layer plugin contract that ct04Dsp consumes; the adapters
# include it via -I../asynchronousByzantineAgreementProtocols in their
# own Makefiles.

test_ct04Dsp: test/test_ct04Dsp.c ct04Dsp.o ct04Rtv.o ct04DspRbc.o bracha87.o ct04Dsp.h ct04Rtv.h ct04DspRbc.h thrDsp.h ../ReedSolomonErasureCoding/thrDspRsec.o ../ReedSolomonErasureCoding/thrDspRsec.h ../ShamirSecretSharing/thrDspSss.o ../ShamirSecretSharing/thrDspSss.h ../ReedSolomonErasureCoding/rsec.o ../ReedSolomonErasureCoding/rsecMk.o ../ShamirSecretSharing/sss.o ../ShamirSecretSharing/sssMk.o ../rmd128/rmd128.o
	$(CC) $(CFLAGS) -I. -I../ReedSolomonErasureCoding -I../ShamirSecretSharing -I../rmd128 -o $@ test/test_ct04Dsp.c ct04Dsp.o ct04Rtv.o ct04DspRbc.o bracha87.o ../ReedSolomonErasureCoding/thrDspRsec.o ../ShamirSecretSharing/thrDspSss.o ../ReedSolomonErasureCoding/rsec.o ../ReedSolomonErasureCoding/rsecMk.o ../ShamirSecretSharing/sss.o ../ShamirSecretSharing/sssMk.o ../rmd128/rmd128.o

# Q7 white-box test: #include ct04Dsp.c directly to reach private
# layout macros (D_PENDINGPC, D_PROOFSELF, D_ROOTCMT, etc.) for
# Byzantine ORIGIN state injection.  Same precedent as test_predicates.c
# which includes bracha87.c.  Do NOT link ct04Dsp.o here - the include
# would cause duplicate symbol definitions.
test_ct04DspQ7: test/test_ct04DspQ7.c ct04Dsp.c ct04Dsp.h thrDsp.h bracha87.h ct04DspRules.c bracha87.o ../ReedSolomonErasureCoding/thrDspRsec.o ../ReedSolomonErasureCoding/thrDspRsec.h ../ReedSolomonErasureCoding/rsec.o ../ReedSolomonErasureCoding/rsecMk.o ../rmd128/rmd128.o
	$(CC) $(CFLAGS) -I. -I../ReedSolomonErasureCoding -I../rmd128 -o $@ test/test_ct04DspQ7.c bracha87.o ../ReedSolomonErasureCoding/thrDspRsec.o ../ReedSolomonErasureCoding/rsec.o ../ReedSolomonErasureCoding/rsecMk.o ../rmd128/rmd128.o

example_ct04Dsp: example/ct04Dsp.c ct04Dsp.o ct04Rtv.o ct04DspRbc.o bracha87.o ct04Dsp.h ct04Rtv.h ct04DspRbc.h thrDsp.h ../ReedSolomonErasureCoding/thrDspRsec.o ../ReedSolomonErasureCoding/thrDspRsec.h ../ShamirSecretSharing/thrDspSss.o ../ShamirSecretSharing/thrDspSss.h ../ReedSolomonErasureCoding/rsec.o ../ReedSolomonErasureCoding/rsecMk.o ../ShamirSecretSharing/sss.o ../ShamirSecretSharing/sssMk.o ../rmd128/rmd128.o
	$(CC) $(CFLAGS) -I. -I../ReedSolomonErasureCoding -I../ShamirSecretSharing -I../rmd128 -o $@ example/ct04Dsp.c ct04Dsp.o ct04Rtv.o ct04DspRbc.o bracha87.o ../ReedSolomonErasureCoding/thrDspRsec.o ../ShamirSecretSharing/thrDspSss.o ../ReedSolomonErasureCoding/rsec.o ../ReedSolomonErasureCoding/rsecMk.o ../ShamirSecretSharing/sss.o ../ShamirSecretSharing/sssMk.o ../rmd128/rmd128.o

test_bracha87: test/test_bracha87.c bracha87.o bracha87.h
	$(CC) $(CFLAGS) -I. -o $@ test/test_bracha87.c bracha87.o

test_bkr94acs: test/test_bkr94acs.c bkr94acs.o bracha87.o bkr94acs.h bracha87.h
	$(CC) $(CFLAGS) -I. -o $@ test/test_bkr94acs.c bkr94acs.o bracha87.o

test_predicates: test/test_predicates.c bracha87.c bracha87.h bracha87Fig1.c bracha87Fig3.c bracha87Fig4.c
	$(CC) $(CFLAGS) -I. -o $@ test/test_predicates.c

test_bracha87_blackbox: test/test_bracha87_blackbox.c bracha87.o bracha87.h
	$(CC) $(CFLAGS) -I. -o $@ test/test_bracha87_blackbox.c bracha87.o

test_bkr94acs_blackbox: test/test_bkr94acs_blackbox.c bkr94acs.o bracha87.o bkr94acs.h bracha87.h
	$(CC) $(CFLAGS) -I. -o $@ test/test_bkr94acs_blackbox.c bkr94acs.o bracha87.o

example_bracha87Fig1: example/bracha87Fig1.c bracha87.o bracha87.h
	$(CC) $(CFLAGS) -I. -o $@ example/bracha87Fig1.c bracha87.o

example_bkr94acs: example/bkr94acs.c bkr94acs.o bracha87.o bkr94acs.h bracha87.h
	$(CC) $(CFLAGS) -I. -o $@ example/bkr94acs.c bkr94acs.o bracha87.o

check: test_bracha87 test_bkr94acs test_predicates test_bracha87_blackbox test_bkr94acs_blackbox test_ct04Dsp test_ct04DspQ7
	./test_bracha87
	./test_bkr94acs
	./test_predicates
	./test_bracha87_blackbox
	./test_bkr94acs_blackbox
	./test_ct04Dsp
	./test_ct04DspQ7

# Plugin-contract tests (thrDspRsecTest, thrDspSssTest) live with their
# adapters in ../ReedSolomonErasureCoding/ and ../ShamirSecretSharing/;
# run their `make check` separately to exercise the plugin contract.

clean:
	rm -f bracha87Fig1.psu bracha87Fig3.psu bracha87Fig4.psu bkr94acs.psu ct04Dsp.psu
	rm -f bracha87.o bkr94acs.o ct04Dsp.o ct04Rtv.o ct04DspRbc.o
	rm -f example_bracha87Fig1 example_bkr94acs example_ct04Dsp
	rm -f test_bracha87 test_bkr94acs test_predicates test_bracha87_blackbox test_bkr94acs_blackbox test_ct04Dsp test_ct04DspQ7

clobber: clean
	rm -f bracha87Fig1.c bracha87Fig3.c bracha87Fig4.c bkr94acsRules.c ct04DspRules.c
