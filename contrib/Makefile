# contrib/Makefile

subdir = contrib
top_builddir = ..
include $(top_builddir)/src/Makefile.global

SUBDIRS = \
		amcheck		\
		auth_delay	\
		auto_explain	\
		basic_archive	\
		basebackup_to_shell	\
		bloom		\
		btree_gin	\
		btree_gist	\
		citext		\
		cube		\
		dblink		\
		dict_int	\
		dict_xsyn	\
		earthdistance	\
		file_fdw	\
		fuzzystrmatch	\
		hstore		\
		intagg		\
		intarray	\
		isn		\
		lo		\
		ltree		\
		oid2name	\
		pageinspect	\
		passwordcheck	\
		pg_buffercache	\
		pg_freespacemap \
		pg_logicalinspect \
		pg_overexplain \
		pg_prewarm	\
		pg_stat_statements \
		pg_surgery	\
		pg_trgm		\
		pgrowlocks	\
		pgstattuple	\
		pg_visibility	\
		pg_walinspect	\
		postgres_fdw	\
		seg		\
		spi		\
		tablefunc	\
		tcn		\
		test_decoding	\
		tsm_system_rows \
		tsm_system_time \
		unaccent	\
		vacuumlo

ifeq ($(with_ssl),openssl)
SUBDIRS += pgcrypto sslinfo
else
ALWAYS_SUBDIRS += pgcrypto sslinfo
endif

ifneq ($(with_uuid),no)
SUBDIRS += uuid-ossp
else
ALWAYS_SUBDIRS += uuid-ossp
endif

ifeq ($(with_libxml),yes)
SUBDIRS += xml2
else
ALWAYS_SUBDIRS += xml2
endif

ifeq ($(with_selinux),yes)
SUBDIRS += sepgsql
else
ALWAYS_SUBDIRS += sepgsql
endif

ifeq ($(with_perl),yes)
SUBDIRS += bool_plperl hstore_plperl jsonb_plperl
else
ALWAYS_SUBDIRS += bool_plperl hstore_plperl jsonb_plperl
endif

ifeq ($(with_python),yes)
SUBDIRS += hstore_plpython jsonb_plpython ltree_plpython
else
ALWAYS_SUBDIRS += hstore_plpython jsonb_plpython ltree_plpython
endif

# Missing:
#		start-scripts	\ (does not have a makefile)


$(recurse)
$(recurse_always)
