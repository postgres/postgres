#!/bin/sh

# === FIRST DRAFT ===

PG_HOME=/var/lib/postgres
PG_DATA=$PG_HOME/data

# default password for CA key
PASSWORD=postgresql

#
# this script creates the root (CA) certificate and
# server cert for PostgreSQL.  The OpenSSL applications
# must be in the path.
#

if [ $PG_HOME"." = "." -o $PG_DATA"." = "." ]
then
  /bin/echo You must define \$PG_HOME and \$PG_DATA before running this program.
  exit 0
fi

#
# generate DSA parameters file used for keys, if one does
# not already exist.
#
if [ ! -f $PG_HOME/dsa1024.pem -o -z $PG_HOME/dsa1024.pem ]
then
  openssl dsaparam -out $PG_HOME/dsa1024.pem 1024
fi

#
# generate CA directory tree and contents, if it does not already
# exist.
#
if [ ! -d $PG_HOME/CA ]
then
  /bin/mkdir $PG_HOME/CA;
fi
if [ ! -d $PG_HOME/CA/certs ]
then
  /bin/mkdir $PG_HOME/CA/certs
fi
if [ ! -d $PG_HOME/CA/crl ]
then
  /bin/mkdir $PG_HOME/CA/crl
fi
if [ ! -d $PG_HOME/CA/newcerts ]
then
  /bin/mkdir $PG_HOME/CA/newcerts
fi
if [ ! -d $PG_HOME/CA/private ]
then
  /bin/mkdir $PG_HOME/CA/private
  /bin/chmod 0700 $PG_HOME/CA/private
fi
if [ ! -f $PG_HOME/CA/index.txt ]
then
  /usr/bin/touch $PG_HOME/CA/index.txt
fi
if [ ! -f $PG_HOME/CA/serial ]
then
  /bin/echo 01 > $PG_HOME/CA/serial
fi

#
# generate root key, if one does not already exist.
#
if [ ! -f $PG_HOME/CA/private/cakey.pem -o -z $PG_HOME/CA/private/cakey.pem ]
then
  openssl gendsa $PG_HOME/dsa1024.pem |\
    openssl pkcs8 -topk8 -v2 bf -out $PG_HOME/CA/private/cakey.pem 
  /bin/chmod 0700 $PG_HOME/CA/private/cakey.pem
fi

#
# generate self-signed root certificate, if one does not already exist
#
if [ ! -f $PG_HOME/CA/cacert.pem -o -z $PG_HOME/CA/cacert.pem ]
then
  /bin/echo "Creating the root certificate...."
  /bin/echo ""
  openssl req -new -x509 -out $PG_HOME/CA/cacert.pem \
	-key $PG_HOME/CA/private/cakey.pem \
	-config $PG_HOME/root.conf
  link -s $PG_HOME/CA/cacert.pem $PG_DATA/root.crt
fi

#
# generate server key, if one does not already exist.
#
if [ ! -f $PG_DATA/server.key -o -z $PG_DATA/server.key ]
then
  openssl gendsa -out $PG_DATA/server.key $PG_HOME/dsa1024.pem
  /bin/chmod 0700 $PG_HOME/CA/private/cakey.pem
fi

#
# generate server certificate, if one does not already exist.
#
if [ ! -f $PG_DATA/server.crt -o -z $PG_DATA/server.crt ]
then
  /bin/echo "Creating the PostgreSQL server certificate...."
  /bin/echo ""
  openssl req -new -x509 -out $PG_DATA/server.self \
	-key $PG_DATA/server.key \
	-config $PG_HOME/server.conf
  if [ -f $PG_DATA/server.self ]
  then
    openssl ca -out $PG_DATA/server.crt -ss_cert $PG_DATA/server.self \
	-config $PG_HOME/root.conf -extensions svr_cert
    /bin/rm -f $PG_DATA/server.self
  fi
fi
