# The Postgres make files exploit features of GNU make that other makes
# do not have.  Because it is a common mistake for users to try to build
# Postgres with a different make, we have this make file that does nothing
# but tell the user to use GNU make.

# If the user were using GNU make now, this file would not get used because
# GNU make uses a make file named "GNUmakefile" in preference to "Makefile"
# if it exists.  Postgres is shipped with a "GNUmakefile".

all install clean dep depend check installcheck:
	@echo "You must use GNU make to use Postgres.  It may be installed"
	@echo "on your system with the name 'gmake'."
