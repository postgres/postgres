package org.postgresql.jdbc3;


import java.sql.*;
import java.util.Vector;
import org.postgresql.PGRefCursorResultSet;
import org.postgresql.core.BaseResultSet;
import org.postgresql.core.Field;

/* $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/jdbc3/Attic/Jdbc3Statement.java,v 1.5 2003/05/03 20:40:45 barry Exp $
 * This class implements the java.sql.Statement interface for JDBC3.
 * However most of the implementation is really done in
 * org.postgresql.jdbc3.AbstractJdbc3Statement or one of it's parents
 */
public class Jdbc3Statement extends org.postgresql.jdbc3.AbstractJdbc3Statement implements java.sql.Statement
{

	public Jdbc3Statement (Jdbc3Connection c)
	{
		super(c);
	}

	public BaseResultSet createResultSet (Field[] fields, Vector tuples, String status, int updateCount, long insertOID, boolean binaryCursor) throws SQLException
	{
		return new Jdbc3ResultSet(this, fields, tuples, status, updateCount, insertOID, binaryCursor);
	}

  	public PGRefCursorResultSet createRefCursorResultSet (String cursorName) throws SQLException
	{
                return new Jdbc3RefCursorResultSet(this, cursorName);
	}
}
