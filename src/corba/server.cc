#include <iostream>
#include "pgsql_int.h"

bool terminate = false;

int main(int argc, char *argv)
{
	CORBA::ORB_var orb = CORBA::ORB_init(argc,argv,"");
	PortableManager::POA_var poa = PortableServer::POA::_narrow(orb->resolve_initial_references("RootPOA"));
	PortableManager::POAManager_var mgr = poa->the_POAManager();

	Server_impl *server = new Server_impl;
	poa->activate_object(server);

	CosNaming::NamingContext_var ctx = CosNaming::NamingContext::_narrow(orb->resolve_initial_references("NamingService"));
	CosNaming::Name_var n = new CosNaming::Name(1);
	n[0].id("PostgreSQL");
	n[0].name("service");
	bool bindok = false;

	if (!CORBA::Object::is_nil(ctx)) {
		try {
			CosNaming::NamingContext_var myctx = ctx->bind_new_context(n);
			CosNaming::Name_var n2 = new CosNaming::Name(1);
			n2[0].id("Server");
			n2[0].name("Server");
			myctx->bind(n2,server->_this());
			bindok = true;
		} catch (CORBA::Exception &e) {
			cerr << "Warning: Naming Service bind failed" << endl;
			bindok = false;
		}
	} else {
		cerr << "Warning: Naming Service not found" << endl;
	}

	mgr->activate();
	while (!terminate) {
		if (orb->work_pending())
			orb->perform_work();
		if (expiry_needed())
			expire_now();
	}

	if (!CORBA::Object::is_nil(ctx) && bindok) {
		try {
			CosNaming::NamingContext myctx = ctx->resolve(n);
			ctx->unbind(n);
			myctx->destroy();
		} catch (CORBA::Exception &e) {
			cerr << "Warning: Naming Service unbind failed" << endl;
		}
	}

	orb->shutdown(true);

	delete server;
	return 0;
}
