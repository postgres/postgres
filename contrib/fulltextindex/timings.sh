#!/bin/sh

PATH=${PATH}:/usr/local/pgsql/bin
TIMEFORMAT="%3Uu %3Ss %lR %P%%"
export PATH TIMEFORMAT

case "$1" in
  -n)
	  trashing=0
	  ;;
  *)
	 trashing=1
	 ;;
esac

echo "TESTING ON UNCLUSTERED FTI"

# trash disk
if [ $trashing = 1 ]
then
  echo "trashing"
  psql -q -n -o /dev/null -c "select count(*) from product;" test
else
  echo
fi

Q="select count(p.oid) from product p, artist_fti f1, artist_fti f2
        where
                f1.string ~ '^lapton' and f2.string ~ '^ric' and
                f1.id=p.oid and f2.id=p.oid;"

echo -n "1: ^lapton and ^ric           : "
time psql -q -n -o /dev/null -c "$Q" test
echo -n "2: ^lapton and ^ric           : "
time psql -q -n -o /dev/null -c "$Q" test
echo -n "3: ^lapton and ^ric           : "
time psql -q -n -o /dev/null -c "$Q" test

# trash disk
if [ $trashing = 1 ]
then
  echo "trashing"
  psql -q -n -o /dev/null -c "select count(*) from product;" test
else
  echo
fi

Q="select count(p.oid) from product p, artist_fti f1, artist_fti f2
        where
                f1.string ~ '^lling' and f2.string ~ '^tones' and
                f1.id=p.oid and f2.id=p.oid;"

echo -n "1: ^lling and ^tones          : "
time psql -q -n -o /dev/null -c "$Q" test
echo -n "2: ^lling and ^tones          : "
time psql -q -n -o /dev/null -c "$Q" test
echo -n "3: ^lling and ^tones          : "
time psql -q -n -o /dev/null -c "$Q" test

# trash disk
if [ $trashing = 1 ]
then
  echo "trashing"
  psql -q -n -o /dev/null -c "select count(*) from product;" test
else
  echo
fi

Q="select count(p.oid) from product p, artist_fti f1, artist_fti f2
        where
                f1.string ~ '^aughan' and f2.string ~ '^evie' and
                f1.id=p.oid and f2.id=p.oid;"

echo -n "1: ^aughan and ^evie          : "
time psql -q -n -o /dev/null -c "$Q" test
echo -n "2: ^aughan and ^evie          : "
time psql -q -n -o /dev/null -c "$Q" test
echo -n "3: ^aughan and ^evie          : "
time psql -q -n -o /dev/null -c "$Q" test

# trash disk
if [ $trashing = 1 ]
then
  echo "trashing"
  psql -q -n -o /dev/null -c "select count(*) from product;" test
else
  echo
fi

Q="select count(p.oid) from product p, artist_fti f1
        where
                f1.string ~ '^lling' and 
                p.oid=f1.id;"

echo -n "1: ^lling                     : "
time psql -q -n -o /dev/null -c "$Q" test
echo -n "2: ^lling                     : "
time psql -q -n -o /dev/null -c "$Q" test
echo -n "3: ^lling                     : "
time psql -q -n -o /dev/null -c "$Q" test

# trash disk
if [ $trashing = 1 ]
then
  echo "trashing"
  psql -q -n -o /dev/null -c "select count(*) from product;" test
else
  echo
fi

Q="select count(p.oid) from product p, artist_fti f1, artist_fti f2, artist_fti f3
        where
                f1.string ~ '^stev' and
                f2.string ~ '^ray' and
                f3.string ~ '^vaugh' and
                p.oid=f1.id and p.oid=f2.id and p.oid=f3.id;"

echo -n "1: ^stev and ^ray and ^vaugh  : "
time psql -q -n -o /dev/null -c "$Q" test
echo -n "2: ^stev and ^ray and ^vaugh  : "
time psql -q -n -o /dev/null -c "$Q" test
echo -n "3: ^stev and ^ray and ^vaugh  : "
time psql -q -n -o /dev/null -c "$Q" test

# trash disk
if [ $trashing = 1 ]
then
  echo "trashing"
  psql -q -n -o /dev/null -c "select count(*) from product;" test
else
  echo
fi

Q="select count(*) from artist_fti where string ~ '^lling';"

echo -n "1: ^lling (no join)           : "
time psql -q -n -o /dev/null -c "$Q" test
echo -n "2: ^lling (no join)           : "
time psql -q -n -o /dev/null -c "$Q" test
echo -n "3: ^lling (no join)           : "
time psql -q -n -o /dev/null -c "$Q" test

# trash disk
if [ $trashing = 1 ]
then
  echo "trashing"
  psql -q -n -o /dev/null -c "select count(*) from product;" test
else
  echo
fi

Q="select count(*) from artist_fti where string ~ '^vaughan';"

echo -n "1: ^vaughan (no join)         : "
time psql -q -n -o /dev/null -c "$Q" test
echo -n "2: ^vaughan (no join)         : "
time psql -q -n -o /dev/null -c "$Q" test
echo -n "3: ^vaughan (no join)         : "
time psql -q -n -o /dev/null -c "$Q" test

# trash disk
if [ $trashing = 1 ]
then
  echo "trashing"
  psql -q -n -o /dev/null -c "select count(*) from product;" test
else
  echo
fi

Q="select count(*) from artist_fti where string ~ '^rol';"

