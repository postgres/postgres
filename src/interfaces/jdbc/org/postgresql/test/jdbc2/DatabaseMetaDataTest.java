package org.postgresql.test.jdbc2;

import org.postgresql.test.JDBC2Tests;
import junit.framework.TestCase;
import java.sql.*;

/**
 * TestCase to test the internal functionality of org.postgresql.jdbc2.DatabaseMetaData
 *
 * PS: Do you know how difficult it is to type on a train? ;-)
 *
 * $Id: DatabaseMetaDataTest.java,v 1.3 2001/10/25 05:59:59 momjian Exp $
 */

public class DatabaseMetaDataTest extends TestCase
{

	/**
	 * Constructor
	 */
	public DatabaseMetaDataTest(String name)
	{
		super(name);
	}

	/**
	 * The spec says this may return null, but we always do!
	 */
	public void testGetMetaData()
	{
		try
		{
			Connection con = JDBC2Tests.openDB();

			DatabaseMetaData dbmd = con.getMetaData();
			assertNotNull(dbmd);

			JDBC2Tests.closeDB(con);
		}
		catch (SQLException ex)
		{
			fail(ex.getMessage());
		}
	}

	/**
	 * Test default capabilities
	 */
	public void testCapabilities()
	{
		try
		{
			Connection con = JDBC2Tests.openDB();

			DatabaseMetaData dbmd = con.getMetaData();
			assertNotNull(dbmd);

			assertTrue(dbmd.allProceduresAreCallable());
			assertTrue(dbmd.allTablesAreSelectable()); // not true all the time

			// This should always be false for postgresql (at least for 7.x)
			assertTrue(!dbmd.isReadOnly());

			// does the backend support this yet? The protocol does...
			assertTrue(!dbmd.supportsMultipleResultSets());

			// yes, as multiple backends can have transactions open
			assertTrue(dbmd.supportsMultipleTransactions());

			assertTrue(dbmd.supportsMinimumSQLGrammar());
			assertTrue(!dbmd.supportsCoreSQLGrammar());
			assertTrue(!dbmd.supportsExtendedSQLGrammar());
			assertTrue(!dbmd.supportsANSI92EntryLevelSQL());
			assertTrue(!dbmd.supportsANSI92IntermediateSQL());
			assertTrue(!dbmd.supportsANSI92FullSQL());

			assertTrue(!dbmd.supportsIntegrityEnhancementFacility());

			JDBC2Tests.closeDB(con);
		}
		catch (SQLException ex)
		{
			fail(ex.getMessage());
		}
	}


	public void testJoins()
	{
		try
		{
			Connection con = JDBC2Tests.openDB();

			DatabaseMetaData dbmd = con.getMetaData();
			assertNotNull(dbmd);

			assertTrue(dbmd.supportsOuterJoins());
			assertTrue(dbmd.supportsFullOuterJoins());
			assertTrue(dbmd.supportsLimitedOuterJoins());

			JDBC2Tests.closeDB(con);
		}
		catch (SQLException ex)
		{
			fail(ex.getMessage());
		}
	}

	public void testCursors()
	{
		try
		{
			Connection con = JDBC2Tests.openDB();

			DatabaseMetaData dbmd = con.getMetaData();
			assertNotNull(dbmd);

			assertTrue(!dbmd.supportsPositionedDelete());
			assertTrue(!dbmd.supportsPositionedUpdate());

			JDBC2Tests.closeDB(con);
		}
		catch (SQLException ex)
		{
			fail(ex.getMessage());
		}
	}

	public void testNulls()
	{
		try
		{
			Connection con = JDBC2Tests.openDB();

			DatabaseMetaData dbmd = con.getMetaData();
			assertNotNull(dbmd);

			// We need to type cast the connection to get access to the
			// PostgreSQL-specific method haveMinimumServerVersion().
			// This is not available through the java.sql.Connection interface.
			assertTrue( con instanceof org.postgresql.Connection );

			assertTrue(!dbmd.nullsAreSortedAtStart());
			assertTrue( dbmd.nullsAreSortedAtEnd() !=
						((org.postgresql.Connection)con).haveMinimumServerVersion("7.2"));
			assertTrue( dbmd.nullsAreSortedHigh() ==
						((org.postgresql.Connection)con).haveMinimumServerVersion("7.2"));
			assertTrue(!dbmd.nullsAreSortedLow());

			assertTrue(dbmd.nullPlusNonNullIsNull());

			assertTrue(dbmd.supportsNonNullableColumns());

			JDBC2Tests.closeDB(con);
		}
		catch (SQLException ex)
		{
			fail(ex.getMessage());
		}
	}

