package org.postgresql.jdbc2;


import java.sql.*;
import java.util.Vector;
import org.postgresql.core.BaseStatement;
import org.postgresql.core.Field;

/* $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/jdbc2/Attic/Jdbc2ResultSet.java,v 1.8.4.1 2004/03/29 17:47:47 barry Exp $
 * This class implements the java.sql.ResultSet interface for JDBC2.
 * However most of the implementation is really done in
 * org.postgresql.jdbc2.AbstractJdbc2ResultSet or one of it's parents
 */
public class Jdbc2ResultSet extends org.postgresql.jdbc2.AbstractJdbc2ResultSet implements java.sql.ResultSet
{

	public Jdbc2ResultSet(BaseStatement statement, Field[] fields, Vector tuples, String status, int updateCount, long insertOID)
	{
		super(statement, fields, tuples, status, updateCount, insertOID);
	}

	public ResultSetMetaData getMetaData() throws SQLException
	{
		return new Jdbc2ResultSetMetaData(rows, fields);
	}

	public java.sql.Clob getClob(int i) throws SQLException
	{
		wasNullFlag = (this_row[i - 1] == null);
		if (wasNullFlag)
			return null;

		return new org.postgresql.jdbc2.Jdbc2Clob(connection, getInt(i));
	}

	public java.sql.Blob getBlob(int i) throws SQLException
	{
		wasNullFlag = (this_row[i - 1] == null);
		if (wasNullFlag)
			return null;

		return new org.postgresql.jdbc2.Jdbc2Blob(connection, getInt(i));
	}

}

