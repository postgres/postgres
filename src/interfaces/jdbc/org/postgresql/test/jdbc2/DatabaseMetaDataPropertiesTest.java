package org.postgresql.test.jdbc2;

import org.postgresql.test.TestUtil;
import junit.framework.TestCase;
import java.sql.*;

/*
 * TestCase to test the internal functionality of
 * org.postgresql.jdbc2.DatabaseMetaData's various properties.
 * Methods which return a ResultSet are tested elsewhere.
 * This avoids a complicated setUp/tearDown for something like
 * assertTrue(dbmd.nullPlusNonNullIsNull());
 */

public class DatabaseMetaDataPropertiesTest extends TestCase
{

	private Connection con;
	/*
	 * Constructor
	 */
	public DatabaseMetaDataPropertiesTest(String name)
	{
		super(name);
	}

	protected void setUp() throws Exception
	{
		con = TestUtil.openDB();
	}
	protected void tearDown() throws Exception
	{
		TestUtil.closeDB( con );
	}

	/*
	 * The spec says this may return null, but we always do!
	 */
	public void testGetMetaData()
	{
		try
		{

			DatabaseMetaData dbmd = con.getMetaData();
			assertNotNull(dbmd);

		}
		catch (SQLException ex)
		{
			fail(ex.getMessage());
		}
	}

	/*
	 * Test default capabilities
	 */
	public void testCapabilities()
	{
		try
		{

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
			if (TestUtil.haveMinimumServerVersion(con,"7.3"))
				assertTrue(dbmd.supportsANSI92EntryLevelSQL());
			else
				assertTrue(!dbmd.supportsANSI92EntryLevelSQL());
			assertTrue(!dbmd.supportsANSI92IntermediateSQL());
			assertTrue(!dbmd.supportsANSI92FullSQL());

			assertTrue(dbmd.supportsIntegrityEnhancementFacility());

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

			DatabaseMetaData dbmd = con.getMetaData();
			assertNotNull(dbmd);

			assertTrue(dbmd.supportsOuterJoins());
			assertTrue(dbmd.supportsFullOuterJoins());
			assertTrue(dbmd.supportsLimitedOuterJoins());

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

			DatabaseMetaData dbmd = con.getMetaData();
			assertNotNull(dbmd);

			assertTrue(!dbmd.supportsPositionedDelete());
			assertTrue(!dbmd.supportsPositionedUpdate());

		}
		catch (SQLException ex)
		{
			fail(ex.getMessage());
		}
	}

	public void testValues()
	{
		try {
			DatabaseMetaData dbmd = con.getMetaData();
			assertNotNull(dbmd);
			int indexMaxKeys = dbmd.getMaxColumnsInIndex();
			if (TestUtil.haveMinimumServerVersion(con,"7.3")) {
				assertEquals(indexMaxKeys,32);
			} else {
				assertEquals(indexMaxKeys,16);
			}
		} catch (SQLException sqle) {
			fail(sqle.getMessage());
		}
	}

	public void testNulls()
	{
		try
		{

			DatabaseMetaData dbmd = con.getMetaData();
			assertNotNull(dbmd);

			assertTrue(!dbmd.nullsAreSortedAtStart());
			assertTrue( dbmd.nullsAreSortedAtEnd() != TestUtil.haveMinimumServerVersion(con,"7.2"));
			assertTrue( dbmd.nullsAreSortedHigh() == TestUtil.haveMinimumServerVersion(con,"7.2"));
			assertTrue(!dbmd.nullsAreSortedLow());

			assertTrue(dbmd.nullPlusNonNullIsNull());

			assertTrue(dbmd.supportsNonNullableColumns());

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

			DatabaseMetaData dbmd = con.getMetaData();
			assertNotNull(dbmd);

			assertTrue(!dbmd.usesLocalFilePerTable());
			assertTrue(!dbmd.usesLocalFiles());

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

			DatabaseMetaData dbmd = con.getMetaData();
			assertNotNull(dbmd);

			// we can add columns
			assertTrue(dbmd.supportsAlterTableWithAddColumn());

			// we can only drop columns in >= 7.3
			if (TestUtil.haveMinimumServerVersion(con,"7.3")) {
				assertTrue(dbmd.supportsAlterTableWithDropColumn());
			} else {
				assertTrue(!dbmd.supportsAlterTableWithDropColumn());
			}
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

			DatabaseMetaData dbmd = con.getMetaData();
			assertNotNull(dbmd);

			assertTrue(dbmd.getURL().equals(TestUtil.getURL()));
			assertTrue(dbmd.getUserName().equals(TestUtil.getUser()));

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
			assertTrue(con instanceof org.postgresql.PGConnection);
			org.postgresql.jdbc2.AbstractJdbc2Connection pc = (org.postgresql.jdbc2.AbstractJdbc2Connection) con;

			DatabaseMetaData dbmd = con.getMetaData();
			assertNotNull(dbmd);

			assertTrue(dbmd.getDatabaseProductName().equals("PostgreSQL"));
			//The test below doesn't make sense to me, it tests that
			//the version of the driver = the version of the database it is connected to
			//since the driver should be backwardly compatible this test is commented out
			//assertTrue(dbmd.getDatabaseProductVersion().startsWith(
			//		   Integer.toString(pc.getDriver().getMajorVersion())
			//		   + "."
			//		   + Integer.toString(pc.getDriver().getMinorVersion())));
			assertTrue(dbmd.getDriverName().equals("PostgreSQL Native Driver"));

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
			assertTrue(con instanceof org.postgresql.PGConnection);
			org.postgresql.jdbc2.AbstractJdbc2Connection pc = (org.postgresql.jdbc2.AbstractJdbc2Connection) con;

			DatabaseMetaData dbmd = con.getMetaData();
			assertNotNull(dbmd);

			assertTrue(dbmd.getDriverVersion().equals(org.postgresql.Driver.getVersion()));
			assertTrue(dbmd.getDriverMajorVersion() == pc.getDriver().getMajorVersion());
			assertTrue(dbmd.getDriverMinorVersion() == pc.getDriver().getMinorVersion());


		}
		catch (SQLException ex)
		{
			fail(ex.getMessage());
		}
	}
}

