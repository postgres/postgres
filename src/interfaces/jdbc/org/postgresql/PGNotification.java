package org.postgresql;


/* $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/Attic/PGNotification.java,v 1.2 2002/09/06 21:23:05 momjian Exp $
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