echo -n "1: ^rol (no join)             : "
time psql -q -n -o /dev/null -c "$Q" test
echo -n "2: ^rol (no join)             : "
time psql -q -n -o /dev/null -c "$Q" test
echo -n "3: ^rol (no join)             : "
time psql -q -n -o /dev/null -c "$Q" test

echo
echo "TESTING ON CLUSTERED FTI"

# trash disk
if [ $trashing = 1 ]
then
  echo "trashing"
  psql -q -n -o /dev/null -c "select count(*) from product;" test
else
  echo
fi

Q="select count(p.oid) from product p, clustered f1, clustered f2
        where
                f1.string ~ '^lapton' and f2.string ~ '^ric' and
                f1.id=p.oid and f2.id=p.oid;"

echo -n "1: ^lapton and ^ric           : "
time psql -q -n -o /dev/null -c "$Q" test
echo -n "2: ^lapton and ^ric           : "
time psql -q -n -o /dev/null -c "$Q" test
echo -n "3: ^lapton and ^ric           : "
time psql -q -n -o /dev/null -c "$Q" test

# trash disk
if [ $trashing = 1 ]
then
  echo "trashing"
  psql -q -n -o /dev/null -c "select count(*) from product;" test
else
  echo
fi

Q="select count(p.oid) from product p, clustered f1, clustered f2
        where
                f1.string ~ '^lling' and f2.string ~ '^tones' and
                f1.id=p.oid and f2.id=p.oid;"

echo -n "1: ^lling and ^tones          : "
time psql -q -n -o /dev/null -c "$Q" test
echo -n "2: ^lling and ^tones          : "
time psql -q -n -o /dev/null -c "$Q" test
echo -n "3: ^lling and ^tones          : "
time psql -q -n -o /dev/null -c "$Q" test

# trash disk
if [ $trashing = 1 ]
then
  echo "trashing"
  psql -q -n -o /dev/null -c "select count(*) from product;" test
else
  echo
fi

Q="select count(p.oid) from product p, clustered f1, clustered f2
        where
                f1.string ~ '^aughan' and f2.string ~ '^evie' and
                f1.id=p.oid and f2.id=p.oid;"

echo -n "1: ^aughan and ^evie          : "
time psql -q -n -o /dev/null -c "$Q" test
echo -n "2: ^aughan and ^evie          : "
time psql -q -n -o /dev/null -c "$Q" test
echo -n "3: ^aughan and ^evie          : "
time psql -q -n -o /dev/null -c "$Q" test

# trash disk
if [ $trashing = 1 ]
then
  echo "trashing"
  psql -q -n -o /dev/null -c "select count(*) from product;" test
else
  echo
fi

Q="select count(p.oid) from product p, clustered f1
        where
                f1.string ~ '^lling' and 
                p.oid=f1.id;"

echo -n "1: ^lling                     : "
time psql -q -n -o /dev/null -c "$Q" test
echo -n "2: ^lling                     : "
time psql -q -n -o /dev/null -c "$Q" test
echo -n "3: ^lling                     : "
time psql -q -n -o /dev/null -c "$Q" test

# trash disk
if [ $trashing = 1 ]
then
  echo "trashing"
  psql -q -n -o /dev/null -c "select count(*) from product;" test
else
  echo
fi

Q="select count(p.oid) from product p, clustered f1, clustered f2, clustered f3
        where
                f1.string ~ '^stev' and
                f2.string ~ '^ray' and
                f3.string ~ '^vaugh' and
                p.oid=f1.id and p.oid=f2.id and p.oid=f3.id;"

echo -n "1: ^stev and ^ray and ^vaugh  : "
time psql -q -n -o /dev/null -c "$Q" test
echo -n "2: ^stev and ^ray and ^vaugh  : "
time psql -q -n -o /dev/null -c "$Q" test
echo -n "3: ^stev and ^ray and ^vaugh  : "
time psql -q -n -o /dev/null -c "$Q" test

# trash disk
if [ $trashing = 1 ]
then
  echo "trashing"
  psql -q -n -o /dev/null -c "select count(*) from product;" test
else
  echo
fi

Q="select count(*) from clustered where string ~ '^lling';"

echo -n "1: ^lling (no join)           : "
time psql -q -n -o /dev/null -c "$Q" test
echo -n "2: ^lling (no join)           : "
time psql -q -n -o /dev/null -c "$Q" test
echo -n "3: ^lling (no join)           : "
time psql -q -n -o /dev/null -c "$Q" test

# trash disk
if [ $trashing = 1 ]
then
  echo "trashing"
  psql -q -n -o /dev/null -c "select count(*) from product;" test
else
  echo
fi

Q="select count(*) from clustered where string ~ '^vaughan';"

echo -n "1: ^vaughan (no join)         : "
time psql -q -n -o /dev/null -c "$Q" test
echo -n "2: ^vaughan (no join)         : "
time psql -q -n -o /dev/null -c "$Q" test
echo -n "3: ^vaughan (no join)         : "
time psql -q -n -o /dev/null -c "$Q" test

# trash disk
if [ $trashing = 1 ]
then
  echo "trashing"
  psql -q -n -o /dev/null -c "select count(*) from product;" test
else
  echo
fi

Q="select count(*) from clustered where string ~ '^rol';"

echo -n "1: ^rol (no join)             : "
time psql -q -n -o /dev/null -c "$Q" test
echo -n "2: ^rol (no join)             : "
time psql -q -n -o /dev/null -c "$Q" test
echo -n "3: ^rol (no join)             : "
time psql -q -n -o /dev/null -c "$Q" test









