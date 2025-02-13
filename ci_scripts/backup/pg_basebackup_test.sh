#!/bin/bash
set -e  # Exit on errors
set -u  # Treat unset variables as errors
set -o pipefail  # Stop on pipeline errors

# Configuration Variables
PG_VERSION="17"
INSTALL_DIR="/usr/lib/postgresql/$PG_VERSION"
PG_DATA="/var/lib/postgresql/$PG_VERSION/main"
CONF_DIR="${PG_DATA}"
HOST="127.0.0.1"

BASE_DIR=$(dirname "$(realpath "$0")") # Get the directory of the script
BACKUP_DIR="/var/lib/postgresql/backups"
FULL_BACKUP_DIR="${BACKUP_DIR}/full_backup"
INCREMENTAL_BACKUP_DIR="${BACKUP_DIR}/incremental_backup"
RESTORE_DIR="${BACKUP_DIR}/restore"
KEYLOCATION="${BASE_DIR}/pg_tde_test_keyring.per"
PG_HBA="${CONF_DIR}/pg_hba.conf"
RESTORE_PG_HBA="$RESTORE_DIR/pg_hba.conf"
PG_PORT="5433"
PG_USER="postgres"
DB_NAME="testdb"
REPL_USER="replicator"
REPL_PASS="replicator_pass"
REPO_TYPE="release" # release or testing or experimental
REPO_VERSION="17.2" # 17.0 or 17.1 or 17.2
TABLE_NAME="emp"
SEARCHED_TEXT="SMITH"

SQL_DIR="${BASE_DIR}/sql"
EXPECTED_DIR="${BASE_DIR}/expected"
ACTUAL_DIR="${BASE_DIR}/actual"

LOGFILE="${BACKUP_DIR}/backup_restore.log"

sudo -u "$PG_USER" mkdir -p "$BACKUP_DIR" "$FULL_BACKUP_DIR" "$INCREMENTAL_BACKUP_DIR" "$RESTORE_DIR" "$EXPECTED_DIR" "$ACTUAL_DIR"

# Function to log messages
log_message() {
    echo "$(date '+%Y-%m-%d %H:%M:%S') - $1" | sudo -u "$PG_USER" tee -a "$LOGFILE"
}

# Step 1: Setup PostgreSQL Percona repo
setup_percona_repo(){
    #          RH derivatives      and          Amazon Linux
    if [[ -f /etc/redhat-release ]] || [[ -f /etc/system-release ]]; then
        # These are the same installation steps as you will find them here: https://percona.github.io/pg_tde/main/yum.html
        sudo dnf module disable postgresql llvm-toolset
        sudo yum -y install https://repo.percona.com/yum/percona-release-latest.noarch.rpm
        sudo percona-release enable ppg-${REPO_VERSION} ${REPO_TYPE} -y
        sudo dnf config-manager --set-enabled ol9_codeready_builder
        sudo yum update -y
    elif [[ -f /etc/debian_version ]]; then
        # These are the same installation steps as you will find them here: https://percona.github.io/pg_tde/main/apt.html
        sudo apt-get install -y wget gnupg2 curl lsb-release
        sudo wget https://repo.percona.com/apt/percona-release_latest.generic_all.deb
        sudo dpkg -i percona-release_latest.generic_all.deb
        sudo percona-release enable ppg-${REPO_VERSION} ${REPO_TYPE} -y
        sudo apt-get update -y
    else
        msg "ERROR: Unsupported operating system"
        exit 1
    fi
}

