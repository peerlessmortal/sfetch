CC      = cc
CFLAGS  = -Wall -Wextra -O2 -std=c11
LDFLAGS = -lm
PREFIX  = /usr/local

sfetch: main.c
	$(CC) $(CFLAGS) -o sfetch main.c $(LDFLAGS)

install: sfetch
	install -Dm755 sfetch $(DESTDIR)$(PREFIX)/bin/sfetch

clean:
	rm -f sfetch

.PHONY: install clean
