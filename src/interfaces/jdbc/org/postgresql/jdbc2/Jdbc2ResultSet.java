package org.postgresql.jdbc2;


import java.sql.*;
import java.util.Vector;
import org.postgresql.Field;

/* $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/jdbc2/Attic/Jdbc2ResultSet.java,v 1.7 2003/02/04 09:20:10 barry Exp $
 * This class implements the java.sql.ResultSet interface for JDBC2.
 * However most of the implementation is really done in
 * org.postgresql.jdbc2.AbstractJdbc2ResultSet or one of it's parents
 */
public class Jdbc2ResultSet extends org.postgresql.jdbc2.AbstractJdbc2ResultSet implements java.sql.ResultSet
{

	public Jdbc2ResultSet(Statement statement, Field[] fields, Vector tuples, String status, int updateCount, long insertOID, boolean binaryCursor)
	{
		super(statement, fields, tuples, status, updateCount, insertOID, binaryCursor);
	}

	public java.sql.ResultSetMetaData getMetaData() throws SQLException
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

