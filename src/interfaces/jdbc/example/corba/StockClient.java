package example.corba;

import java.io.*;
import java.sql.*;
import org.omg.CosNaming.*;

/*
 * This class is the frontend to our mini CORBA application.
 *
 * It has no GUI, just a text frontend to keep it simple.
 *
 * $Id: StockClient.java,v 1.6 2002/09/06 21:23:05 momjian Exp $
 */
public class StockClient
{
	org.omg.CosNaming.NamingContext nameService;

	stock.StockDispenser dispenser;
	stock.StockItem item;

	BufferedReader	in;

	public StockClient(String[] args)
	{
		try
		{
			// We need this for our IO
			in = new BufferedReader(new InputStreamReader(System.in));

			// Initialize the orb
			org.omg.CORBA.ORB orb = org.omg.CORBA.ORB.init(args, null);

			// Get a reference to the Naming Service
			org.omg.CORBA.Object nameServiceObj = orb.resolve_initial_references("NameService");
			if (nameServiceObj == null)
			{
				System.err.println("nameServiceObj == null");
				return ;
			}

			nameService = org.omg.CosNaming.NamingContextHelper.narrow(nameServiceObj);
			if (nameService == null)
			{
				System.err.println("nameService == null");
				return ;
			}

			// Resolve the dispenser
			NameComponent[] dispName = {
										   new NameComponent("StockDispenser", "Stock")
									   };
			dispenser = stock.StockDispenserHelper.narrow(nameService.resolve(dispName));
			if (dispenser == null)
			{
				System.err.println("dispenser == null");
				return ;
			}

			// Now run the front end.
			run();
		}
		catch (Exception e)
		{
			System.out.println(e.toString());
			e.printStackTrace();
			System.exit(1);
		}
	}

	public static void main(String[] args)
	{
		new StockClient(args);
	}

	public void run()
	{
		// First reserve a StockItem
		try
		{
			item = dispenser.reserveItem();
		}
		catch (Exception e)
		{
			System.out.println(e.toString());
			e.printStackTrace();
			System.exit(1);
		}

		mainMenu();

		// finally free the StockItem
		try
		{
			dispenser.releaseItem(item);
		}
		catch (Exception e)
		{
			System.out.println(e.toString());
			e.printStackTrace();
			System.exit(1);
		}
	}

	private void mainMenu()
	{
		boolean run = true;
		while (run)
		{
			System.out.println("\nCORBA Stock System\n");
			System.out.println("  1 Display stock item");
			System.out.println("  2 Remove item from stock");
			System.out.println("  3 Put item into stock");
			System.out.println("  4 Order item");
			System.out.println("  5 Display all items");
			System.out.println("  0 Exit");
			int i = getMenu("Main", 5);
			switch (i)
			{
				case 0:
					run = false;
					break;

				case 1:
					displayItem();
					break;

				case 2:
					bookOut();
					break;

				case 3:
					bookIn();
					break;

				case 4:
					order(0);
					break;

				case 5:
					displayAll();
					break;
			}
		}
	}

	private void displayItem()
	{
		try
		{
			int id = getMenu("\nStockID to display", item.getLastID());
			if (id > 0)
			{
				item.fetchItem(id);
				System.out.println("========================================");

				String status = "";
				if (!item.isItemValid())
					status = " ** Superceded **";

				int av = item.getAvailable();

				System.out.println("      Stock ID: " + id + status +
								   "\nItems Available: " + av +
								   "\nItems on order: " + item.getOrdered() +
								   "\n   Description: " + item.getDescription());
				System.out.println("========================================");

				if (av > 0)
					if (yn("Take this item out of stock?"))
					{
						int rem = 1;
						if (av > 1)
							rem = getMenu("How many?", av);
						if (rem > 0)
							item.removeStock(rem);
					}

			}
		}
		catch (Exception e)
		{
			System.out.println(e.toString());
			e.printStackTrace();
		}
	}

	private void bookOut()
	{
		try
		{
			int id = getMenu("\nStockID to take out", item.getLastID());
			if (id > 0)
			{
				item.fetchItem(id);
				int av = item.getAvailable();
				if (av > 0)
					if (yn("Take this item out of stock?"))
					{
						int rem = 1;
						if (av > 1)
							rem = getMenu("How many?", av);
						if (rem > 0)
							item.removeStock(rem);
					}
					else
					{
						System.out.println("This item is not in stock.");
						int order = item.getOrdered();
						if (order > 0)
							System.out.println("There are " + item.getOrdered() + " items on order.");
						else
						{
							if (item.isItemValid())
							{
								System.out.println("You will need to order some more " + item.getDescription());
								order(id);
							}
							else
								System.out.println("This item is now obsolete");
						}
					}
			}
			else
				System.out.println(item.getDescription() + "\nThis item is out of stock");
		}
		catch (Exception e)
		{
			System.out.println(e.toString());
			e.printStackTrace();
		}
	}

	// book an item into stock
	private void bookIn()
	{
		try
		{
			int id = getMenu("\nStockID to book in", item.getLastID());
			item.fetchItem(id);
			System.out.println(item.getDescription());

			if (item.getOrdered() > 0)
			{
				int am = getMenu("How many do you want to book in", item.getOrdered());
				if (am > 0)
					item.addNewStock(am);
			}
			else
				System.out.println("You don't have any of this item on ordered");

		}
		catch (Exception e)
		{
			System.out.println(e.toString());
			e.printStackTrace();
		}
	}

	// Order an item
	private void order(int id)
	{
		try
		{
			if (id == 0)
				id = getMenu("\nStockID to order", item.getLastID());
			item.fetchItem(id);
			System.out.println(item.getDescription());
			int am = getMenu("How many do you want to order", 999);
			if (am > 0)
				item.orderStock(am);
		}
		catch (Exception e)
		{
			System.out.println(e.toString());
			e.printStackTrace();
		}
	}

	private void displayAll()
	{
		try
		{
			boolean cont = true;
			int nr = item.getLastID();
			String header = "\nId\tAvail\tOrdered\tDescription";
			System.out.println(header);
			for (int i = 1;i <= nr && cont;i++)
			{
				item.fetchItem(i);
				System.out.println("" + i + "\t" + item.getAvailable() + "\t" + item.getOrdered() + "\t" + item.getDescription());
				if ((i % 20) == 0)
				{
					if ((cont = yn("Continue?")))
						System.out.println(header);
				}
			}
		}
		catch (Exception e)
		{
			System.out.println(e.toString());
			e.printStackTrace();
		}
	}

	private int getMenu(String title, int max)
	{
		int v = -1;
		while (v < 0 || v > max)
		{
			System.out.print(title);
			System.out.print(" [0-" + max + "]: ");
			System.out.flush();
			try
			{
				v = Integer.parseInt(in.readLine());
			}
			catch (Exception nfe)
			{
				v = -1;
			}
		}
		return v;
	}

	private boolean yn(String title)
	{
		try
		{
			while (true)
			{
				System.out.print(title);
				System.out.flush();
				String s = in.readLine();
				if (s.startsWith("y") || s.startsWith("Y"))
					return true;
				if (s.startsWith("n") || s.startsWith("N"))
					return false;
			}
		}
		catch (Exception nfe)
		{
			System.out.println(nfe.toString());
			nfe.printStackTrace();
			System.exit(1);
		}
		return false;
	}
}
