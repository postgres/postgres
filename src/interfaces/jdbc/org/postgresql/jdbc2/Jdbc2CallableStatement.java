package org.postgresql.jdbc2;


import java.sql.*;
import java.util.Vector;
import org.postgresql.PGRefCursorResultSet;
import org.postgresql.core.BaseResultSet;
import org.postgresql.core.Field;

public class Jdbc2CallableStatement extends org.postgresql.jdbc2.AbstractJdbc2Statement implements java.sql.CallableStatement
{

	public Jdbc2CallableStatement(Jdbc2Connection connection, String sql) throws SQLException
	{
		super(connection, sql);
	}

	public BaseResultSet createResultSet (Field[] fields, Vector tuples, String status, int updateCount, long insertOID) throws SQLException
	{
		return new Jdbc2ResultSet(this, fields, tuples, status, updateCount, insertOID);
	}

  	public PGRefCursorResultSet createRefCursorResultSet (String cursorName) throws SQLException
	{
                return new Jdbc2RefCursorResultSet(this, cursorName);
	}
}

