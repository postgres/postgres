#!/bin/sh

echo \$HOME = $HOME

CLIENTDIR=$HOME/.postgresql

#
# copy root certificate, if necessary
#
if [ ! -f $CLIENTDIR/root.crt -o -z $CLIENTDIR/root.crt ]
then
  if [ -f /etc/postgresql/root.crt ]
  then
    /bin/cp -p /etc/postgresql/root.crt $CLIENTDIR
  fi
fi

#
# generate client key, if one does not already exist.
#
if [ ! -f $CLIENTDIR/postgresql.key -o -z $CLIENTDIR/postgresql.key ]
then
  if [ ! -f /etc/postgresql/dsa1024.pem -o -z /etc/postgresql/dsa1024.pem ]
  then
    /bin/echo "You must get the dsa1024.pem file from your DBA."
    exit 0
  fi
  openssl gendsa /etc/postgresql/dsa1024.pem |\
    openssl pkcs8 -topk8 -v2 bf -out $CLIENTDIR/postgresql.key
  /bin/chmod 0600 $CLIENTDIR/postgresql.key
fi

#
# generate client SS certificate, if one does not already exist.
#
if [ ! -f $CLIENTDIR/postgresql.crt -o -z $CLIENTDIR/postgresql.crt ]
then
  if [ ! -f $CLIENTDIR/postgresql.pem -o -z $CLIENTDIR/postgresql.pem ]
  then
    /bin/echo "Creating client certificate...."
    /bin/echo ""
    openssl req -new -x509 -out $CLIENTDIR/postgresql.pem \
      -key $CLIENTDIR/postgresql.key -config /etc/postgresql/client.conf
    /bin/echo ""
    /bin/cat << EOM

You must now provide a copy of your ~/.postgresql/postgresql.pem file
to your DBA for them to sign.  When they have done so, you should rerun
this application.
EOM
  else
    cp -p $CLIENTDIR/postgresql.pem $CLIENTDIR/postgresql.crt
  fi
fi