# Step 2: Install Percona PostgreSQL and start server
install_packages() {
  #          RH derivatives      and          Amazon Linux
if [[ -f /etc/redhat-release ]] || [[ -f /etc/system-release ]]; then 
  sudo yum -y install percona-postgresql-client-common percona-postgresql-common percona-postgresql-server-dev-all percona-postgresql${PG_VERSION} percona-postgresql${PG_VERSION}-contrib percona-postgresql${PG_VERSION}-devel percona-postgresql${PG_VERSION}-libs
  sudo /usr/pgsql-${PG_VERSION}/bin/postgresql-${PG_VERSION}-setup initdb
  #sudo systemctl start postgresql-${PG_VERSION}
  start_server "$PG_DATA"
elif [[ -f /etc/debian_version ]]; then
  sudo apt-get install -y percona-postgresql-${PG_VERSION} percona-postgresql-contrib percona-postgresql-server-dev-all
else
  msg "ERROR: Unsupported operating system"
  exit 1
fi
}

# Step 3: Setup PostgreSQL and Create Sample Data
setup_postgresql() {
    echo "Setting up PostgreSQL , enable tde_heap and creating sample data..."
    sudo -u "$PG_USER" psql -p $PG_PORT -c "ALTER SYSTEM SET shared_preload_libraries ='pg_tde';"
    sudo -u "$PG_USER" psql -p $PG_PORT -c "ALTER SYSTEM SET summarize_wal = 'on';"
    sudo -u "$PG_USER" psql -p $PG_PORT -c "ALTER SYSTEM SET wal_level = 'replica';"
    sudo -u "$PG_USER" psql -p $PG_PORT -c "ALTER SYSTEM SET wal_log_hints = 'on';"
    #sudo -u "$PG_USER" psql -p $PG_PORT -c "ALTER SYSTEM SET pg_tde.wal_encrypt = on;"
    if [[ -f /etc/debian_version ]]; then
        restart_server "$PG_DATA"
        KEYLOCATION="/var/lib/postgresql/pg_tde_test_keyring.per"
    else
        restart_server "$PG_DATA"
        KEYLOCATION="/var/lib/pgsql/pg_tde_test_keyring.per"
    fi
}

# Setup TDE Heap
setup_tde_heap(){
    # create a sample database
    echo "Create a sample database"
    sudo -u "$PG_USER" psql -p $PG_PORT -c "DROP DATABASE IF EXISTS $DB_NAME;"
    sudo -u "$PG_USER" psql -p $PG_PORT -c "CREATE DATABASE $DB_NAME;"
    sudo -u "$PG_USER" psql -d "$DB_NAME" -p "$PG_PORT" -c "CREATE EXTENSION IF NOT EXISTS pg_tde;"
    sudo -u "$PG_USER" psql -d "$DB_NAME" -p "$PG_PORT" -c "SELECT pg_tde_add_key_provider_file('file-vault','$KEYLOCATION');"
    sudo -u "$PG_USER" psql -d "$DB_NAME" -p "$PG_PORT" -c "SELECT pg_tde_set_principal_key('test-db-master-key','file-vault');"
    sudo -u "$PG_USER" psql -p $PG_PORT -c "ALTER DATABASE $DB_NAME SET default_table_access_method='tde_heap';"
    sudo -u "$PG_USER" psql -p $PG_PORT -c "SELECT pg_reload_conf();"
}

# Insert some sample data for testing purposes
populate_sample_data(){
    # Create a sample database objects like tables, view, indexes etc
    run_sql "sample_data.sql"
    if [[ $? -ne 0 ]]; then
        log_message "Error: Failed to create sample data. ❌"
        return 1
    else
        log_message "Sample data created successfully. ✅ "
    fi
}

# Function to run SQL files and capture results
run_sql() {
    local sql_file=$1
    log_message "[RUNNING] $sql_file"
    sudo -u $PG_USER bash <<EOF
    psql -d $DB_NAME -p "$PG_PORT"  -f "$SQL_DIR/$sql_file" >> $LOGFILE 2>&1
EOF
}

# Create expected files before running backups
create_expected_output() {
    local sql_file=$1
    log_message "Creating expected output for $sql_file..."
    sudo -u $PG_USER bash <<EOF
    psql -d $DB_NAME -p $PG_PORT -e -a -f "$SQL_DIR/${sql_file}.sql" -t -A > "$EXPECTED_DIR/${sql_file}.out" 2>&1
EOF
}

