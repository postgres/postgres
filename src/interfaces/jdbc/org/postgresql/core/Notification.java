/*-------------------------------------------------------------------------
 *
 * Notification.java
 *     This is the implementation of the PGNotification interface
 *
 * Copyright (c) 2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/core/Attic/Notification.java,v 1.3 2003/03/07 18:39:41 barry Exp $
 *
 *-------------------------------------------------------------------------
 */
package org.postgresql.core;

import org.postgresql.PGNotification;

public class Notification implements PGNotification
{
	public Notification(String p_name, int p_pid)
	{
		m_name = p_name;
		m_pid = p_pid;
	}

	/*
	 * Returns name of this notification
	 */
	public String getName()
	{
		return m_name;
	}

	/*
	 * Returns the process id of the backend process making this notification
	 */
	public int getPID()
	{
		return m_pid;
	}

	private String m_name;
	private int m_pid;

}

