package example.corba;

import java.sql.*;

/*
 * This class handles the JDBC side of things. It opens a connection to
 * the database, and performes queries on that database.
 *
 * In essence, you could use this class on it's own. The rest of the classes
 * in this example handle either the CORBA mechanism, or the frontend.
 *
 * Note: Before you ask, why perform a query on each call, you have to remember
 * that an object could be changed by another client, and we need to ensure that
 * the returned data is live and accurate.
 *
 * $Id: StockDB.java,v 1.4 2001/11/19 22:43:13 momjian Exp $
 */
public class StockDB
{
	Connection con;
	Statement st;

	// the current stock number
	int id = -1;

	public void connect(String url, String usr, String pwd) throws Exception
	{
		Class.forName("org.postgresql.Driver");
		System.out.println("Connecting to " + url);
		con = DriverManager.getConnection(url, usr, pwd);
		st = con.createStatement();
	}

	public void closeConnection() throws Exception
	{
		con.close();
	}

	public void fetchItem(int id) throws Exception
	{
		this.id = id;
	}

	public int newItem() throws Exception
	{
		// tba
		return -1;
	}

	public String getDescription() throws SQLException
	{
		ResultSet rs = st.executeQuery("select description from stock where id=" + id);
		if (rs != null)
		{
			rs.next();
			String s = rs.getString(1);
			rs.close();
			return s;
		}
		throw new SQLException("No ResultSet");
	}

	public int getAvailable() throws SQLException
	{
		ResultSet rs = st.executeQuery("select avail from stock where id=" + id);
		if (rs != null)
		{
			rs.next();
			int v = rs.getInt(1);
			rs.close();
			return v;
		}
		throw new SQLException("No ResultSet");
	}

	public int getOrdered() throws SQLException
	{
		ResultSet rs = st.executeQuery("select ordered from stock where id=" + id);
		if (rs != null)
		{
			rs.next();
			int v = rs.getInt(1);
			rs.close();
			return v;
		}
		throw new SQLException("No ResultSet");
	}

	public boolean isItemValid() throws SQLException
	{
		ResultSet rs = st.executeQuery("select valid from stock where id=" + id);
		if (rs != null)
		{
			rs.next();
			boolean b = rs.getBoolean(1);
			rs.close();
			return b;
		}
		throw new SQLException("No ResultSet");
	}

	public void addNewStock(int amount) throws SQLException
	{
		st.executeUpdate("update stock set avail=avail+" + amount +
						 ", ordered=ordered-" + amount +
						 " where id=" + id + " and ordered>=" + amount);
	}

	public void removeStock(int amount) throws SQLException
	{
		st.executeUpdate("update stock set avail=avail-" + amount +
						 " where id=" + id);
	}

	public void orderStock(int amount) throws SQLException
	{
		st.executeUpdate("update stock set ordered=ordered+" + amount +
						 " where id=" + id);
	}

	public int getLastID() throws SQLException
	{
		ResultSet rs = st.executeQuery("select max(id) from stock");
		if (rs != null)
		{
			rs.next();
			int v = rs.getInt(1);
			rs.close();
			return v;
		}
		throw new SQLException("No ResultSet");
	}

}
