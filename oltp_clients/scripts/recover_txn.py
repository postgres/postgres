import psycopg2

# Connect to PostgreSQL database
conn = psycopg2.connect(
    dbname='flexi',
    user='postgres',
    password='bench',
    host='localhost',
    port='5432'
)

# Create a cursor object
cur = conn.cursor()
conn.autocommit = True

# Fetch all prepared transactions
cur.execute("SELECT * FROM pg_prepared_xacts;")
prepared_transactions = cur.fetchall()

# Rollback each prepared transaction
for trans in prepared_transactions:
    gid = trans[1]
    cur.execute(f"ROLLBACK PREPARED '{gid}';")

# Close cursor and connection
cur.close()
conn.close()