# Function to verify expected vs actual output
verify_output() {
    local sql_file=$1
    local expected_file="$EXPECTED_DIR/$2"
    mkdir -p $ACTUAL_DIR
    local actual_file="$ACTUAL_DIR/${2%.expected}"

    sudo -u $PG_USER bash <<EOF
    psql -d $DB_NAME -p $PG_PORT -e -a -f "$SQL_DIR/$sql_file" -t -A > "$actual_file" 2>&1
EOF
    if diff -q "$actual_file" "$expected_file" > /dev/null; then
        log_message "$sql_file matches expected output. ✅"
    else
        log_message "$sql_file output mismatch. ❌"
        diff "$actual_file" "$expected_file" |  sudo -u "$PG_USER" tee -a $LOGFILE
    fi
}

# Function to Update pg_hba.conf for Backup
update_pg_hba_for_backup() {
    echo "Updating pg_hba.conf for backup..."
    sudo -u "$PG_USER" bash -c "echo 'host replication $REPL_USER 127.0.0.1/32 md5' >> $PG_HBA"
    sudo -u "$PG_USER" bash -c "echo 'host replication $REPL_USER ::1/128 md5' >> $PG_HBA"
    #sudo systemctl reload postgresql
    restart_server "$PG_DATA"
    echo "pg_hba.conf updated for backup. ✅ "
}

# Configure Replication user for pg_basebackup
setup_replication_user() {
    echo "Configuring replication user..."
    # Create a replication user in PostgreSQL

    #CREATE ROLE $REPL_USER WITH REPLICATION LOGIN PASSWORD '$REPL_PASS';
    sudo -u "$PG_USER" psql -p $PG_PORT -c "
        DO \$\$ 
        BEGIN 
            IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = '$REPL_USER') THEN
                EXECUTE format('CREATE ROLE %I WITH LOGIN PASSWORD %L', '$REPL_USER', '$REPL_PASS');
            END IF;
        END \$\$;"

    # Update pg_hba.conf to allow replication
    update_pg_hba_for_backup

    # create .pgpass file for replication user and password
    sudo -u $PG_USER bash <<EOF
    echo "127.0.0.1:${PG_PORT}:*:replicator:replicator_pass" > /var/lib/postgresql/.pgpass
    chmod 600 /var/lib/postgresql/.pgpass
EOF

    # Reload PostgreSQL configuration
    #sudo systemctl reload postgresql
    restart_server "$PG_DATA"
    echo "Replication user configured successfully. ✅ "
}

# Perform Full Backup Using pg_basebackup
perform_full_backup() {
    backup_command_options=" -Fp -Xs -P -R "
    log_message "Starting full backup with pg_basebackup with options ${backup_command_options}..."
    sudo -u "$PG_USER" rm -rf "$FULL_BACKUP_DIR"
    sudo -u "$PG_USER" mkdir -p "$FULL_BACKUP_DIR"
    sudo -u "$PG_USER" $INSTALL_DIR/bin/pg_basebackup -h "$HOST" -p "$PG_PORT" \
        -D "$FULL_BACKUP_DIR" $backup_command_options
    if [[ $? -ne 0 ]]; then
        log_message "Error: Backup failed. ❌"
        return 1
    else
        log_message "Backup completed successfully. ✅ "
    fi
}

# Update pg_hba.conf for Restore if configuration is different
update_pg_hba_for_restore() {
    echo "Updating pg_hba.conf for restore..."

    # Ensure the restored directory contains pg_hba.conf
    sudo -u $PG_USER bash <<EOF
    if [[ -f "$RESTORE_PG_HBA" ]]; then
        echo "Adding local access for postgres to pg_hba.conf in the restored directory..."
        echo "local   all   postgres   trust" >> "$RESTORE_PG_HBA"
        echo "host    all   all        127.0.0.1/32   md5" >> "$RESTORE_PG_HBA"
        echo "host    all   all        ::1/128        md5" >> "$RESTORE_PG_HBA"
    else
        log_message "Error: Restored pg_hba.conf not found at $RESTORE_PG_HBA. ❌"
        exit 1
    fi
EOF
    log_message "pg_hba.conf updated for restore. ✅ "
}


