package org.postgresql.test.jdbc2.optional;

import org.postgresql.test.TestUtil;
import org.postgresql.jdbc2.optional.SimpleDataSource;

/**
 * Performs the basic tests defined in the superclass.	Just adds the
 * configuration logic.
 *
 * @author Aaron Mulder (ammulder@chariotsolutions.com)
 * @version $Revision: 1.3.6.1 $
 */
public class SimpleDataSourceTest extends BaseDataSourceTest
{
	/**
	 * Constructor required by JUnit
	 */
	public SimpleDataSourceTest(String name)
	{
		super(name);
	}

	/**
	 * Creates and configures a new SimpleDataSource.
	 */
	protected void initializeDataSource()
	{
		if (bds == null)
		{
			bds = new SimpleDataSource();
			bds.setServerName(TestUtil.getServer());
			bds.setPortNumber(TestUtil.getPort());
			bds.setDatabaseName(TestUtil.getDatabase());
			bds.setUser(TestUtil.getUser());
			bds.setPassword(TestUtil.getPassword());
		}
	}
}
