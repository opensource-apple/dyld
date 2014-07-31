/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2004 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

int dummy_dyld_symbol = 1;

#include <stdlib.h>

// The following API's are deprecated.
// In Mac OS X 10.4 we only do a minimal implementation of these API's 
// to keep from breaking existing clients (Omni and MS Crash Reporters).
//
// This minmal implementation only allows inspection of one process and
// only reveals the current images in that process (no notification of
// later image changes). It assumes both processes use the same dyld
// and dyld has not slid in either process.
//
#if __ppc__

#include "mach-o/dyld_debug.h"
#include "mach-o/dyld_gdb.h"
#include "mach-o/dyld_priv.h"

// global state set up by _dyld_debug_subscribe_to_events() and accessed by _dyld_debug_module_name()
static const struct dyld_image_info*	sImages = NULL;
static uint32_t							sImagesCount = 0;
static task_port_t						sImagesTaskPort = 0;


// reads an address range out of another process
// returns a malloc'ed block that caller should free
static void* xprocess_read(task_port_t target_task, const void* address, size_t len)
{
	void* result = NULL;
	mach_vm_address_t page_address = (uint32_t)address & (-4096);
	mach_vm_address_t  last_page_address = ((uint32_t)address + len + 4095) & (-4096);
	mach_vm_size_t page_size = last_page_address - page_address;
	uint8_t* local_start;
	uint32_t local_len;
	kern_return_t r = vm_read(
		target_task,
		page_address,
		page_size,
		(vm_offset_t*)&local_start,
		&local_len);
	if ( r == KERN_SUCCESS ) {
		result = malloc(len);
		if ( result != NULL ) 
			memcpy(result, &local_start[(uint32_t)address - page_address], len);
		vm_deallocate(mach_task_self(), (uintptr_t)local_start, local_len);
	}
	return result;
}

// reads a c-string out of another process.  The returned string should be vm_deallocated.
// All strings must be less than 1 to 2 pages long
static const char* xprocess_read_string(task_port_t target_task, const void* address)
{
	char* result = NULL;
	mach_vm_address_t page_address = (uint32_t)address & (-4096);
	mach_vm_size_t page_size = 0x2000;	// always read two pages
	uint8_t* local_start;
	uint32_t local_len;
	kern_return_t r = vm_read(
		target_task,
		page_address,
		page_size,
		(vm_offset_t*)&local_start,
		&local_len);
	if ( r == KERN_SUCCESS ) {
		const char* str = (char*)&local_start[(uint32_t)address - page_address];
		return str;
	}
	return result;
}


// SPI into dyld to get address of _dyld_get_all_image_infos data structure
static const struct dyld_all_image_infos* dyld_get_all_image_infos()
{
    static const struct dyld_all_image_infos* (*p)() = NULL;

	if ( p == NULL )
	    _dyld_func_lookup("__dyld_get_all_image_infos", (void**)&p);
	
	if ( p != NULL )
		return p();
	else
		return NULL;
}



/*
 * _dyld_debug_module_name() is passed a dyld_debug_module struct and
 * sets image_name and module_name as well as the nameCnts.  If the module
 * does not refer to a valid module DYLD_INVALID_ARGUMENTS is returned.
 */
enum dyld_debug_return
_dyld_debug_module_name(
task_port_t target_task,
unsigned long send_timeout,
unsigned long rcv_timeout,
boolean_t inconsistent_data_ok,
struct dyld_debug_module module,
char **image_name,
unsigned long *image_nameCnt,
char **module_name,
unsigned long *module_nameCnt)
{
	// examine sImage* info set up by _dyld_debug_subscribe_to_events()
	if ( sImagesTaskPort == target_task ) {
		unsigned int i;
		for (i=0; i < sImagesCount; ++i) {
			if ( module.header == sImages[i].imageLoadAddress ) {
				// copy requested string
				const char* path = xprocess_read_string(sImagesTaskPort, sImages[i].imageFilePath);
				if ( path != NULL ) {
					*image_name = (char*)path;
					*image_nameCnt = strlen(path);
					*module_name = NULL;
					*module_nameCnt = 0;
					return DYLD_SUCCESS;
				}
			}
		}	
	}

	// not supported
	return DYLD_INVALID_ARGUMENTS;
}


