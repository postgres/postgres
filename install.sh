#!/usr/bin
./configure --enable-depend --enable-cassert --enable-debug CFLAGS="-ggdb -Og"

make -j10
sudo make install
cd contrib
make
sudo make install
sudo chgrp -R $USER /usr/local/pgsql
sudo chown -R $USER /usr/local/pgsql
sudo mkdir -p /var/postgresql/data
sudo chown -R $USER /var/postgresql/data
sudo chgrp -R $USER /var/postgresql/data

# postgresql config
export PGHOME=/usr/local/pgsql
export PGDATA=/var/postgresql/data
export PGHOST=/tmp
export PATH=$PGHOME/bin:$PATH
export MANPATH=$PGHOME/share/man:$MANPATH
# export LANG=en_US.utf8
export DATE=`date +"%Y-%m-%d %H:%M:%S"`
export LD_LIBRARY_PATH=$PGHOME/lib:$LD_LIBRARY_PATH
alias pg_start='pg_ctl start -D $PGDATA'
alias pg_stop='pg_ctl stop -D $PGDATA -m fast'
initdb -D $PGDATA

echo 0 | sudo tee /proc/sys/kernel/yama/ptrace_scope


psql -d postgres -c 'create database ycsb'