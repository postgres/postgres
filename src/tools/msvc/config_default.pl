# Configuration arguments for vcbuild.
use strict;
use warnings;

our $config = {
	asserts => 0,    # --enable-cassert
	  # integer_datetimes=>1,   # --enable-integer-datetimes - on is now default
	  # float4byval=>1,         # --disable-float4-byval, on by default
	  # float8byval=>0,         # --disable-float8-byval, off by default
	  # blocksize => 8,         # --with-blocksize, 8kB by default
	  # wal_blocksize => 8,     # --with-wal-blocksize, 8kB by default
	  # wal_segsize => 16,      # --with-wal-segsize, 16MB by default
	ldap    => 1,        # --with-ldap
	nls     => undef,    # --enable-nls=<path>
	tcl     => undef,    # --with-tls=<path>
	perl    => undef,    # --with-perl
	python  => undef,    # --with-python=<path>
	krb5    => undef,    # --with-krb5=<path>
	openssl => undef,    # --with-ssl=<path>
	uuid    => undef,    # --with-ossp-uuid
	xml     => undef,    # --with-libxml=<path>
	xslt    => undef,    # --with-libxslt=<path>
	iconv   => undef,    # (not in configure, path to iconv)
	zlib    => undef     # --with-zlib=<path>
};

1;
