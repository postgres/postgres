To make macOS automatically launch your PostgreSQL server at system start,
do the following:

1. Edit the postgres-wrapper.sh script and adjust the file path
variables at its start to reflect where you have installed Postgres,
if that's not /usr/local/pgsql.

2. Copy the modified postgres-wrapper.sh script into some suitable
installation directory.  It can be, but doesn't have to be, where
you keep the Postgres executables themselves.

3. Edit the org.postgresql.postgres.plist file and adjust its path
for postgres-wrapper.sh to match what you did in step 2.  Also,
if you plan to run the Postgres server under some user name other
than "postgres", adjust the UserName parameter value for that.

4. Copy the modified org.postgresql.postgres.plist file into
/Library/LaunchDaemons/.  You must do this as root:
    sudo cp org.postgresql.postgres.plist /Library/LaunchDaemons
because the file will be ignored if it is not root-owned.

At this point a reboot should launch the server.  But if you want
to test it without rebooting, you can do
    sudo launchctl load /Library/LaunchDaemons/org.postgresql.postgres.plist
