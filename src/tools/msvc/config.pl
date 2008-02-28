# Configuration arguments for vcbuild.
use strict;
use warnings;

our $config = {
    asserts=>0,			# --enable-cassert
    integer_datetimes=>0,   # --enable-integer-datetimes
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
