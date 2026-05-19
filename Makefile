CFLAGS = -I. -Os -g
DTC = ../decisionTableCompiler/dtc
AWK = awk

all: example_bracha87Fig1 example_bkr94acs

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

check: test_bracha87 test_bkr94acs test_predicates test_bracha87_blackbox test_bkr94acs_blackbox
	./test_bracha87
	./test_bkr94acs
	./test_predicates
	./test_bracha87_blackbox
	./test_bkr94acs_blackbox

clean:
	rm -f bracha87Fig1.psu bracha87Fig3.psu bracha87Fig4.psu bkr94acs.psu
	rm -f bracha87.o bkr94acs.o
	rm -f example_bracha87Fig1 example_bkr94acs
	rm -f test_bracha87 test_bkr94acs test_predicates test_bracha87_blackbox test_bkr94acs_blackbox

clobber: clean
	rm -f bracha87Fig1.c bracha87Fig3.c bracha87Fig4.c bkr94acsRules.c
