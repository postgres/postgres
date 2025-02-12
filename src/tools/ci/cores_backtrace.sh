#! /bin/sh

if [ $# -ne 2 ]; then
    echo "cores_backtrace.sh <os> <directory>"
    exit 1
fi

os=$1
directory=$2

case $os in
    freebsd|linux|macos|netbsd|openbsd)
    ;;
    *)
        echo "unsupported operating system ${os}"
        exit 1
    ;;
esac

first=1
for corefile in $(find "$directory" -type f) ; do
    if [ "$first" -eq 1 ]; then
        first=0
    else
        # to make it easier to separate the different crash reports
        echo -e '\n\n'
    fi

    if [ "$os" = 'macos' ] || [ "$os" = 'openbsd' ]; then
        lldb -c $corefile --batch -o 'thread backtrace all' -o 'quit'
    else
        auxv=$(gdb --quiet --core ${corefile} --batch -ex 'info auxv' 2>/dev/null)
        if [ $? -ne 0 ]; then
            echo "could not process ${corefile}"
            continue
        fi

        if [ "$os" = 'freebsd' ]; then
            binary=$(echo "$auxv" | grep AT_EXECPATH | perl -pe "s/^.*\"(.*)\"\$/\$1/g")
        elif [ "$os" = 'netbsd' ]; then
            binary=$(echo "$auxv" | grep AT_SUN_EXECNAME | perl -pe "s/^.*\"(.*)\"\$/\$1/g")
        elif [ "$os" = 'linux' ]; then
            binary=$(echo "$auxv" | grep AT_EXECFN | perl -pe "s/^.*\"(.*)\"\$/\$1/g")
        else
            echo 'should not get here'
            exit 1
        fi

        echo "dumping ${corefile} for ${binary}"
        gdb --batch --quiet -ex "thread apply all bt full" -ex "quit" "$binary" "$corefile" 2>/dev/null
    fi
done