# initate the database
initialize_server() {
    DATADIR="${1:-$PG_DATA}"
    sudo -u $PG_USER rm -fr $DATADIR
    sudo -u $PG_USER mkdir -p $DATADIR
    sudo -u $PG_USER bash <<EOF
    $INSTALL_DIR/bin/initdb -D $DATADIR
EOF
}

# Start the server with specific data directory
start_server() {
    DATADIR="${1:-$PG_DATA}"
    PORT="${2:-$PG_PORT}"
    sudo -u $PG_USER bash <<EOF
    $INSTALL_DIR/bin/pg_ctl -D $DATADIR start -o "-p $PORT" -l $DATADIR/logfile
EOF
}

# Stop the server with specific data directory
stop_server() {
    DATADIR="${1:-$PG_DATA}"
    PORT="${2:-$PG_PORT}"
    sudo -u $PG_USER bash <<EOF
    $INSTALL_DIR/bin/pg_ctl -D $DATADIR stop -o "-p $PORT" -l $DATADIR/logfile
EOF
}

# Restart the server with specific data directory
restart_server() {
    DATADIR="${1:-$PG_DATA}"
    PORT="${2:-$PG_PORT}"
    sudo -u $PG_USER bash <<EOF
    $INSTALL_DIR/bin/pg_ctl -D $DATADIR restart -o "-p $PORT" -l $DATADIR/logfile
EOF
}

# Restore full backup to a new directory
restore_full_backup() {
    log_message "Restoring full backup for verification... $RESTORE_DIR"
    #sudo systemctl stop postgresql
    stop_server "$PG_DATA"
    sudo -u $PG_USER bash <<EOF
    rm -rf "$RESTORE_DIR" && mkdir -p "$RESTORE_DIR"
    cp -R "$FULL_BACKUP_DIR/"* "$RESTORE_DIR"
    chown -R ${PG_USER}:${PG_USER} "$RESTORE_DIR"
    chmod -R 700 "$RESTORE_DIR"
EOF
    # Update pg_hba.conf for the restored directory
    update_pg_hba_for_restore

    # Update `postgresql.conf` for the restored directory
    echo "data_directory = '$RESTORE_DIR'" | sudo -u "$PG_USER" tee -a "$RESTORE_DIR/postgresql.conf"

    # Start PostgreSQL with the restored directory
    #sudo -u "$PG_USER" $INSTALL_DIR/bin/pg_ctl -D "$RESTORE_DIR" start &>/dev/null
    start_server "$RESTORE_DIR"
    if [[ $? -ne 0 ]]; then
        log_message "Error: Failed to start PostgreSQL with restored directory. ❌"
        return 1
    fi
    log_message "Backup restored successfully. ✅ "
}

# Take an incremental backup
perform_incremental_backup() {
    # stop the restored server
    stop_server "$RESTORE_DIR"
    # start the full backup server
    #sudo systemctl start postgresql
    start_server "$PG_DATA"

    # Insert some data
    run_sql "incremental_data.sql"
    create_expected_output "verify_incremental_data"
    # Perform incremental backup
    log_message "Taking incremental backup..."
    sudo -u "$PG_USER" rm -fr "$INCREMENTAL_BACKUP_DIR"
    sudo -u "$PG_USER" mkdir -p "$INCREMENTAL_BACKUP_DIR"
    sudo -u "$PG_USER" $INSTALL_DIR/bin/pg_basebackup -h "$HOST" -p "$PG_PORT" --incremental="$FULL_BACKUP_DIR/backup_manifest" -D "$INCREMENTAL_BACKUP_DIR"
    if [ $? -ne 0 ]; then
        log_message "Incremental backup failed!"
        exit 1
    fi
    log_message "Incremental backup completed successfully."
    # stop the full backup server
    # sudo systemctl stop postgresql
    stop_server "$PG_DATA"
}

