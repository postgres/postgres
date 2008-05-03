# Configuration arguments for vcbuild.
use strict;
use warnings;

our $config = {
    asserts=>0,			# --enable-cassert
    # integer_datetimes=>1,   # --enable-integer-datetimes - on is now default
    # float4byval=>1,         # --disable-float4-byval, on by default
    # float8byval=>0,         # --disable-float8-byval, off by default
    # blocksize => 8,         # --with-blocksize, 8kB by default
    # wal_blocksize => 8,     # --with-wal-blocksize, 8kb by default
    # wal_segsize => 16,      # --with-wal-segsize, 16MB by default
    nls=>undef,				# --enable-nls=<path>
    tcl=>'c:\tcl',		# --with-tls=<path>
    perl=>'c:\perl', 			# --with-perl
    python=>'c:\python24', # --with-python=<path>
    krb5=>'c:\prog\pgsql\depend\krb5', # --with-krb5=<path>
    ldap=>1,			# --with-ldap
    openssl=>'c:\openssl', # --with-ssl=<path>
    uuid=>'c:\prog\pgsql\depend\ossp-uuid', #--with-ossp-uuid
    xml=>'c:\prog\pgsql\depend\libxml2',
    xslt=>'c:\prog\pgsql\depend\libxslt',
    iconv=>'c:\prog\pgsql\depend\iconv',
    zlib=>'c:\prog\pgsql\depend\zlib'# --with-zlib=<path>
};

1;
