#!/usr/bin

usage() {
  echo "Usage: $0 -t <arg1> -s <arg2> -r <arg3> -w <arg4> -T <arg5> -a"
  echo "Options:"
}

N_THREAD=8
N_READ=0.5
SKEWNESS=0.5
RUN_TIME=1

while getopts ":t:s:r:T:a" opt; do
  case $opt in
      a)
          LAUNCH=1
          ;;
      T)
          RUN_TIME=$OPTARG
          ;;
      t)
          N_THREAD=$OPTARG
          ;;
      s)
          SKEWNESS=$OPTARG
          ;;
      r)
          N_READ=$OPTARG
          ;;
      \?)
          echo "Invalid option: -$OPTARG" >&2
          usage
          ;;
      :)
          echo "Option -$OPTARG requires an argument." >&2
          usage
          ;;
  esac
done

if [ -n "$LAUNCH" ]; then
  echo "Cleanup database"
  bash ./scripts/cleanup.sh
  echo "Kill pending pg_service"
  pg_ctl stop -D $PGDATA -m fast
  echo "Restart pg_service"
  pg_ctl start -D $PGDATA > ./run_log.txt
fi

COMMON_OPTIONS="-node=c -local=true -addr=127.0.0.1:5001"
# For becnhmarking of SSI.

cd ./oltp_clients || exit

# for learned.
./bin/fc-server $COMMON_OPTIONS \
  -c=$N_THREAD \
  -runtime=$RUN_TIME \
  -warmup=2 \
  -lock=learned \
  -rw=$N_READ \
  -iso=none \
  -skew=$SKEWNESS

if [ -n "$LAUNCH" ]; then
  pg_ctl stop -D $PGDATA -m fast
fi