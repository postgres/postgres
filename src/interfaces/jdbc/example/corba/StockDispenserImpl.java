package example.corba;

import org.omg.CosNaming.*;

/*
 * This class implements the server side of the example.
 *
 * $Id: StockDispenserImpl.java,v 1.5 2002/09/06 21:23:05 momjian Exp $
 */
public class StockDispenserImpl extends stock._StockDispenserImplBase
{
	private int maxObjects = 10;
	private int numObjects = 0;
	private StockItemStatus[] stock = new StockItemStatus[maxObjects];

	public StockDispenserImpl(String[] args, String name, int num)
	{
		super();

		try
		{
			// get reference to orb
			org.omg.CORBA.ORB orb = org.omg.CORBA.ORB.init(args, null);

			// prestart num objects
			if (num >= maxObjects)
				num = maxObjects;
			numObjects = num;
			for (int i = 0;i < numObjects;i++)
			{
				stock[i] = new StockItemStatus();
				stock[i].ref = new StockItemImpl(args, "StockItem" + (i + 1));
				orb.connect(stock[i].ref);
			}
		}
		catch (org.omg.CORBA.SystemException e)
		{
			e.printStackTrace();
		}
	}

	/*
	 * This method, defined in stock.idl, reserves a slot in the dispenser
	 */
	public stock.StockItem reserveItem() throws stock.StockException
	{
		for (int i = 0;i < numObjects;i++)
		{
			if (!stock[i].inUse)
			{
				stock[i].inUse = true;
				System.out.println("Reserving slot " + i);
				return stock[i].ref;
			}
		}
		return null;
	}

	/*
	 * This releases a slot from the dispenser
	 */
	public void releaseItem(stock.StockItem item) throws stock.StockException
	{
		for (int i = 0;i < numObjects;i++)
		{
			if (stock[i].ref.getInstanceName().equals(item.getInstanceName()))
			{
				stock[i].inUse = false;
				System.out.println("Releasing slot " + i);
				return ;
			}
		}
		System.out.println("Reserved object not a member of this dispenser");
		return ;
	}

	/*
	 * This class defines a slot in the dispenser
	 */
	class StockItemStatus
	{
		StockItemImpl ref;
		boolean inUse;

		StockItemStatus()
		{
			ref = null;
			inUse = false;
		}
	}

}
