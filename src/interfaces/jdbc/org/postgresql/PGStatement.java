/*-------------------------------------------------------------------------
 *
 * PGStatement.java
 *     This interface defines PostgreSQL extentions to the java.sql.Statement
 *     interface.  Any java.sql.Statement object returned by the driver will 
 *     also implement this interface
 *
 * Copyright (c) 2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/Attic/PGStatement.java,v 1.7 2003/03/07 18:39:41 barry Exp $
 *
 *-------------------------------------------------------------------------
 */
package org.postgresql;


import java.sql.*;

public interface PGStatement
{

	/**
	 * Returns the Last inserted/updated oid.
	 * @return OID of last insert
   	 * @since 7.3
	 */
	public long getLastOID() throws SQLException;

	/**
	 * Turn on the use of prepared statements in the server (server side
	 * prepared statements are unrelated to jdbc PreparedStatements)
   	 * @since 7.3
	 */
	public void setUseServerPrepare(boolean flag) throws SQLException;

	/**
	 * Is this statement using server side prepared statements
   	 * @since 7.3
	 */
	public boolean isUseServerPrepare();

}
