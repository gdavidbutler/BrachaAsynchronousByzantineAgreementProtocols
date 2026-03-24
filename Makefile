CFLAGS = -Os -g

all: test_bracha87 test_acs example_consensus example_acs

bracha87.o: bracha87.c bracha87.h
	$(CC) $(CFLAGS) -c -o $@ bracha87.c

acs.o: acs.c acs.h bracha87.h
	$(CC) $(CFLAGS) -c -o $@ acs.c

test_bracha87: test/test_bracha87.c bracha87.o bracha87.h
	$(CC) $(CFLAGS) -I. -o $@ test/test_bracha87.c bracha87.o

example_consensus: example/consensus.c bracha87.o bracha87.h
	$(CC) $(CFLAGS) -I. -o $@ example/consensus.c bracha87.o

example_acs: example/acs.c acs.o bracha87.o acs.h bracha87.h
	$(CC) $(CFLAGS) -I. -o $@ example/acs.c acs.o bracha87.o

test_acs: test/test_acs.c acs.o bracha87.o acs.h bracha87.h
	$(CC) $(CFLAGS) -I. -o $@ test/test_acs.c acs.o bracha87.o

check: test_bracha87 test_acs
	./test_bracha87
	./test_acs

clean:
	rm -f bracha87.o acs.o test_bracha87 test_acs example_consensus example_acs
