
#include <mach-o/dyld-interposing.h>
#include "foo.h"

int myfoo()
{
	return 20;
}

DYLD_INTERPOSE(myfoo, foo)
