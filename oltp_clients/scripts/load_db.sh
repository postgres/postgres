#!bin/bash
docker rm -f pg
docker pull postgres
docker run --name pg -e POSTGRES_PASSWORD=bench -p 5432:5432 -e POSTGRES_MAX_PREPARED_TRANSACTIONS=100 -d postgres -N 1000
docker exec -it pg psql -U postgres -c "create database flexi"
docker exec -it pg psql -U postgres -d flexi -c "SET default_transaction_isolation = 'serializable'"
docker exec -it pg psql -U postgres -d flexi -c "ALTER SYSTEM SET max_prepared_transactions TO 1000"
docker exec -it pg psql -U postgres -d flexi -c "ALTER SYSTEM SET shared_buffers = '4GB'"
docker exec -it pg psql -U postgres -d flexi -c "DROP TABLE IF exists YCSB_MAIN"
docker exec -it pg psql -U postgres -d flexi -c "CREATE TABLE YCSB_MAIN (key VARCHAR(255) PRIMARY KEY,  value VARCHAR(255))"
docker restart pg
docker exec -it pg psql -U postgres -d flexi -c "SHOW max_prepared_transactions"

docker exec -it pg psql -U postgres -d flexi -c "SELECT * FROM pg_prepared_xacts"

docker rm -f mg
docker pull mongo
docker run --name mg -p 27019:27017 -d mongo
docker exec -it mg mongosh
use flexi
db.createUser({
    user: "tester",
    pwd: "123",
    roles: [{ role: "dbAdmin", db: "flexi" }]
});
