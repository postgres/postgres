package org.postgresql;

import java.util.Hashtable;

/**
 * Just a factory class for creating and reusing 
 * ObjectPool arrays of different sizes.
 */
public class ObjectPoolFactory {
    private static Hashtable instances = new Hashtable();

    ObjectPool pool = new ObjectPool();
    int maxsize;

    public static ObjectPoolFactory getInstance(int size){
	Integer s = new Integer(size);
	ObjectPoolFactory poolFactory = (ObjectPoolFactory) instances.get(s);
	if(poolFactory == null){
	    synchronized(instances) {
		poolFactory = (ObjectPoolFactory) instances.get(s);
		if(poolFactory == null){
		    poolFactory = new ObjectPoolFactory(size);
		    instances.put(s, poolFactory);
		}
	    }
	}
	return poolFactory;
    }

    private ObjectPoolFactory(int maxsize){
	this.maxsize = maxsize;
    }
    
    public ObjectPool[] getObjectPoolArr(){
	ObjectPool p[] = null;
	synchronized(pool){
	    if(pool.size() > 0)
		p = (ObjectPool []) pool.remove();
	}
	if(p == null) {
	    p = new ObjectPool[maxsize];
	    for(int i = 0; i < maxsize; i++){
		p[i] = new ObjectPool();
	    }
	}
	return p;
    }

    public void releaseObjectPoolArr(ObjectPool p[]){
	synchronized(pool){
	    pool.add(p);
	    for(int i = 0; i < maxsize; i++){
		p[i].clear();
	    }
	}
    }
}
