package org.postgresql.test.jdbc2;

import org.postgresql.test.TestUtil;
import java.sql.CallableStatement;
import java.sql.Connection;
import java.sql.SQLException;
import java.sql.Statement;
import java.sql.Types;

import junit.framework.TestCase;

/*
 * CallableStatement tests.
 * @author Paul Bethe
 */
public class CallableStmtTest extends TestCase
{
	private Connection con;

	public CallableStmtTest (String name)
	{
		super(name);
	}

	protected void setUp() throws Exception
	{
		con = TestUtil.openDB();
		Statement stmt = con.createStatement ();
		stmt.execute ("CREATE OR REPLACE FUNCTION testspg__getString (varchar) " +
					  "RETURNS varchar AS ' DECLARE inString alias for $1; begin " +
					  "return ''bob''; end; ' LANGUAGE 'plpgsql';");
		stmt.execute ("CREATE OR REPLACE FUNCTION testspg__getDouble (float) " +
					  "RETURNS float AS ' DECLARE inString alias for $1; begin " +
					  "return 42.42; end; ' LANGUAGE 'plpgsql';");
		stmt.execute ("CREATE OR REPLACE FUNCTION testspg__getInt (int) RETURNS int " +
					  " AS 'DECLARE	inString alias for $1; begin " +
					  "return 42; end;' LANGUAGE 'plpgsql';");
		stmt.execute ("CREATE OR REPLACE FUNCTION testspg__getNumeric (numeric) " +
					  "RETURNS numeric AS ' DECLARE	inString alias for $1; " +
					  "begin	return 42; end; ' LANGUAGE 'plpgsql';");
		stmt.close ();
	}

	protected void tearDown() throws Exception
	{
		Statement stmt = con.createStatement ();
		stmt.execute ("drop FUNCTION testspg__getString (varchar);");
		stmt.execute ("drop FUNCTION testspg__getDouble (float);");
		stmt.execute ("drop FUNCTION testspg__getInt (int);");
		stmt.execute ("drop FUNCTION testspg__getNumeric (numeric);");
		TestUtil.closeDB(con);
	}


	final String func = "{ ? = call ";
	final String pkgName = "testspg__";
	//	protected void runTest () throws Throwable {
	//testGetString ();
	//}

	public void testGetDouble () throws Throwable
	{
		CallableStatement call = con.prepareCall (func + pkgName + "getDouble (?) }");
		call.setDouble (2, (double)3.04);
		call.registerOutParameter (1, Types.DOUBLE);
		call.execute ();
		double result = call.getDouble (1);
		assertTrue ("correct return from getString ()", result == 42.42);
	}

	public void testGetInt () throws Throwable
	{
		CallableStatement call = con.prepareCall (func + pkgName + "getInt (?) }");
		call.setInt (2, 4);
		call.registerOutParameter (1, Types.INTEGER);
		call.execute ();
		int result = call.getInt (1);
		assertTrue ("correct return from getString ()", result == 42);
	}

	public void testGetNumeric () throws Throwable
	{
		CallableStatement call = con.prepareCall (func + pkgName + "getNumeric (?) }");
		call.setBigDecimal (2, new java.math.BigDecimal(4));
		call.registerOutParameter (1, Types.NUMERIC);
		call.execute ();
		java.math.BigDecimal result = call.getBigDecimal (1);
		assertTrue ("correct return from getString ()",
					result.equals (new java.math.BigDecimal(42)));
	}

	public void testGetString () throws Throwable
	{
		CallableStatement call = con.prepareCall (func + pkgName + "getString (?) }");
		call.setString (2, "foo");
		call.registerOutParameter (1, Types.VARCHAR);
		call.execute ();
		String result = call.getString (1);
		assertTrue ("correct return from getString ()", result.equals ("bob"));

	}

	public void testBadStmt () throws Throwable
	{
		tryOneBadStmt ("{ ?= " + pkgName + "getString (?) }");
		tryOneBadStmt ("{ ?= call getString (?) ");
		tryOneBadStmt ("{ = ? call getString (?); }");
	}

	protected void tryOneBadStmt (String sql) throws Throwable
	{
		boolean wasCaught = false;
		try
		{
			CallableStatement call = con.prepareCall (sql);
		}
		catch (SQLException e)
		{
			wasCaught = true; // good -> this statement was missing something
		}
		assertTrue ("bad statment ('" + sql + "')was not caught", wasCaught);
	}

}
