#!/bin/bash
set -e

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

INSTALL_DIR="/Users/cangokmen/Desktop/QuART-VACUUM/pg_install"

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}Postgres 17 VACUUM Radix Tree Test${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""

# Check if PostgreSQL is running
if ! "$INSTALL_DIR/bin/pg_ctl" -D "/Users/cangokmen/Desktop/QuART-VACUUM/pg_data" status > /dev/null 2>&1; then
    echo -e "${YELLOW}PostgreSQL is not running. Starting...${NC}"
    ./start_postgres.sh
    sleep 2
fi

# Check version
echo -e "${YELLOW}Checking PostgreSQL version...${NC}"
PG_VERSION=$("$INSTALL_DIR/bin/psql" -d postgres -t -c "SHOW server_version;" | xargs)
echo "  Version: $PG_VERSION"
echo ""

# Run test
echo -e "${YELLOW}Running VACUUM test...${NC}"
"$INSTALL_DIR/bin/psql" -d postgres -f test_vacuum_radix_tree.sql

echo ""
echo -e "${GREEN}Test completed!${NC}"
echo ""
echo "To view logs: tail -f pg_data/log/postgresql-*.log"
echo "To monitor vacuum: $INSTALL_DIR/bin/psql -d postgres -f test_vacuum_monitor.sql"
