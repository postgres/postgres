SUBDIRS = src/include src/lib src/preproc

all install uninstall clean::
	for i in $(SUBDIRS); do ( cd $$i; make $@ ); done
