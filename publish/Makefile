CC=cc
CFLAGS=
LIBS=-lmosquitto

publish: publish.c
	$(CC) $(CFLAGS) $(LIBS) publish.c -opublish

install: publish
	cp publish /usr/local/bin
	chmod 755 /usr/local/bin/publish
