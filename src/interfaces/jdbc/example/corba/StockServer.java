package example.corba;

import org.omg.CosNaming.*;

/*
 * This class implements the server side of the example.
 *
 * $Id: StockServer.java,v 1.5 2002/09/06 21:23:05 momjian Exp $
 */
public class StockServer
{
	public static void main(String[] args)
	{
		int numInstances = 3;

		try
		{
			// Initialise the ORB
			org.omg.CORBA.ORB orb = org.omg.CORBA.ORB.init(args, null);

			// Create the StockDispenser object
			StockDispenserImpl dispenser = new StockDispenserImpl(args, "Stock Dispenser", numInstances);

			// Export the new object
			orb.connect(dispenser);

			// Get the naming service
			org.omg.CORBA.Object nameServiceObj = orb.resolve_initial_references("NameService");
			if (nameServiceObj == null)
			{
				System.err.println("nameServiceObj = null");
				return ;
			}

			org.omg.CosNaming.NamingContext nameService = org.omg.CosNaming.NamingContextHelper.narrow(nameServiceObj);
			if (nameService == null)
			{
				System.err.println("nameService = null");
				return ;
			}

			// bind the dispenser into the naming service
			NameComponent[] dispenserName = {
												new NameComponent("StockDispenser", "Stock")
											};
			nameService.rebind(dispenserName, dispenser);

			// Now wait forever for the current thread to die
			Thread.currentThread().join();
		}
		catch (Exception e)
		{
			e.printStackTrace();
		}
	}
}


