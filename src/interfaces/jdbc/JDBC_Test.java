import java.io.*;
import java.lang.*;
import java.sql.*;

class JDBC_Test
{
        public JDBC_Test() 
	{
	}

	public static void main(String argv[])
	{
		String url = new String(argv[0]);
		Connection db;
		Statement s;
		ResultSet rs;

		// Load the driver
		try
		{
			Class.forName("postgresql.Driver");
		} catch (ClassNotFoundException e) {
			System.err.println("Exception: " + e.toString());
		}

		// Lets do a few things -- it doesn't do everything, but
		// it tests out basic functionality
		try
		{
		System.out.println("Connecting to Database URL = " + url);
		db = DriverManager.getConnection(url, "adrian", "");
		System.out.println("Connected...Now creating a statement");
		s = db.createStatement();
		System.out.println("Ok...now we will create a table");
		s.executeUpdate("create table test (a int2, b int2)");
		System.out.println("Now we will insert some columns");
		s.executeUpdate("insert into test values (1, 1)");
		s.executeUpdate("insert into test values (2, 1)");
		s.executeUpdate("insert into test values (3, 1)");
		System.out.println("Inserted some data");
		System.out.println("Now lets try a select");
		rs = s.executeQuery("select a, b from test");
		System.out.println("Back from the select...the following are results");
		int i = 0;
		while (rs.next())
		{
			int a = rs.getInt("a");
			int b = rs.getInt("b");
			System.out.println("row " + i + "	" + a + "	" + b);
			i++;
		}
		System.out.println("Ok...dropping the table");
		s.executeUpdate("drop table test");
		System.out.println("Now closing the connection");
		s.close();
		db.close();
		} catch (SQLException e) {
		System.out.println("Exception: " + e.toString());
		}
	}
}
