# $Header: /cvsroot/pgsql/src/nls-global.mk,v 1.1 2001/06/02 18:25:17 petere Exp $

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

PO_FILES = $(addsuffix .po, $(LANGUAGES))
MO_FILES = $(addsuffix .mo, $(LANGUAGES))

XGETTEXT += --foreign-user


all-po: $(MO_FILES)

distprep: $(srcdir)/$(CATALOG_NAME).pot

%.mo: %.po
	$(MSGFMT) -o $@ $<

ifdef XGETTEXT
ifeq ($(word 1,$(GETTEXT_FILES)),+)
$(srcdir)/$(CATALOG_NAME).pot: $(word 2, $(GETTEXT_FILES))
	$(XGETTEXT) -D $(srcdir) -ctranslator -n $(addprefix -k, $(GETTEXT_TRIGGERS)) -f $<
else
$(srcdir)/$(CATALOG_NAME).pot: $(GETTEXT_FILES)
	$(XGETTEXT) -ctranslator -n $(addprefix -k, $(GETTEXT_TRIGGERS)) $^
endif
	mv messages.po $@
else # not XGETTEXT
	@echo "You don't have 'xgettext'."; exit 1
endif # not XGETTEXT


install-po: all-po installdirs-po
	for lang in $(LANGUAGES); do \
	  $(INSTALL_DATA) $$lang.mo $(DESTDIR)$(localedir)/$$lang/LC_MESSAGES/$(CATALOG_NAME).mo || exit 1; \
	done

installdirs-po:
	$(mkinstalldirs) $(foreach lang, $(LANGUAGES), $(DESTDIR)$(localedir)/$(lang)/LC_MESSAGES)

uninstall-po:
	rm -f $(foreach lang, $(LANGUAGES), $(DESTDIR)$(localedir)/$(lang)/LC_MESSAGES/$(CATALOG_NAME).mo)


clean-po:
	rm -f $(MO_FILES)
	@rm -f $(addsuffix .po.old, $(AVAIL_LANGUAGES))

maintainer-clean-po:
	rm -f $(srcdir)/$(CATALOG_NAME).pot


maintainer-check-po: $(PO_FILES)
	for file in $^; do \
	  $(MSGFMT) -v -o /dev/null $$file || exit 1; \
	done


init-po: $(srcdir)/$(CATALOG_NAME).pot


update-po: $(srcdir)/$(CATALOG_NAME).pot
ifdef MSGMERGE
#	XXX  should be $(LANGUAGES)?
	@for lang in $(AVAIL_LANGUAGES); do \
	  echo "merging $$lang:"; \
	  if $(MSGMERGE) $(srcdir)/$$lang.po $< -o $$lang.po.new; \
	  then \
	    mv $(srcdir)/$$lang.po $$lang.po.old; \
	    mv $$lang.po.new $(srcdir)/$$lang.po; \
	  else \
	    echo "msgmerge for $$lang failed"; \
	    rm -f $$lang.po.new; \
	  fi; \
	done
else
	@echo "You don't have 'msgmerge'." ; exit 1
endif


all: all-po
install: install-po
installdirs: installdirs-po
uninstall: uninstall-po
clean distclean: clean-po
maintainer-clean: maintainer-clean-po
maintainer-check: maintainer-check-po

.PHONY: all-po install-po installdirs-po uninstall-po clean-po \
        maintainer-clean-po maintainer-check-po init-po update-po
