package uk.org.retep.dtu;

import uk.org.retep.util.Logger;

import java.util.Iterator;

/**
 * This class processes a Module. It's implemented as a Thread and there can
 * be many threads running on a single module
 */
public class DProcessor
{
  /**
   * This starts a module
   */
  public static DProcessor run(DModule aModule) {
    // 3600000 is 1 hour in milliseconds
    return run(aModule,3600000);
  }

  /**
   * This starts a module
   */
  public static DProcessor run(DModule aModule,long timeout) {
    return new DProcessor(aModule,timeout);
  }

  protected DProcessor(DModule aModule,long timeout) {
    ThreadGroup group = new ThreadGroup(aModule.getDisplayName()+" DProcessor");

    // Setup the environment
    DEnvironment env = new DEnvironment();

    // loop for any nodes without a transform pointing _to_ it.
    Iterator it = aModule.iterator();
    while(it.hasNext()) {
      DNode node = (DNode) it.next();

      // Only start if we have no predecessor
      if(node.getFromTransforms()==0) {
        proc proc = new proc(group,aModule,node,env);
        proc.start();
      }
    }

    // Now wait until all the threads have finished
    boolean running=true;
    try {
      int cnt=1; // must loop at least once!

      while(cnt>0) {
        int numThreads = group.activeCount();
        Thread threads[] = new Thread[numThreads];
        cnt = group.enumerate(threads,false);

        //Logger.log(Logger.DEBUG,"Waiting on threads",cnt);
        while(cnt>0) {
          //Logger.log(Logger.DEBUG,"Waiting on thread",cnt);
          threads[--cnt].join(timeout);
        }

        Logger.log(Logger.DEBUG,"All threads appear to have died, retesting");
      }
    } catch(InterruptedException ie) {
      Logger.log(Logger.ERROR,"DProcessor, exception caught while waiting for threads to die",ie);
    }

    // finally close any open datasources
    Logger.log(Logger.DEBUG,"DProcessor cleanup");

    Logger.log(Logger.DEBUG,"DProcessor finished");
  }

  class proc implements Runnable
  {
    protected DModule module; // The module being run
    protected DNode   pc;     // current Program Counter

    protected DEnvironment env; // Shared environment

    // Used when launching new threads only
    protected DTransform trans; // If not null, a transform to run first
    protected int status;

    protected Thread thread;

    /**
     * Start processing from DNode aNode. This is called by DProcessor at
     * initialisation only.
     */
    protected proc(ThreadGroup aGroup,DModule aModule,DNode aNode,DEnvironment aEnv)
    {
      // aGroup will be null when forking...
      if(aGroup==null) {
        thread = new Thread(this);
      } else {
        thread = new Thread(aGroup,this);
      }

      module = aModule;
      pc = aNode;
      env = aEnv;
    }

    /**
     * Start processing the DTransform aTransform from aNode (does not execute
     * the node). This is called by this inner class itself when forking new
     * threads.
     */
    protected proc(DModule aModule,DNode aNode,DEnvironment aEnv,DTransform aTransform,int aStatus)
    {
      this(null,aModule,aNode,aEnv);
      trans = aTransform;
      status = aStatus;
    }

    /**
     * Start this thread
     */
    public void start()
    {
      thread.start();
    }

    public void run()
    {
      // Handle an initial transform first. It's used when a new Thread was created.
      if(trans!=null) {
        transform(trans,false,status);
        trans=null;
      }

      while(pc!=null) {
        //Logger.log(Logger.DEBUG,"running node ",pc.getID());

        // Process the node
        int status = pc.run(env);
        //Logger.log(Logger.DEBUG,"      status ",status);

        // Now the transforms. This thread continues with the first one that runs,
        // but any others that will also run will do so in their own thread.
        // If no transform runs (or there are none), then the thread will die.
        int numTrans = pc.getToTransforms();
        boolean fork=false;
        for(int i=0;i<numTrans;i++) {
          fork = transform(pc.getTransform(i),fork,status);
          //Logger.log(Logger.DEBUG,"fork",fork?"true":"false");
        }
        //Logger.log(Logger.DEBUG,"fork",fork?"true":"false");
        if(!fork) {
          // No transforms ran, so we quit this thread
          pc=null;
        }

        // This lets the other threads a chance to run
        Thread.yield();
      }
    }

    /**
     * This executes a transform
     * @param aTransform DTransform to execute
     * @param fork true then a new process is triggered
     * @param status The return status of the previous node
     * @return true if the transform ran or a fork occured.
     */
    public boolean transform(DTransform aTransform,boolean fork,int status)
    {
      // Check to see if the transform will run (based on the calling nodes return
      // status
      if(!aTransform.willRun(status,env)) {
        return false;
      }

      if(fork) {
        // Create the new processor but this time we want a transform to run
        proc proc = new proc(module,pc,env,aTransform,status);
        return true;
      }

      if(aTransform.run(env)) {
        pc=aTransform.getTo();
        return true;
      }

      return false;
    }

  } // class proc

}