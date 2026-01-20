#!/bin/bash
# Build and setup PostgreSQL for testing VACUUM with Adaptive Radix Trees
# This script compiles PostgreSQL from source and sets up a test environment

set -e

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

WORKSPACE_DIR="/Users/cangokmen/Desktop/QuART-VACUUM"
BUILD_DIR="$WORKSPACE_DIR/build"
INSTALL_DIR="$WORKSPACE_DIR/pg_install"
DATA_DIR="$WORKSPACE_DIR/pg_data"

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}PostgreSQL Build & Setup Script${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""

# Step 0: Check and install dependencies
echo -e "${BLUE}Step 0: Checking dependencies...${NC}"

# Check for pkg-config
if ! command -v pkg-config &> /dev/null; then
    echo -e "${YELLOW}pkg-config not found. Checking for Homebrew...${NC}"
    if command -v brew &> /dev/null; then
        echo -e "${YELLOW}Installing pkg-config via Homebrew...${NC}"
        brew install pkg-config
    else
        echo -e "${RED}Error: pkg-config not found and Homebrew not available.${NC}"
        echo -e "${YELLOW}Please install Homebrew from https://brew.sh/ or install pkg-config manually.${NC}"
        exit 1
    fi
fi

# Check for ICU (optional, we'll disable if not available)
ICU_OPTION=""
if pkg-config --exists icu-uc icu-i18n 2>/dev/null; then
    echo -e "${GREEN}✓ ICU found, will build with ICU support${NC}"
    ICU_OPTION="--with-icu"
else
    echo -e "${YELLOW}ICU not found, building without ICU support${NC}"
    ICU_OPTION="--without-icu"
fi

echo -e "${GREEN}✓ Dependencies checked${NC}"
echo ""

# Step 1: Configure
echo -e "${BLUE}Step 1: Configuring PostgreSQL build...${NC}"
cd "$WORKSPACE_DIR"

if [ ! -f "configure" ]; then
    echo -e "${RED}Error: configure script not found. Run autoconf first.${NC}"
    exit 1
fi

# Create build directory
mkdir -p "$BUILD_DIR"
mkdir -p "$INSTALL_DIR"

# Configure with debugging enabled
./configure \
    --prefix="$INSTALL_DIR" \
    --enable-debug \
    --enable-cassert \
    $ICU_OPTION \
    CFLAGS="-O0 -g3" \
    || { echo -e "${RED}Configuration failed${NC}"; exit 1; }

echo -e "${GREEN}✓ Configuration complete${NC}"
echo ""

# Step 2: Compile
echo -e "${BLUE}Step 2: Compiling PostgreSQL (this may take 5-10 minutes)...${NC}"
make -j$(sysctl -n hw.ncpu) || { echo -e "${RED}Compilation failed${NC}"; exit 1; }

echo -e "${GREEN}✓ Compilation complete${NC}"
echo ""

# Step 3: Install
echo -e "${BLUE}Step 3: Installing PostgreSQL to $INSTALL_DIR...${NC}"
make install || { echo -e "${RED}Installation failed${NC}"; exit 1; }

echo -e "${GREEN}✓ Installation complete${NC}"
echo ""

# Step 4: Initialize database cluster
echo -e "${BLUE}Step 4: Initializing database cluster...${NC}"

# Remove old data directory if it exists
if [ -d "$DATA_DIR" ]; then
    echo -e "${YELLOW}Removing existing data directory...${NC}"
    rm -rf "$DATA_DIR"
fi

"$INSTALL_DIR/bin/initdb" -D "$DATA_DIR" || { echo -e "${RED}initdb failed${NC}"; exit 1; }

echo -e "${GREEN}✓ Database cluster initialized${NC}"
echo ""

# Step 5: Configure PostgreSQL for testing
echo -e "${BLUE}Step 5: Configuring PostgreSQL for VACUUM testing...${NC}"

cat >> "$DATA_DIR/postgresql.conf" <<EOF

# Configuration for VACUUM testing
log_autovacuum_min_duration = 0
log_min_messages = log
logging_collector = on
log_directory = 'log'
log_filename = 'postgresql-%Y-%m-%d_%H%M%S.log'
log_statement = 'none'
log_duration = off

# Performance settings
shared_buffers = 256MB
maintenance_work_mem = 64MB
max_wal_size = 2GB
checkpoint_timeout = 30min

# Make sure autovacuum is enabled
autovacuum = on
autovacuum_naptime = 1s
EOF

echo -e "${GREEN}✓ PostgreSQL configured${NC}"
echo ""

# Step 6: Create helper scripts
echo -e "${BLUE}Step 6: Creating helper scripts...${NC}"

# Start script
cat > "$WORKSPACE_DIR/start_postgres.sh" <<'EOF'
#!/bin/bash
INSTALL_DIR="/Users/cangokmen/Desktop/QuART-VACUUM/pg_install"
DATA_DIR="/Users/cangokmen/Desktop/QuART-VACUUM/pg_data"

echo "Starting PostgreSQL..."
"$INSTALL_DIR/bin/pg_ctl" -D "$DATA_DIR" -l "$DATA_DIR/logfile" start

echo "Waiting for PostgreSQL to start..."
sleep 2

echo ""
echo "PostgreSQL is running!"
echo "Connection info:"
echo "  Host: localhost"
echo "  Port: 5432"
echo "  Database: postgres"
echo ""
echo "To connect: $INSTALL_DIR/bin/psql -d postgres"
echo "To stop: ./stop_postgres.sh"
EOF
chmod +x "$WORKSPACE_DIR/start_postgres.sh"

# Stop script
cat > "$WORKSPACE_DIR/stop_postgres.sh" <<'EOF'
#!/bin/bash
INSTALL_DIR="/Users/cangokmen/Desktop/QuART-VACUUM/pg_install"
DATA_DIR="/Users/cangokmen/Desktop/QuART-VACUUM/pg_data"

echo "Stopping PostgreSQL..."
"$INSTALL_DIR/bin/pg_ctl" -D "$DATA_DIR" stop
EOF
chmod +x "$WORKSPACE_DIR/stop_postgres.sh"

# Status script
cat > "$WORKSPACE_DIR/status_postgres.sh" <<'EOF'
#!/bin/bash
INSTALL_DIR="/Users/cangokmen/Desktop/QuART-VACUUM/pg_install"
DATA_DIR="/Users/cangokmen/Desktop/QuART-VACUUM/pg_data"

"$INSTALL_DIR/bin/pg_ctl" -D "$DATA_DIR" status
EOF
chmod +x "$WORKSPACE_DIR/status_postgres.sh"

# Update test script to use local PostgreSQL
cat > "$WORKSPACE_DIR/run_vacuum_test.sh" <<'EOF'
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
EOF
chmod +x "$WORKSPACE_DIR/run_vacuum_test.sh"

echo -e "${GREEN}✓ Helper scripts created${NC}"
echo ""

# Summary
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}Build Complete!${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""
echo -e "${YELLOW}Next steps:${NC}"
echo ""
echo "1. Start PostgreSQL:"
echo "   ${BLUE}./start_postgres.sh${NC}"
echo ""
echo "2. Run the VACUUM test:"
echo "   ${BLUE}./run_vacuum_test.sh${NC}"
echo ""
echo "3. Monitor vacuum progress (in another terminal):"
echo "   ${BLUE}$INSTALL_DIR/bin/psql -d postgres -f test_vacuum_monitor.sql${NC}"
echo ""
echo "4. When done, stop PostgreSQL:"
echo "   ${BLUE}./stop_postgres.sh${NC}"
echo ""
echo -e "${YELLOW}Additional commands:${NC}"
echo "  Check status: ./status_postgres.sh"
echo "  View logs: tail -f pg_data/log/postgresql-*.log"
echo "  Connect: $INSTALL_DIR/bin/psql -d postgres"
echo ""
