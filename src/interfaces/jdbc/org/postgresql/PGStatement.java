package org.postgresql;


import java.sql.*;

/* $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/Attic/PGStatement.java,v 1.6 2002/09/08 00:15:28 barry Exp $
 * This interface defines PostgreSQL extentions to the java.sql.Statement interface.
 * Any java.sql.Statement object returned by the driver will also implement this
 * interface
 */
public interface PGStatement
{

	/*
	 * Returns the Last inserted/updated oid.
	 * @return OID of last insert
			* @since 7.3
	 */
	public long getLastOID() throws SQLException;

	public void setUseServerPrepare(boolean flag) throws SQLException;

	public boolean isUseServerPrepare();

}
