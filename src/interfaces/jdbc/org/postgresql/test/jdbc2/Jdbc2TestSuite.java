package org.postgresql.test.jdbc2;

import junit.framework.TestSuite;

/*
 * Executes all known tests for JDBC2 and includes some utility methods.
 */
public class Jdbc2TestSuite extends TestSuite
{

	/*
	 * The main entry point for JUnit
	 */
	public static TestSuite suite()
	{
		TestSuite suite = new TestSuite();

		//
		// Add one line per class in our test cases. These should be in order of
		// complexity.

		// ANTTest should be first as it ensures that test parameters are
		// being sent to the suite.
		//
		suite.addTestSuite(ANTTest.class);

		// Basic Driver internals
		suite.addTestSuite(DriverTest.class);
		suite.addTestSuite(ConnectionTest.class);
		suite.addTestSuite(DatabaseMetaDataTest.class);
		suite.addTestSuite(DatabaseMetaDataPropertiesTest.class);
		suite.addTestSuite(EncodingTest.class);

		// Connectivity/Protocols

		// ResultSet
		suite.addTestSuite(ResultSetTest.class);

		// Time, Date, Timestamp
		suite.addTestSuite(DateTest.class);
		suite.addTestSuite(TimeTest.class);
		suite.addTestSuite(TimestampTest.class);

		// PreparedStatement
		suite.addTestSuite(PreparedStatementTest.class);

		// ServerSide Prepared Statements
		suite.addTestSuite(ServerPreparedStmtTest.class);

		// BatchExecute
		suite.addTestSuite(BatchExecuteTest.class);


		// Other misc tests, based on previous problems users have had or specific
		// features some applications require.
		suite.addTestSuite(JBuilderTest.class);
		suite.addTestSuite(MiscTest.class);
		suite.addTestSuite(NotifyTest.class);

		// Fastpath/LargeObject
		suite.addTestSuite(BlobTest.class);
		suite.addTestSuite(OID74Test.class);

		suite.addTestSuite(UpdateableResultTest.class );

		suite.addTestSuite(CallableStmtTest.class );
		suite.addTestSuite(CursorFetchTest.class);
		suite.addTestSuite(ServerCursorTest.class);

		// That's all folks
		return suite;
	}
}
