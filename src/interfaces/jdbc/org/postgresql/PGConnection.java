/*-------------------------------------------------------------------------
 *
 * PGConnection.java
 *	  The public interface definition for a Postgresql Connection
 *    This interface defines PostgreSQL extentions to the java.sql.Connection
 *    interface. Any java.sql.Connection object returned by the driver will 
 *    also implement this interface
 *
 * Copyright (c) 2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/Attic/PGConnection.java,v 1.6 2003/05/29 03:21:32 barry Exp $
 *
 *-------------------------------------------------------------------------
 */
package org.postgresql;

import java.sql.*;
import org.postgresql.core.Encoding;
import org.postgresql.fastpath.Fastpath;
import org.postgresql.largeobject.LargeObjectManager;

public interface PGConnection
{
	/**
	 * This method returns any notifications that have been received
	 * since the last call to this method.
	 * Returns null if there have been no notifications.
	 * @since 7.3
	 */
	public PGNotification[] getNotifications();

	/**
	 * This returns the LargeObject API for the current connection.
	 * @since 7.3
	 */
	public LargeObjectManager getLargeObjectAPI() throws SQLException;

	/**
	 * This returns the Fastpath API for the current connection.
	 * @since 7.3
	 */
	public Fastpath getFastpathAPI() throws SQLException;

        /*
         * This allows client code to add a handler for one of org.postgresql's
         * more unique data types.
         *
         * <p><b>NOTE:</b> This is not part of JDBC, but an extension.
         *
         * <p>The best way to use this is as follows:
         *
         * <p><pre>
         * ...
         * ((org.postgresql.PGConnection)myconn).addDataType("mytype","my.class.name");
         * ...
         * </pre>
         *
         * <p>where myconn is an open Connection to org.postgresql.
         *
         * <p>The handling class must extend org.postgresql.util.PGobject
         *
         * @see org.postgresql.util.PGobject
         */
        public void addDataType(String type, String name);


	/** @deprecated */
	public Encoding getEncoding() throws SQLException;

	/** @deprecated */
	public int getSQLType(String pgTypeName) throws SQLException;

	/** @deprecated */
	public int getSQLType(int oid) throws SQLException;

	/** @deprecated */
	public String getPGType(int oid) throws SQLException;

	/** @deprecated */
	public int getPGType(String typeName) throws SQLException;

	/** @deprecated */
	public Object getObject(String type, String value) throws SQLException;

}

