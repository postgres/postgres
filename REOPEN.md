# Quick Start After Reopening Project

## 1. Start PostgreSQL

```bash
./start_postgres.sh
```

**Or manually:**
```bash
./pg_install/bin/pg_ctl -D pg_data start
```

## 2. Run the VACUUM Test

```bash
./run_vacuum_test.sh
```

## 3. Stop PostgreSQL When Done

```bash
./stop_postgres.sh
```

---

## Common Commands

**Check if PostgreSQL is running:**
```bash
./status_postgres.sh
```

**View logs:**
```bash
tail -f pg_data/log/postgresql-*.log
```

**Connect to database:**
```bash
./pg_install/bin/psql -d postgres
```

**Quick reference:**
```bash
./quickref.sh
```

---

## Troubleshooting

**PostgreSQL won't start:**
```bash
# Check if already running
./status_postgres.sh

# Check logs
cat pg_data/logfile

# Force stop and restart
./pg_install/bin/pg_ctl -D pg_data stop -m immediate
./start_postgres.sh
```

**Port already in use:**
```bash
# Find what's using port 5432
lsof -i :5432

# Kill the process or change port in pg_data/postgresql.conf
```