# Restore the incremental backup
restore_incremental_backup() {
    log_message "Restoring incremental backup..."
    sudo -u $PG_USER bash <<EOF
    rm -rf "$RESTORE_DIR" && mkdir -p "$RESTORE_DIR"
    chown -R ${PG_USER}:${PG_USER} "$RESTORE_DIR"
    chmod -R 700 "$RESTORE_DIR"
    $INSTALL_DIR/bin/pg_combinebackup "$FULL_BACKUP_DIR" "$INCREMENTAL_BACKUP_DIR" -o "$RESTORE_DIR"
EOF
    # Update pg_hba.conf for the restored directory
    update_pg_hba_for_restore

    # Update `postgresql.conf` for the restored directory
    echo "data_directory = '$RESTORE_DIR'" | sudo -u "$PG_USER" tee -a "$RESTORE_DIR/postgresql.conf"
    # Start PostgreSQL with restored backup to verify
    log_message "Starting PostgreSQL for verification..."
    start_server "$RESTORE_DIR"
    sleep 5
}

# Verify Data Encryption at Rest
verify_encrypted_data_at_rest() {
    # Get Data File Path
    DATA_PATH=$(sudo -u "$PG_USER" psql -p $PG_PORT -d "$DB_NAME" -t -c "SELECT pg_relation_filepath('$TABLE_NAME');" | xargs)
    DATA_DIR=$(sudo -u "$PG_USER" psql -p $PG_PORT -d "$DB_NAME" -t -c "SHOW data_directory" | xargs)
    DATA_FILE="$DATA_DIR/$DATA_PATH"

    log_message "Verifying data encryption at rest for table: $TABLE_NAME"

    # Extract first 10 lines of raw data
    RAW_DATA=$(sudo hexdump -C "$DATA_FILE" | head -n 10 || true)
    log_message "$RAW_DATA"

    READABLE_TEXT=$(sudo strings "$DATA_FILE" | grep "$SEARCHED_TEXT" || true)
    # Check if there is readable text in the data file
    if [[ -n "$READABLE_TEXT" ]]; then
       log_message "Readable text detected! Data appears UNENCRYPTED.❌ "
    else
	    log_message "Test Passed: Data appears to be encrypted! ✅ "
    fi
}

# Verify PGDATA/pg_tde folder exists
verify_tde_folder() {
    log_message "Verifying PGDATA/pg_tde folder exists..."
    # Get PGDATA directory
    PGDATA=$(sudo -u "$PG_USER" psql -p $PG_PORT -d "$DB_NAME" -t -c "SHOW data_directory;" | xargs)
    # Define the TDE directory path
    TDE_DIR="$PGDATA/pg_tde"
    sudo -u "$PG_USER" ls "$TDE_DIR" &>/dev/null
    if [[ $? -eq 0 ]]; then
        log_message "$TDE_DIR folder exists. ✅ "
    else
        log_message "Error: $TDE_DIR folder not found. ❌"
    fi
}

