package org.postgresql.jdbc2;


import java.sql.*;

public class Jdbc2PreparedStatement extends org.postgresql.jdbc2.AbstractJdbc2Statement implements java.sql.PreparedStatement
{

	public Jdbc2PreparedStatement(Jdbc2Connection connection, String sql) throws SQLException
	{
		super(connection, sql);
	}

}

