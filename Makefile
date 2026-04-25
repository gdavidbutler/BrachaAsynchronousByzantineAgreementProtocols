CFLAGS = -Os -g

all: test_bracha87 test_bkr94acs example_bracha87 example_bkr94acs

bracha87.o: bracha87.c bracha87.h
	$(CC) $(CFLAGS) -c -o $@ bracha87.c

bkr94acs.o: bkr94acs.c bkr94acs.h bracha87.h
	$(CC) $(CFLAGS) -c -o $@ bkr94acs.c

test_bracha87: test/test_bracha87.c bracha87.o bracha87.h
	$(CC) $(CFLAGS) -I. -o $@ test/test_bracha87.c bracha87.o

example_bracha87: example/bracha87.c bracha87.o bracha87.h
	$(CC) $(CFLAGS) -I. -o $@ example/bracha87.c bracha87.o

example_bkr94acs: example/bkr94acs.c bkr94acs.o bracha87.o bkr94acs.h bracha87.h
	$(CC) $(CFLAGS) -I. -o $@ example/bkr94acs.c bkr94acs.o bracha87.o

test_bkr94acs: test/test_bkr94acs.c bkr94acs.o bracha87.o bkr94acs.h bracha87.h
	$(CC) $(CFLAGS) -I. -o $@ test/test_bkr94acs.c bkr94acs.o bracha87.o

check: test_bracha87 test_bkr94acs
	./test_bracha87
	./test_bkr94acs

clean:
	rm -f bracha87.o bkr94acs.o test_bracha87 test_bkr94acs example_bracha87 example_bkr94acs
