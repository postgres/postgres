package org.postgresql;


import java.sql.*;

/* $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/Attic/PGStatement.java,v 1.3 2002/07/23 03:59:55 barry Exp $
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

}
