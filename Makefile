TARGETS = backlight mailcheck temperature

BUILDDIRS = $(TARGETS:%=build-%)
INSTALLDIRS = $(TARGETS:%=install-%)
CLEANDIRS = $(TARGETS:%=clean-%)

.PHONY: all build clean distclean reallyclean $(TARGETS) $(BUILDDIRS) $(INSTALLDIRS) $(CLEANDIRS)

all: $(BUILDDIRS)

$(TARGETS):
	$(MAKE) -C $@

$(BUILDDIRS):
	$(MAKE) -C $(@:build-%=%)

install: all $(INSTALLDIRS)

$(INSTALLDIRS):
	$(MAKE) -C $(@:install-%=%) install

clean: $(CLEANDIRS)

$(CLEANDIRS): 
	$(MAKE) -C $(@:clean-%=%) clean

distclean: clean

reallyclean: clean