verify_tde_files(){
    log_message "Verifying required TDE files for database OID..."
    # Get PGDATA directory
    PGDATA=$(sudo -u "$PG_USER" psql -p $PG_PORT -d "$DB_NAME" -t -c "SHOW data_directory;" | xargs)

    # Get relation filepath (returns something like base/16543/16632)
    REL_FILE_PATH=$(sudo -u "$PG_USER" psql -p $PG_PORT -d "$DB_NAME" -t -c "SELECT pg_relation_filepath('$TABLE_NAME');" | xargs)

    # Extract the database OID (second field from the relation path)
    DB_OID=$(sudo echo "$REL_FILE_PATH" | awk -F'/' '{print $2}')

    # Define the TDE directory path
    TDE_DIR="$PGDATA/pg_tde"
    # Define expected files
    KEYRING_FILE="$TDE_DIR/pg_tde_${DB_OID}_keyring"
    DAT_FILE="$TDE_DIR/pg_tde_${DB_OID}_dat"
    MAP_FILE="$TDE_DIR/pg_tde_${DB_OID}_map"

    # Verify required TDE files
    log_message "Checking required TDE files for database OID $DB_OID..."
    MISSING_FILES=0
    for FILE in "$KEYRING_FILE" "$DAT_FILE" "$MAP_FILE"; do
        sudo -u "$PG_USER" ls "$FILE" &>/dev/null
        if [[ $? -ne 0 ]]; then
            log_message "Missing file: $FILE ❌"
            MISSING_FILES=$((MISSING_FILES + 1))
        else
            log_message "File exists: $FILE ✅"
        fi
    done

    # Final Test Result
    if [[ "$MISSING_FILES" -gt 0 ]]; then
        log_message "One or more required TDE files are missing! ❌"
    else
        log_message "All required TDE files exist! ✅"
    fi

}

# Step 7: Verify Restored Data
verify_restored_data() {
    file_name=$1
    echo "Verifying restored data..."
    verify_output "${file_name}.sql"       "${file_name}.out"
}

verify_sql_files() {
    echo "Verifying SQL files..."
}

verify_backup_integrity() {
    backup_dir="${1:-$FULL_BACKUP_DIR}"
    echo "Verifying backup integrity..."
    sudo -u "$PG_USER" ${INSTALL_DIR}/bin/pg_verifybackup "$backup_dir"
    if [[ $? -eq 0 ]]; then
        echo "Backup integrity verified successfully. ✅ "
    else
        echo "Backup integrity verification failed! ❌"
        return 1
    fi
}

# Verify restore backup with different scenarios
test_scenarios() {
    backup_dir="${1:-$FULL_BACKUP_DIR}"
    restore_dir="${2:-$RESTORE_DIR}"
    data_file="${3:-verify_sample_data}"
    echo "Verifying backup restored testing scenarios..."
    
    # Simulate encryption at rest
    verify_encrypted_data_at_rest "$restore_dir"

    # Scenario 2: Simulate backup integrity verification
    verify_backup_integrity "$backup_dir"

    # Scenario 3: Verify that the pg_tde directory exists
    verify_tde_folder

    # Scenario 4: Verify that the required TDE files exist
    verify_tde_files

}

# Main Script Execution
main() {
    echo "=== Starting pg_basebackup Test Automation ==="
    #setup_percona_repo
    #install_packages
    initialize_server
    start_server
    setup_postgresql
    setup_tde_heap
    setup_replication_user
    populate_sample_data
    create_expected_output "verify_sample_data"

    log_message "Backing up PostgreSQL database..."
    perform_full_backup

    log_message "Restoring and verifying backups..."
    restore_full_backup

    log_message "Running different tests to verify restored data..."
    test_scenarios

    #Verify restored data
    verify_restored_data "verify_sample_data"

    echo "====================================="
    echo "=== Performing Incremental Backup ==="
    echo "====================================="
    log_message "Performing incremental backup..."
    perform_incremental_backup

    log_message "Restoring incremental backup..."
    restore_incremental_backup

    log_message "Running different tests to verify incremental restored data..."
    test_scenarios "$INCREMENTAL_BACKUP_DIR" "$RESTORE_DIR"

    #Verify both fullbackup & incremental backup data
    verify_restored_data "verify_sample_data"
    verify_restored_data "verify_incremental_data"

    # "Do we need to cover other backup tools like pgBackRest/Barman?"
    # TO be implemented

    # # Verify wal encryption in restored data
    # To be implemented

    # "Scenarios to cover the keys, key rotation, key management, etc."
    # TO be implemented

    #echo "when you change the keyering file"

    #echo "Need to verify the data after server restart"

    echo "Verify other pg_basebackup options like checksum, wal options etc"

    echo "=== pg_basebackup Test Automation Completed! === 🚀"
}

# Run Main Function
main
