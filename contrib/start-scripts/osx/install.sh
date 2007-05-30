sudo sh -c 'echo "POSTGRESQL=-YES-" >> /etc/hostconfig'
sudo mkdir /Library/StartupItems/PostgreSQL
sudo cp PostgreSQL /Library/StartupItems/PostgreSQL
sudo cp StartupParameters.plist /Library/StartupItems/PostgreSQL
if [ -e /Library/StartupItems/PostgreSQL/PostgreSQL ]
then
  echo "Startup Item Installed Successfully . . . "
  echo "Starting PostgreSQL Server . . . "
  SystemStarter restart PostgreSQL
fi
