package example.corba;

import org.omg.CosNaming.*;

/*
 * This class implements the server side of the example.
 *
 * $Id: StockItemImpl.java,v 1.3 2001/11/19 22:43:13 momjian Exp $
 */
public class StockItemImpl extends stock._StockItemImplBase
{
	private StockDB db;
	private String instanceName;

	public StockItemImpl(String[] args, String iname)
	{
		super();
		try
		{
			db = new StockDB();
			db.connect(args[1], args[2], args[3]);
			System.out.println("StockDB object " + iname + " created");
			instanceName = iname;
		}
		catch (Exception e)
		{
			e.printStackTrace();
		}
	}

	/*
	 * This is defined in stock.idl
	 *
	 * It sets the item to view
	 */
	public void fetchItem(int id) throws stock.StockException
	{
		try
		{
			db.fetchItem(id);
		}
		catch (Exception e)
		{
			throw new stock.StockException(e.toString());
		}
	}


	/*
	 * This is defined in stock.idl
	 *
	 * It sets the item to view
	 */
	public int newItem() throws stock.StockException
	{
		try
		{
			return db.newItem();
		}
		catch (Exception e)
		{
			throw new stock.StockException(e.toString());
		}
	}

	/*
	 * This is defined in stock.idl
	 *
	 * It returns the description of a Stock item
	 */
	public String getDescription() throws stock.StockException
	{
		try
		{
			return db.getDescription();
		}
		catch (Exception e)
		{
			throw new stock.StockException(e.toString());
		}
	}

	/*
	 * This is defined in stock.idl
	 *
	 * It returns the description of a Stock item
	 */
	public int getAvailable() throws stock.StockException
	{
		try
		{
			return db.getAvailable();
		}
		catch (Exception e)
		{
			throw new stock.StockException(e.toString());
		}
	}

	/*
	 * This is defined in stock.idl
	 *
	 * It returns the description of a Stock item
	 */
	public int getOrdered() throws stock.StockException
	{
		try
		{
			return db.getOrdered();
		}
		catch (Exception e)
		{
			throw new stock.StockException(e.toString());
		}
	}

	/*
	 * This is defined in stock.idl
	 *
	 * It returns the description of a Stock item
	 */
	public boolean isItemValid() throws stock.StockException
	{
		try
		{
			return db.isItemValid();
		}
		catch (Exception e)
		{
			throw new stock.StockException(e.toString());
		}
	}

	/*
	 * This is defined in stock.idl
	 *
	 * It returns the description of a Stock item
	 */
	public void addNewStock(int id) throws stock.StockException
	{
		try
		{
			db.addNewStock(id);
		}
		catch (Exception e)
		{
			throw new stock.StockException(e.toString());
		}
	}

	/*
	 * This is defined in stock.idl
	 *
	 * It returns the description of a Stock item
	 */
	public void removeStock(int id) throws stock.StockException
	{
		try
		{
			db.removeStock(id);
		}
		catch (Exception e)
		{
			throw new stock.StockException(e.toString());
		}
	}

	/*
	 * This is defined in stock.idl
	 *
	 * It returns the description of a Stock item
	 */
	public void orderStock(int id) throws stock.StockException
	{
		try
		{
			db.orderStock(id);
		}
		catch (Exception e)
		{
			throw new stock.StockException(e.toString());
		}
	}

	/*
	 * This returns the highest id used, hence the number of items available
	 */
	public int getLastID() throws stock.StockException
	{
		try
		{
			return db.getLastID();
		}
		catch (Exception e)
		{
			throw new stock.StockException(e.toString());
		}
	}

	/*
	 * This is used by our Dispenser
	 */
	public String getInstanceName()
	{
		return instanceName;
	}
}

