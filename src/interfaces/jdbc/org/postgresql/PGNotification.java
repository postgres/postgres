/*-------------------------------------------------------------------------
 *
 * PGNotification.java
 *    This interface defines public PostgreSQL extention for Notifications
 *
 * Copyright (c) 2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/Attic/PGNotification.java,v 1.3 2003/03/07 18:39:41 barry Exp $
 *
 *-------------------------------------------------------------------------
 */
package org.postgresql;


public interface PGNotification
{
	/**
	 * Returns name of this notification
	 * @since 7.3
	 */
	public String getName();

	/**
	 * Returns the process id of the backend process making this notification
	 * @since 7.3
	 */
	public int getPID();

}

