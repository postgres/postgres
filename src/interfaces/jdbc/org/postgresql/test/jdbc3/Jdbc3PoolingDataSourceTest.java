package org.postgresql.test.jdbc3;

import java.sql.Connection;
import java.sql.SQLException;
import javax.naming.InitialContext;
import javax.naming.NamingException;
import org.postgresql.test.jdbc2.optional.PoolingDataSourceTest;
import org.postgresql.test.TestUtil;
import org.postgresql.jdbc3.Jdbc3PoolingDataSource;
import org.postgresql.jdbc2.optional.PoolingDataSource;

/**
 * Minimal tests for JDBC3 pooling DataSource.  Needs many more.
 *
 * @author Aaron Mulder (ammulder@chariotsolutions.com)
 * @version $Revision: 1.1.6.1 $
 */
public class Jdbc3PoolingDataSourceTest extends PoolingDataSourceTest
{
    private final static String DS_NAME = "JDBC 3 Test DataSource";

    /**
     * Constructor required by JUnit
     */
    public Jdbc3PoolingDataSourceTest(String name)
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
            bds = new Jdbc3PoolingDataSource();
            configureDataSource((Jdbc3PoolingDataSource) bds);
        }
    }

    private void configureDataSource(PoolingDataSource source)
    {
        String db = TestUtil.getURL();
        source.setServerName(TestUtil.getServer());
        source.setPortNumber(TestUtil.getPort());
        source.setDatabaseName(TestUtil.getDatabase());
        source.setUser(TestUtil.getUser());
        source.setPassword(TestUtil.getPassword());
        source.setDataSourceName(DS_NAME);
        source.setInitialConnections(2);
        source.setMaxConnections(10);
    }

    /**
     * Check that 2 DS instances can't use the same name.
     */
    public void testCantReuseName()
    {
        initializeDataSource();
        Jdbc3PoolingDataSource pds = new Jdbc3PoolingDataSource();
        try
        {
            pds.setDataSourceName(DS_NAME);
            fail("Should have denied 2nd DataSource with same name");
        }
        catch (IllegalArgumentException e)
        {
        }
    }

    /**
     * Test that JDBC 2 and JDBC 3 DSs come from different buckets
     * as far as creating with the same name
     */
    public void testDifferentImplPools()
    {
        initializeDataSource();
        PoolingDataSource pds = new PoolingDataSource();
        try
        {
            configureDataSource(pds);
            PoolingDataSource p2 = new PoolingDataSource();
            try
            {
                configureDataSource(p2);
                fail("Shouldn't be able to create 2 JDBC 2 DSs with same name");
            }
            catch (IllegalArgumentException e)
            {
            }
            Jdbc3PoolingDataSource p3 = new Jdbc3PoolingDataSource();
            try
            {
                configureDataSource(p3);
                fail("Shouldn't be able to create 2 JDBC 3 DSs with same name");
            }
            catch (IllegalArgumentException e)
            {
            }
        }
        finally
        {
            pds.close();
        }
    }

    /**
     * Test that JDBC 2 and JDBC 3 DSs come from different buckets
     * as far as fetching from JNDI
     */
    public void testDifferentImplJndi()
    {
        initializeDataSource();
        PoolingDataSource pds = new PoolingDataSource();
        try
        {
            configureDataSource(pds);
            try
            {
                Connection j3c = getDataSourceConnection();
                Connection j2c = pds.getConnection();
                j2c.close();
                j3c.close();
                InitialContext ctx = getInitialContext();
                ctx.bind("JDBC2", pds);
                ctx.bind("JDBC3", bds);
                pds = (PoolingDataSource) ctx.lookup("JDBC2");
                bds = (Jdbc3PoolingDataSource) ctx.lookup("JDBC3");
                j2c = pds.getConnection();
                j3c = bds.getConnection();
                j2c.close();
                j3c.close();
            }
            catch (SQLException e)
            {
                fail(e.getMessage());
            }
            catch (NamingException e)
            {
                fail(e.getMessage());
            }
        }
        finally
        {
            pds.close();
        }
    }
}
