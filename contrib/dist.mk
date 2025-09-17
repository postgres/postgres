# contrib/dist.mk
#
# Package each contrib extension into its own .tar.gz archive

prefix ?= /install/pglite
CONTRIB_BUILD_ROOT := /tmp/extensions/build
ARCHIVE_DIR := /install/pglite/extensions

CONTRIBS := $(SUBDIRS)

# Default target: build tarballs for all contribs
dist: $(addsuffix .tar.gz,$(CONTRIBS))

# Pattern rule: build $(EXT).tar.gz for each contrib
%.tar.gz:
	@echo "=== Staging $* ==="
	rm -rf $(CONTRIB_BUILD_ROOT)/$*
	bash -c 'mkdir -p $(CONTRIB_BUILD_ROOT)/$*/$(prefix)/{bin,lib,share/extension,share/doc,share/postgresql/extension,share/postgresql/tsearch_data,include}'
	$(MAKE) -C $* install DESTDIR=$(CONTRIB_BUILD_ROOT)/$*
	@echo "=== Packaging $* ==="
	mkdir -p $(ARCHIVE_DIR)
	cd $(CONTRIB_BUILD_ROOT)/$*/$(prefix) && \
	files=$$(find . -type f -o -type l | sed 's|^\./||') && \
	tar -czf $(ARCHIVE_DIR)/$*.tar.gz $$files
# 	tar -C $(CONTRIB_BUILD_ROOT)/$*/$(prefix) -czf $(ARCHIVE_DIR)/$*.tar.gz .

.PHONY: dist
