package org.postgresql;


/* $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/Attic/PGNotification.java,v 1.1 2002/09/02 03:07:36 barry Exp $
 * This interface defines PostgreSQL extention for Notifications
 */
public interface PGNotification
{
    /* 
     * Returns name of this notification
     */
    public String getName();

    /*
     * Returns the process id of the backend process making this notification
     */
    public int getPID();

}

