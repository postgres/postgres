package org.postgresql.jdbc3;


import java.sql.*;
import org.postgresql.PGRefCursorResultSet;
import org.postgresql.core.BaseResultSet;
import org.postgresql.core.BaseStatement;
import org.postgresql.core.Field;

public class Jdbc3PreparedStatement extends org.postgresql.jdbc3.AbstractJdbc3Statement implements java.sql.PreparedStatement
{

	public Jdbc3PreparedStatement(Jdbc3Connection connection, String sql) throws SQLException
	{
		super(connection, sql);
	}

	public BaseResultSet createResultSet (Field[] fields, java.util.Vector tuples, String status, int updateCount, long insertOID) throws SQLException
	{
                return new Jdbc3ResultSet((BaseStatement)this, fields, tuples, status, updateCount, insertOID);
	}
         
  	public PGRefCursorResultSet createRefCursorResultSet (String cursorName) throws SQLException
	{
                return new Jdbc3RefCursorResultSet(this, cursorName);
	}
}

