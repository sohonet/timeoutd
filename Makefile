CFLAGS=-fomit-frame-pointer -O2 -s -g -Wall
timeoutd:	timeoutd.c Makefile
	#$(CC) $(CFLAGS) -o timeoutd timeoutd.c
	$(CC) $(CFLAGS) -o timeoutd.o -c timeoutd.c -DTIMEOUTDX11
	$(CC) $(CFLAGS) -o timeoutd -L/usr/X11R6/lib timeoutd.o -lXss -lXext
	

install:
	install -o root -g system -m 2111 timeoutd /usr/etc/timeoutd
	install -o man -g info -m 444 timeoutd.8 /usr/man/man8
	install -o man -g info -m 444 timeouts.5 /usr/man/man5
