# $Header: /cvsroot/pgsql/src/nls-global.mk,v 1.8 2003/09/14 22:40:38 petere Exp $

# Common rules for Native Language Support (NLS)
#
# If some subdirectory of the source tree wants to provide NLS, it
# needs to contain a file 'nls.mk' with the following make variable
# assignments:
#
# CATALOG_NAME	        -- name of the message catalog (xxx.po); probably
#                          name of the program
# AVAIL_LANGUAGES	-- list of languages that are provided/supported
# GETTEXT_FILES         -- list of source files that contain message strings
# GETTEXT_TRIGGERS      -- (optional) list of functions that contain
#                          translatable strings
#
# That's all, the rest is done here, if --enable-nls was specified.
#
# The only user-visible targets here are 'init-po', to make an initial
# "blank" catalog from program sources, and 'update-po', which is to
# be called if the messages in the program source have changed, in
# order to merge the changes into the existing .po files.


# existence checked by Makefile.global; otherwise we won't get here
include $(srcdir)/nls.mk

# If user specified the languages he wants in --enable-nls=LANGUAGES,
# filter out the rest.  Else use all available ones.
ifdef WANTED_LANGUAGES
LANGUAGES = $(filter $(WANTED_LANGUAGES), $(AVAIL_LANGUAGES))
else
LANGUAGES = $(AVAIL_LANGUAGES)
endif

PO_FILES = $(addprefix po/, $(addsuffix .po, $(LANGUAGES)))
MO_FILES = $(addprefix po/, $(addsuffix .mo, $(LANGUAGES)))

ifdef XGETTEXT
XGETTEXT += --foreign-user -ctranslator
endif


all-po: $(MO_FILES)

%.mo: %.po
	$(MSGFMT) -o $@ $<

ifdef XGETTEXT
ifeq ($(word 1,$(GETTEXT_FILES)),+)
po/$(CATALOG_NAME).pot: $(word 2, $(GETTEXT_FILES))
	$(XGETTEXT) -D $(srcdir) -n $(addprefix -k, $(GETTEXT_TRIGGERS)) -f $<
else
po/$(CATALOG_NAME).pot: $(GETTEXT_FILES)
# Change to srcdir explicitly, don't rely on $^.  That way we get
# consistent #: file references in the po files.
	$(XGETTEXT) -D $(srcdir) -n $(addprefix -k, $(GETTEXT_TRIGGERS)) $(GETTEXT_FILES)
endif
	@$(mkinstalldirs) $(dir $@)
	mv messages.po $@
else # not XGETTEXT
	@echo "You don't have 'xgettext'."; exit 1
endif # not XGETTEXT


install-po: all-po installdirs-po
ifneq (,$(LANGUAGES))
	for lang in $(LANGUAGES); do \
	  $(INSTALL_DATA) po/$$lang.mo $(DESTDIR)$(localedir)/$$lang/LC_MESSAGES/$(CATALOG_NAME).mo || exit 1; \
	done
endif

installdirs-po:
	$(mkinstalldirs) $(foreach lang, $(LANGUAGES), $(DESTDIR)$(localedir)/$(lang)/LC_MESSAGES)

uninstall-po:
	rm -f $(foreach lang, $(LANGUAGES), $(DESTDIR)$(localedir)/$(lang)/LC_MESSAGES/$(CATALOG_NAME).mo)


clean-po:
	rm -f $(MO_FILES)
	@rm -f $(addsuffix .old, $(PO_FILES))
	rm -f po/$(CATALOG_NAME).pot


maintainer-check-po: $(PO_FILES)
	for file in $^; do \
	  $(MSGFMT) -c -v -o /dev/null $$file || exit 1; \
	done


init-po: po/$(CATALOG_NAME).pot


update-po: po/$(CATALOG_NAME).pot
ifdef MSGMERGE
	@for lang in $(LANGUAGES); do \
	  echo "merging $$lang:"; \
	  if $(MSGMERGE) $(srcdir)/po/$$lang.po $< -o po/$$lang.po.new; \
	  then \
	    mv $(srcdir)/po/$$lang.po po/$$lang.po.old; \
	    mv po/$$lang.po.new $(srcdir)/po/$$lang.po; \
	  else \
	    echo "msgmerge for $$lang failed"; \
	    rm -f po/$$lang.po.new; \
	  fi; \
	done
else
	@echo "You don't have 'msgmerge'." ; exit 1
endif


all: all-po
install: install-po
installdirs: installdirs-po
uninstall: uninstall-po
clean distclean maintainer-clean: clean-po
maintainer-check: maintainer-check-po

.PHONY: all-po install-po installdirs-po uninstall-po clean-po \
        maintainer-check-po init-po update-po
.SILENT: installdirs-po
