package org.postgresql.jdbc3;


import java.sql.*;

public class Jdbc3PreparedStatement extends org.postgresql.jdbc3.AbstractJdbc3Statement implements java.sql.PreparedStatement
{
   
	public Jdbc3PreparedStatement(Jdbc3Connection connection, String sql) throws SQLException
	{
		super(connection, sql);
	}
   
}

