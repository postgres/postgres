package org.postgresql.jdbc3;

import java.util.*;
import javax.naming.*;
import org.postgresql.jdbc2.optional.PGObjectFactory;

/**
 * JDBC3 version of the Object Factory used to recreate objects
 * from their JNDI references.
 *
 * @author Aaron Mulder (ammulder@alumni.princeton.edu)
 * @version $Revision: 1.1 $
 */
public class Jdbc3ObjectFactory extends PGObjectFactory
{
    /**
     * Dereferences a PostgreSQL DataSource.  Other types of references are
     * ignored.
     */
    public Object getObjectInstance(Object obj, Name name, Context nameCtx,
                                    Hashtable environment) throws Exception
    {
        Reference ref = (Reference) obj;
        if (ref.getClassName().equals(Jdbc3SimpleDataSource.class.getName()))
        {
            return loadSimpleDataSource(ref);
        }
        else if (ref.getClassName().equals(Jdbc3ConnectionPool.class.getName()))
        {
            return loadConnectionPool(ref);
        }
        else if (ref.getClassName().equals(Jdbc3PoolingDataSource.class.getName()))
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
        Jdbc3PoolingDataSource pds = Jdbc3PoolingDataSource.getDataSource(name);
        if (pds != null)
        {
            return pds;
        }
        // Otherwise, create a new one
        pds = new Jdbc3PoolingDataSource();
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
        Jdbc3SimpleDataSource ds = new Jdbc3SimpleDataSource();
        return loadBaseDataSource(ds, ref);
    }

    private Object loadConnectionPool(Reference ref)
    {
        Jdbc3ConnectionPool cp = new Jdbc3ConnectionPool();
        return loadBaseDataSource(cp, ref);
    }

}
