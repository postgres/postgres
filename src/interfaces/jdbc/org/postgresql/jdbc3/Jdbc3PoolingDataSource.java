package org.postgresql.jdbc3;

import java.util.*;
import org.postgresql.jdbc2.optional.PoolingDataSource;
import org.postgresql.jdbc2.optional.ConnectionPool;

import javax.naming.Reference;

/**
 * JDBC 3 implementation of a pooling DataSource.  This is best
 * used outside of an application server environment.  Application
 * servers generally prefer to deal with instances of
 * ConnectionPoolDataSource (see ConnectionPool) or XADataSource
 * (not available for PostgreSQL).
 *
 * @author Aaron Mulder (ammulder@alumni.princeton.edu)
 * @version $Revision: 1.1 $
 */
public class Jdbc3PoolingDataSource extends PoolingDataSource
{
    /**
     * Store JDBC3 DataSources in different bucket than JDBC2 DataSources
     */
    private static Map dataSources = new HashMap();

    /**
     * Store JDBC3 DataSources in different bucket than JDBC2 DataSources
     */
    static Jdbc3PoolingDataSource getDataSource(String name)
    {
        return (Jdbc3PoolingDataSource) dataSources.get(name);
    }

    /**
     * Store JDBC3 DataSources in different bucket than JDBC2 DataSources
     */
    protected void removeStoredDataSource()
    {
        synchronized (dataSources)
        {
            dataSources.remove(dataSourceName);
        }
    }

    /**
     * Store JDBC3 DataSources in different bucket than JDBC2 DataSources
     */
    public void setDataSourceName(String dataSourceName)
    {
        if (isInitialized())
        {
            throw new IllegalStateException("Cannot set Data Source properties after DataSource has been used");
        }
        if (this.dataSourceName != null && dataSourceName != null && dataSourceName.equals(this.dataSourceName))
        {
            return;
        }
        synchronized (dataSources)
        {
            if (getDataSource(dataSourceName) != null)
            {
                throw new IllegalArgumentException("DataSource with name '" + dataSourceName + "' already exists!");
            }
            if (this.dataSourceName != null)
            {
                dataSources.remove(this.dataSourceName);
            }
            this.dataSourceName = dataSourceName;
            dataSources.put(dataSourceName, this);
        }
    }

    /**
     * Generates a JDBC3 object factory reference.
     */
    protected Reference createReference()
    {
        return new Reference(getClass().getName(), Jdbc3ObjectFactory.class.getName(), null);
    }

    /**
     * Creates a JDBC3 ConnectionPool to use with this DataSource.
     */
    protected ConnectionPool createConnectionPool()
    {
        return new Jdbc3ConnectionPool();
    }

    /**
     * Gets a description of this DataSource.
     */
    public String getDescription()
    {
        return "JDBC3 Pooling DataSource from " + org.postgresql.Driver.getVersion();
    }
}
