SUBDIRS = include lib preproc doc

all install uninstall clean:
	for i in $(SUBDIRS); do ( cd $$i; make $@ ); done
