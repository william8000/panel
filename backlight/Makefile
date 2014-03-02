# Makefile for laptop panel backlight applet

NAME=backlight
BINDIR=/usr/libexec/mate-applets

SCHEMADIR=/usr/share/glib-2.0/schemas
SCHEMAFILE=org.mate.panel.applet.backlight.gschema.xml

APPLETSDIR=/usr/share/mate-panel/applets
APPLETSFILE=org.mate.applets.BacklightApplet.mate-panel-applet

SERVICESDIR=/usr/share/dbus-1/services
SERVICESFILE=org.mate.panel.applet.BacklightAppletFactory.service

INSTALL=/usr/bin/install
INSTALLEXE=$(INSTALL) -m 555
INSTALLDAT=$(INSTALL) -m 444

#ICON=backlight.png

FILES=$(NAME).c $(SERVER) $(SCHEMAFILE) $(APPLETSFILE) $(SERVICESFILE) Makefile

TARBZ2=$(NAME).tar.bz2

$(NAME): $(NAME).c
	gcc `pkg-config --cflags --libs libmatepanelapplet-4.0` `pkg-config --cflags --libs dbus-glib-1` -Wall -o $(NAME) $(NAME).c

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
