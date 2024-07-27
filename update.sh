#!/usr/bin

pg_ctl stop -D $PGDATA -m fast
#make clean
make -j10
sudo make install
