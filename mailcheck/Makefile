# Makefile for mail checker

NAME=mailcheck
BINDIR=/usr/libexec/mate-applets

SCHEMADIR=/usr/share/glib-2.0/schemas
SCHEMAFILE=org.mate.panel.applet.mailcheck.gschema.xml

APPLETSDIR=/usr/share/mate-panel/applets
APPLETSFILE=org.mate.applets.MailCheckApplet.mate-panel-applet

SERVICESDIR=/usr/share/dbus-1/services
SERVICESFILE=org.mate.panel.applet.MailCheckAppletFactory.service

INSTALL=/usr/bin/install
INSTALLEXE=$(INSTALL) -m 555
INSTALLDAT=$(INSTALL) -m 444

FILES=$(NAME).c $(SERVER) $(SCHEMAFILE) $(APPLETSFILE) $(SERVICESFILE) Makefile

TARBZ2=$(NAME).tar.bz2

.PHONY: install install-$(NAME) install-schema install-applet install-server clean tar

$(NAME): $(NAME).c
	$(CC) -pipe -O3 `pkg-config --cflags --libs libmatepanelapplet-4.0` -Wall $(CFLAGS) $(LDFLAGS) -o $(NAME) $(NAME).c $(LDLIBS) -lX11

install: install-$(NAME) install-schema install-applet install-server

install-$(NAME): $(NAME)
	mkdir -p $(BINDIR)
	$(INSTALLEXE) $(NAME) $(BINDIR)/$(NAME)_applet2

install-schema: $(SCHEMAFILE)
	$(INSTALLDAT) $(SCHEMAFILE) $(SCHEMADIR)/$(SCHEMAFILE)

install-applet: $(APPLETSFILE)
	$(INSTALLDAT) $(APPLETSFILE) $(APPLETSDIR)/$(APPLETSFILE)

install-server: $(SERVICESFILE)
	$(INSTALLDAT) $(SERVICESFILE) $(SERVICESDIR)/$(SERVICESFILE)

$(TARBZ2): $(FILES)
	tar cvf - $(FILES) | bzip2 -v9 > $(TARBZ2)

tar: $(TARBZ2)

clean:
	rm -f $(NAME).o $(NAME)
