#!/bin/sh
#-------------------------------------------------------------------------
#
# cleardbdir.sh--
#    completely clear out the database directory
#
#    A program by this name used to be necessary because the database 
#    files were mixed in with postgres program files.  Now, the database
#    files are in their own directory so you can just rm it.
#
#    We have to ship this program, which now just tells the user there's
#    no such program, to make sure that the old program from
#    a prior release gets deleted.  If it hung around, it could confuse 
#    the user.
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/cleardbdir/Attic/cleardbdir.sh,v 1.2 1996/09/23 08:23:03 scrappy Exp $
#
#-------------------------------------------------------------------------

echo "The cleardbir program no longer exists.  To remove an old database"
echo "system, simply wipe out the whole directory that contains it."
echo
echo "You can create a new database system with initdb."
