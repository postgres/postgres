#!/bin/sh
if [ -d /tmp/libpq ]
then
  rm -rf /tmp/libpq
fi
mkdir /tmp/libpq
#
mkdir -p /tmp/libpq/src/interfaces
cp -rp src/interfaces/libpq /tmp/libpq/src/interfaces/libpq
#
mkdir -p /tmp/libpq/src/include
cp -rp src/include/pg_config.h.in src/include/port src/include/libpq src/include/mb /tmp/libpq/src/include
for i in `echo c.h postgres_ext.h postgres_fe.h pg_config_manual.h pg_trace.h port.h getaddrinfo.h`
do
  cp src/include/${i} /tmp/libpq/src/include
done
#
mkdir -p /tmp/libpq/src/backend/port
cp -rp src/backend/port /tmp/libpq/src/backend
#
cp -rp src/template src/port src/makefiles /tmp/libpq/src
#
mkdir -p /tmp/libpq/src/backend/libpq
cp src/backend/libpq/ip.c src/backend/libpq/md5.c /tmp/libpq/src/backend/libpq
#
mkdir -p /tmp/libpq/src/backend/utils/mb
cp src/backend/utils/mb/encnames.c src/backend/utils/mb/wchar.c  /tmp/libpq/src/backend/utils/mb
cp src/Makefile.global.in src/Makefile.shlib /tmp/libpq/src
cp aclocal.m4 configure.in configure GNUmakefile.in /tmp/libpq
cp -rp config /tmp/libpq
cd /tmp/libpq
find . -type d -name CVS -exec rm -rf {} \;
sed -i.bak 's/src\/backend\/access\/common\/heaptuple.c/src\/interfaces\/libpq\/fe-auth.c/' configure.in
autoconf
