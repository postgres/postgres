ifeq ($(python_majorversion),3)
# Adjust regression tests for Python 3 compatibility
#
# Mention those regression test files that need to be mangled in the
# variable REGRESS_PLPYTHON3_MANGLE.  They will be copied to a
# subdirectory python3/ and have their Python syntax and other bits
# adjusted to work with Python 3.

# Note that the order of the tests needs to be preserved in this
# expression.
REGRESS := $(foreach test,$(REGRESS),$(if $(filter $(test),$(REGRESS_PLPYTHON3_MANGLE)),python3/$(test),$(test)))

.PHONY: pgregress-python3-mangle
pgregress-python3-mangle:
	$(MKDIR_P) sql/python3 expected/python3 results/python3
	for file in $(patsubst %,$(srcdir)/sql/%.sql,$(REGRESS_PLPYTHON3_MANGLE)) $(patsubst %,$(srcdir)/expected/%*.out,$(REGRESS_PLPYTHON3_MANGLE)); do \
	  sed \
	      -e "s/<type 'exceptions\.\([[:alpha:]]*\)'>/<class '\1'>/g" \
	      -e "s/<type 'long'>/<class 'int'>/g" \
	      -e "s/\([0-9][0-9]*\)L/\1/g" \
	      -e 's/\([ [{]\)u"/\1"/g' \
	      -e "s/\([ [{]\)u'/\1'/g" \
	      -e "s/def next/def __next__/g" \
	      -e "s/LANGUAGE plpythonu/LANGUAGE plpython3u/g" \
	      -e "s/LANGUAGE plpython2u/LANGUAGE plpython3u/g" \
	      -e "s/EXTENSION \([^ ]*_\)*plpythonu/EXTENSION \1plpython3u/g" \
	      -e "s/EXTENSION \([^ ]*_\)*plpython2u/EXTENSION \1plpython3u/g" \
	      -e 's/installing required extension "plpython2u"/installing required extension "plpython3u"/g' \
	    $$file >`echo $$file | sed 's,^.*/\([^/][^/]*/\)\([^/][^/]*\)$$,\1python3/\2,'` || exit; \
	done

check installcheck: pgregress-python3-mangle

pg_regress_clean_files += sql/python3/ expected/python3/ results/python3/

endif # Python 3
