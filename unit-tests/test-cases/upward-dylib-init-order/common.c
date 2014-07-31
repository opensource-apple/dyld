#include "common.h"
#include <stdio.h>

static bool b = false;
static bool u = false;
static bool isOk = true;

void setB()
{
	if ( u || b )
		isOk = false;
	b = true;
}

void setU()
{
	if ( u )
		isOk = false;
	u = true;
}

// return true iff
// setB() was called, then setU()
bool ok()
{
	//fprintf(stderr, "isOk=%d, u=%d, b=%d\n", isOk, u, b);
	return isOk && u && b;
}

