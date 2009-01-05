CFLAGS=-fomit-frame-pointer -m486 -O2 -s
timeoutd:	timeoutd.c Makefile
	$(CC) $(CFLAGS) -o timeoutd timeoutd.c

install:
	install -o root -g system -m 2111 timeoutd /usr/etc/timeoutd
	install -o man -g info -m 444 timeoutd.8 /usr/man/man8
	install -o man -g info -m 444 timeouts.5 /usr/man/man5
