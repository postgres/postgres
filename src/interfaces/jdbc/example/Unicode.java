package example;

import java.sql.*;
import java.util.*;

/*
 *	Test inserting and extracting Unicode-encoded strings.
 *
 *	Synopsis:
 *	   example.Unicode <url> <user> <password>
 *	where <url> must specify an existing database to which <user> and
 *	<password> give access and which has UNICODE as its encoding.
 *	(To create a database with UNICODE encoding, you need to run createdb
 *	with the flag "-E UNICODE".)
 *
 *	This test only produces output on error.
 *
 *	@author William Webber <william@live.com.au>
 */
public class Unicode
{

	/*
	 *	The url for the database to connect to.
	 */
	private String url;

	/*
	 *	The user to connect as.
	 */
	private String user;

	/*
	 *	The password to connect with.
	 */
	private String password;

	private static void usage()
	{
		log("usage: example.Unicode <url> <user> <password>");
	}

	private static void log(String message)
	{
		System.err.println(message);
	}

	private static void log(String message, Exception e)
	{
		System.err.println(message);
		e.printStackTrace();
	}


	public Unicode(String url, String user, String password)
	{
		this.url = url;
		this.user = user;
		this.password = password;
	}

	/*
	 *	Establish and return a connection to the database.
	 */
	private Connection getConnection() throws SQLException,
				ClassNotFoundException
	{
		Class.forName("org.postgresql.Driver");
		Properties info = new Properties();
		info.put("user", user);
		info.put("password", password);
		info.put("charSet", "utf-8");
		return DriverManager.getConnection(url, info);
	}

	/*
	 *	Get string representing a block of 256 consecutive unicode characters.
	 *	We exclude the null character, "'",  and "\".
	 */
	private String getSqlSafeUnicodeBlock(int blockNum)
	{
		if (blockNum < 0 || blockNum > 255)
			throw new IllegalArgumentException("blockNum must be from 0 to "
											   + "255: " + blockNum);
		StringBuffer sb = new StringBuffer(256);
		int blockFirst = blockNum * 256;
		int blockLast = blockFirst + 256;
		for (int i = blockFirst; i < blockLast; i++)
		{
			char c = (char) i;
			if (c == '\0' || c == '\'' || c == '\\')
				continue;
			sb.append(c);
		}
		return sb.toString();
	}

	/*
	 *	Is the block a block of valid unicode values.
	 *	d800 to db7f is the "unassigned high surrogate" range.
	 *	db80 to dbff is the "private use" range.
	 *	These should not be used in actual Unicode strings;
	 *	at least, jdk1.2 will not convert them to utf-8.
	 */
	private boolean isValidUnicodeBlock(int blockNum)
	{
		if (blockNum >= 0xd8 && blockNum <= 0xdb)
			return false;
		else
			return true;
	}

	/*
	 *	Report incorrect block retrieval.
	 */
	private void reportRetrievalError(int blockNum, String block,
									  String retrieved)
	{
		String message = "Block " + blockNum + " returned incorrectly: ";
		int i = 0;
		for (i = 0; i < block.length(); i++)
		{
			if (i >= retrieved.length())
			{
				message += "too short";
				break;
			}
			else if (retrieved.charAt(i) != block.charAt(i))
			{
				message +=
					"first changed character at position " + i + ", sent as 0x"
					+ Integer.toHexString((int) block.charAt(i))
					+ ", retrieved as 0x"
					+ Integer.toHexString ((int) retrieved.charAt(i));
				break;
			}
		}
		if (i >= block.length())
			message += "too long";
		log(message);
	}

	/*
	 *	Do the testing.
	 */
	public void runTest()
	{
		Connection connection = null;
		Statement statement = null;
		int blockNum = 0;
		final int CREATE = 0;
		final int INSERT = 1;
		final int SELECT = 2;
		final int LIKE = 3;
		int mode = CREATE;
		try
		{
			connection = getConnection();
			statement = connection.createStatement();
			statement.executeUpdate("CREATE TABLE test_unicode "
									+ "( blockNum INT PRIMARY KEY, "
									+ "block TEXT );");
			mode = INSERT;
			for (blockNum = 0; blockNum < 256; blockNum++)
			{
				if (isValidUnicodeBlock(blockNum))
				{
					String block = getSqlSafeUnicodeBlock(blockNum);
					statement.executeUpdate
					("INSERT INTO test_unicode VALUES ( " + blockNum
					 + ", '" + block + "');");
				}
			}
			mode = SELECT;
			for (blockNum = 0; blockNum < 256; blockNum++)
			{
				if (isValidUnicodeBlock(blockNum))
				{
					String block = getSqlSafeUnicodeBlock(blockNum);
					ResultSet rs = statement.executeQuery
								   ("SELECT block FROM test_unicode WHERE blockNum = "
									+ blockNum + ";");
					if (!rs.next())
						log("Could not retrieve block " + blockNum);
					else
					{
						String retrieved = rs.getString(1);
						if (!retrieved.equals(block))
						{
							reportRetrievalError(blockNum, block, retrieved);
						}
					}
				}
			}
			mode = LIKE;
			for (blockNum = 0; blockNum < 256; blockNum++)
			{
				if (isValidUnicodeBlock(blockNum))
				{
					String block = getSqlSafeUnicodeBlock(blockNum);
					String likeString = "%" +
										block.substring(2, block.length() - 3) + "%" ;
					ResultSet rs = statement.executeQuery
								   ("SELECT blockNum FROM test_unicode WHERE block LIKE '"
									+ likeString + "';");
					if (!rs.next())
						log("Could get block " + blockNum + " using LIKE");
				}
			}
		}
		catch (SQLException sqle)
		{
			switch (mode)
			{
				case CREATE:
					log("Exception creating database", sqle);
					break;
				case INSERT:
					log("Exception inserting block " + blockNum, sqle);
					break;
				case SELECT:
					log("Exception selecting block " + blockNum, sqle);
					break;
				case LIKE:
					log("Exception doing LIKE on block " + blockNum, sqle);
					break;
				default:
					log("Exception", sqle);
					break;
			}
		}
		catch (ClassNotFoundException cnfe)
		{
			log("Unable to load driver", cnfe);
			return ;
		}
		try
		{
			if (statement != null)
				statement.close();
			if (connection != null)
				connection.close();
		}
		catch (SQLException sqle)
		{
			log("Exception closing connections", sqle);
		}
		if (mode > CREATE)
		{
			// If the backend gets what it regards as garbage on a connection,
			// that connection may become unusable.  To be safe, we create
			// a fresh connection to delete the table.
			try
			{
				connection = getConnection();
				statement = connection.createStatement();
				statement.executeUpdate("DROP TABLE test_unicode;");
			}
			catch (Exception sqle)
			{
				log("*** ERROR: unable to delete test table "
					+ "test_unicode; must be deleted manually", sqle);
			}
		}
	}

	public static void main(String [] args)
	{
		if (args.length != 3)
		{
			usage();
			System.exit(1);
		}
		new Unicode(args[0], args[1], args[2]).runTest();
	}
}
