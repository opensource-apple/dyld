
#include <strings.h>
#include <CoreFoundation/CoreFoundation.h>

#include "test.h"

void __attribute__((constructor)) init()
{
	uintptr_t libSysFuncAddr = (uintptr_t)&strcmp;
	uintptr_t cfFuncAddr     = (uintptr_t)&CFStringGetTypeID;
	if ( cfFuncAddr - libSysFuncAddr < 256*1024*1024 )
		PASS("read-only-import-shared-cache-override");
	else
		PASS("read-only-import-shared-cache-override");
}	



