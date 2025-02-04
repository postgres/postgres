SCRIPT_DIR="$(cd -- "$(dirname "$0")" >/dev/null 2>&1; pwd -P)"
cd "$SCRIPT_DIR/../"

if ! test -f src/backend/postgres; then
  echo "src/backend/postgres doesn't exists, run make-build.sh first in debug mode"
  exit 1
fi

if ! test -f contrib/pg_tde/pg_tde.so; then
  echo "contrib/pg_tde/pg_tde.so doesn't exists, run make-build.sh first in debug mode"
  exit 1
fi

objdump -W src/backend/postgres |\
    egrep -A3 DW_TAG_typedef |\
    perl -e ' while (<>) { chomp; @flds = split;next unless (1 < @flds);\
        next if $flds[0]  ne "DW_AT_name" && $flds[1] ne "DW_AT_name";\
        next if $flds[-1] =~ /^DW_FORM_str/;\
        print $flds[-1],"\n"; }'  |\
    sort | uniq > percona.typedefs

objdump -W contrib/pg_tde/pg_tde.so |\
    egrep -A3 DW_TAG_typedef |\
    perl -e ' while (<>) { chomp; @flds = split;next unless (1 < @flds);\
        next if $flds[0]  ne "DW_AT_name" && $flds[1] ne "DW_AT_name";\
        next if $flds[-1] =~ /^DW_FORM_str/;\
        print $flds[-1],"\n"; }'  |\
    sort | uniq > tde.typedefs

# Combine with original typedefs
cat percona.typedefs tde.typedefs src/tools/pgindent/typedefs.list | sort | uniq > combined.typedefs
