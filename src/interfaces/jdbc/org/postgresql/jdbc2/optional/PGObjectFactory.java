package org.postgresql.jdbc2.optional;

import javax.naming.spi.ObjectFactory;
import javax.naming.*;
import java.util.Hashtable;

/**
 * Returns a DataSource-ish thing based on a JNDI reference.  In the case of a
 * SimpleDataSource or ConnectionPool, a new instance is created each time, as
 * there is no connection state to maintain. In the case of a PoolingDataSource,
 * the same DataSource will be returned for every invocation within the same
 * VM/ClassLoader, so that the state of the connections in the pool will be
 * consistent.
 *
 * @author Aaron Mulder (ammulder@chariotsolutions.com)
 * @version $Revision: 1.3 $
 */
public class PGObjectFactory implements ObjectFactory
{
	/**
	 * Dereferences a PostgreSQL DataSource.  Other types of references are
	 * ignored.
	 */
	public Object getObjectInstance(Object obj, Name name, Context nameCtx,
									Hashtable environment) throws Exception
	{
		Reference ref = (Reference)obj;
		if (ref.getClassName().equals(SimpleDataSource.class.getName()))
		{
			return loadSimpleDataSource(ref);
		}
		else if (ref.getClassName().equals(ConnectionPool.class.getName()))
		{
			return loadConnectionPool(ref);
		}
		else if (ref.getClassName().equals(PoolingDataSource.class.getName()))
		{
			return loadPoolingDataSource(ref);
		}
		else
		{
			return null;
		}
	}

	private Object loadPoolingDataSource(Reference ref)
	{
		// If DataSource exists, return it
		String name = getProperty(ref, "dataSourceName");
		PoolingDataSource pds = PoolingDataSource.getDataSource(name);
		if (pds != null)
		{
			return pds;
		}
		// Otherwise, create a new one
		pds = new PoolingDataSource();
		pds.setDataSourceName(name);
		loadBaseDataSource(pds, ref);
		String min = getProperty(ref, "initialConnections");
		if (min != null)
		{
			pds.setInitialConnections(Integer.parseInt(min));
		}
		String max = getProperty(ref, "maxConnections");
		if (max != null)
		{
			pds.setMaxConnections(Integer.parseInt(max));
		}
		return pds;
	}

	private Object loadSimpleDataSource(Reference ref)
	{
		SimpleDataSource ds = new SimpleDataSource();
		return loadBaseDataSource(ds, ref);
	}

	private Object loadConnectionPool(Reference ref)
	{
		ConnectionPool cp = new ConnectionPool();
		return loadBaseDataSource(cp, ref);
	}

	protected Object loadBaseDataSource(BaseDataSource ds, Reference ref)
	{
		ds.setDatabaseName(getProperty(ref, "databaseName"));
		ds.setPassword(getProperty(ref, "password"));
		String port = getProperty(ref, "portNumber");
		if (port != null)
		{
			ds.setPortNumber(Integer.parseInt(port));
		}
		ds.setServerName(getProperty(ref, "serverName"));
		ds.setUser(getProperty(ref, "user"));
		return ds;
	}

    protected String getProperty(Reference ref, String s)
	{
		RefAddr addr = ref.get(s);
		if (addr == null)
		{
			return null;
		}
		return (String)addr.getContent();
	}

}
