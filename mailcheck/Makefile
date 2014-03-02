# Makefile for mail checker

NAME=mailcheck
BINDIR=/usr/local/lib/gnome-panel

SERVER=MailCheck.server
SERVERDIR=/usr/lib/bonobo/servers

ICON=redhat-email.png

FILES=$(NAME).c $(SERVER) $(ICON) Makefile

TARBZ2=$(NAME).tar.bz2

$(NAME): $(NAME).c
	gcc `pkg-config --cflags --libs libpanelapplet-2.0` -lX11 -o $(NAME) $(NAME).c

install: install-$(NAME) install-server

install-$(NAME): $(NAME)
	mkdir -p $(BINDIR)
	cp -p $(NAME) $(BINDIR)/$(NAME)

install-server: $(SERVER)
	cp -p $(SERVER) $(SERVERDIR)/$(SERVER)

$(TARBZ2): $(FILES)
	tar cvf - $(FILES) | bzip2 -v9 > $(TARBZ2)

tar: $(TARBZ2)

clean:
	rm -f $(NAME).o $(NAME)