	public void testLocalFiles()
	{
		try
		{
			Connection con = JDBC2Tests.openDB();

			DatabaseMetaData dbmd = con.getMetaData();
			assertNotNull(dbmd);

			assertTrue(!dbmd.usesLocalFilePerTable());
			assertTrue(!dbmd.usesLocalFiles());

			JDBC2Tests.closeDB(con);
		}
		catch (SQLException ex)
		{
			fail(ex.getMessage());
		}
	}

	public void testIdentifiers()
	{
		try
		{
			Connection con = JDBC2Tests.openDB();

			DatabaseMetaData dbmd = con.getMetaData();
			assertNotNull(dbmd);

			assertTrue(!dbmd.supportsMixedCaseIdentifiers()); // always false
			assertTrue(dbmd.supportsMixedCaseQuotedIdentifiers());	// always true

			assertTrue(!dbmd.storesUpperCaseIdentifiers());   // always false
			assertTrue(dbmd.storesLowerCaseIdentifiers());	  // always true
			assertTrue(!dbmd.storesUpperCaseQuotedIdentifiers()); // always false
			assertTrue(!dbmd.storesLowerCaseQuotedIdentifiers()); // always false
			assertTrue(!dbmd.storesMixedCaseQuotedIdentifiers()); // always false

			assertTrue(dbmd.getIdentifierQuoteString().equals("\""));


			JDBC2Tests.closeDB(con);
		}
		catch (SQLException ex)
		{
			fail(ex.getMessage());
		}
	}

	public void testTables()
	{
		try
		{
			Connection con = JDBC2Tests.openDB();

			DatabaseMetaData dbmd = con.getMetaData();
			assertNotNull(dbmd);

			// we can add columns
			assertTrue(dbmd.supportsAlterTableWithAddColumn());

			// we can't drop columns (yet)
			assertTrue(!dbmd.supportsAlterTableWithDropColumn());

			JDBC2Tests.closeDB(con);
		}
		catch (SQLException ex)
		{
			fail(ex.getMessage());
		}
	}

	public void testSelect()
	{
		try
		{
			Connection con = JDBC2Tests.openDB();

			DatabaseMetaData dbmd = con.getMetaData();
			assertNotNull(dbmd);

			// yes we can?: SELECT col a FROM a;
			assertTrue(dbmd.supportsColumnAliasing());

			// yes we can have expressions in ORDERBY
			assertTrue(dbmd.supportsExpressionsInOrderBy());

			// Yes, an ORDER BY clause can contain columns that are not in the
			// SELECT clause.
			assertTrue(dbmd.supportsOrderByUnrelated());

			assertTrue(dbmd.supportsGroupBy());
			assertTrue(dbmd.supportsGroupByUnrelated());
			assertTrue(dbmd.supportsGroupByBeyondSelect()); // needs checking

			JDBC2Tests.closeDB(con);
		}
		catch (SQLException ex)
		{
			fail(ex.getMessage());
		}
	}

	public void testDBParams()
	{
		try
		{
			Connection con = JDBC2Tests.openDB();

			DatabaseMetaData dbmd = con.getMetaData();
			assertNotNull(dbmd);

			assertTrue(dbmd.getURL().equals(JDBC2Tests.getURL()));
			assertTrue(dbmd.getUserName().equals(JDBC2Tests.getUser()));

			JDBC2Tests.closeDB(con);
		}
		catch (SQLException ex)
		{
			fail(ex.getMessage());
		}
	}

	public void testDbProductDetails()
	{
		try
		{
			Connection con = JDBC2Tests.openDB();
			assertTrue(con instanceof org.postgresql.Connection);
			org.postgresql.Connection pc = (org.postgresql.Connection) con;

			DatabaseMetaData dbmd = con.getMetaData();
			assertNotNull(dbmd);

			assertTrue(dbmd.getDatabaseProductName().equals("PostgreSQL"));
			assertTrue(dbmd.getDatabaseProductVersion().startsWith(Integer.toString(pc.this_driver.getMajorVersion()) + "." + Integer.toString(pc.this_driver.getMinorVersion())));
			assertTrue(dbmd.getDriverName().equals("PostgreSQL Native Driver"));

			JDBC2Tests.closeDB(con);
		}
		catch (SQLException ex)
		{
			fail(ex.getMessage());
		}
	}

	public void testDriverVersioning()
	{
		try
		{
			Connection con = JDBC2Tests.openDB();
			assertTrue(con instanceof org.postgresql.Connection);
			org.postgresql.Connection pc = (org.postgresql.Connection) con;

			DatabaseMetaData dbmd = con.getMetaData();
			assertNotNull(dbmd);

			assertTrue(dbmd.getDriverVersion().equals(pc.this_driver.getVersion()));
			assertTrue(dbmd.getDriverMajorVersion() == pc.this_driver.getMajorVersion());
			assertTrue(dbmd.getDriverMinorVersion() == pc.this_driver.getMinorVersion());


			JDBC2Tests.closeDB(con);
		}
		catch (SQLException ex)
		{
			fail(ex.getMessage());
		}
	}
}