/*
 * set_dyld_debug_error_func() is called to register a function to be called
 * when error occurs in the dyld debug API's.
 */
void
_dyld_debug_set_error_func(
void (*func)(struct dyld_debug_error_data *e))
{
	// do nothing
}



// Examine a mach_header in another process and determine its slide
static ptrdiff_t slideForHeader(task_port_t target_task, const struct mach_header* otherAddressHeader)
{
	ptrdiff_t result = 0;
	const struct mach_header* mh = xprocess_read(target_task, otherAddressHeader, 0x2000);
	if ( mh != NULL ) {
		int i;
		const struct segment_command *sgp = 
			(const struct segment_command *)((char*)mh + sizeof(mh));

		for (i = 0; i < mh->ncmds; i++){
			if (sgp->cmd == LC_SEGMENT) {
				if (sgp->fileoff == 0  &&  sgp->filesize != 0) {
					result = (uintptr_t)mh - (uintptr_t)sgp->vmaddr;
					break;
				}
			}
			sgp = (const struct segment_command *)((char *)sgp + sgp->cmdsize);
		}
		free((void*)mh);
	}
	return result;  
}


/*
 * _dyld_debug_subscribe_to_events creates a new thread that is will call the
 * specified dyld_event_routine when dynamic link events occur in the target
 * task.  This uses _dyld_debug_add_event_subscriber() and is just a different
 * interface to get events.
 */
enum dyld_debug_return
_dyld_debug_subscribe_to_events(
task_port_t target_task,
unsigned long send_timeout,
unsigned long rcv_timeout,
boolean_t inconsistent_data_ok,
void (*dyld_event_routine)(struct dyld_event event))
{
	sImages = NULL;
	sImagesCount = 0;
	sImagesTaskPort = 0;
	// get location of dyld_get_all_image_infos in this process
	// It is possible that dyld slid in one of the processes, in which case this fails
	const struct dyld_all_image_infos* infoOtherAddressSpace = dyld_get_all_image_infos();
	if ( infoOtherAddressSpace != NULL ) {
		const struct dyld_all_image_infos* infos;
		infos = (const struct dyld_all_image_infos*)xprocess_read(target_task, infoOtherAddressSpace, sizeof(struct dyld_all_image_infos));
		if ( infos != NULL ) {
			// sanity check version
			if ( infos->version == 1 ) {
				sImages = xprocess_read(target_task, infos->infoArray, infos->infoArrayCount * sizeof(struct dyld_image_info));
				if ( sImages != NULL ) {
					int i;
					// save info info into sImage* globals for use by later calls to _dyld_debug_module_name()
					sImagesCount = infos->infoArrayCount;
					sImagesTaskPort = target_task;
					// tell caller about every image
					for (i=0; i < infos->infoArrayCount; ++i) {
						struct dyld_event addEvent;
						bzero(&addEvent, sizeof(struct dyld_event));
						const struct mach_header* mh = sImages[i].imageLoadAddress;
						addEvent.type = DYLD_IMAGE_ADDED;
						addEvent.arg[0].header		 = (struct mach_header*)mh;
						addEvent.arg[0].vmaddr_slide = slideForHeader(target_task, mh);
						addEvent.arg[0].module_index = 0;
						(*dyld_event_routine)(addEvent);
					}
				}
			}
			// we free the dyld_all_image_infos struct, but not the array we copied
			// The array is left in sImage* for use by later calls to _dyld_debug_module_name()
			free((void*)infos);
		}
	}
	
	// tell client event handler no more images
	struct dyld_event event;
	event.type = DYLD_PAST_EVENTS_END;
	(*dyld_event_routine)(event);

	return DYLD_SUCCESS;
}

#endif
