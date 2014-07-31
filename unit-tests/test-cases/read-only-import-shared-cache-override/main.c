#include <dlfcn.h>
#include "test.h"

int main()
{
#if __arm__
	// no shared cache on iPhone, so skip test
	PASS("read-only-import-shared-cache-override");
#else
	// dynamically load libfoo.dylib which depends on libstdc++.dylib 
	// being re-bound to libfoo's operator new.
	dlopen("libfoo.dylib", RTLD_LAZY);
#endif
	return 0;
}	

