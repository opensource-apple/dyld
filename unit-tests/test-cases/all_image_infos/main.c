/*
 * Copyright (c) 2008 Apple Inc. All rights reserved.
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
#include <stdlib.h> // EXIT_SUCCESS
#include <stdio.h> 
#include <mach-o/dyld.h>
#include <mach-o/dyld_images.h>
#include <mach/mach.h>

#include "test.h"

extern struct mach_header __dso_handle;

#ifndef DYLD_ALL_IMAGE_INFOS_OFFSET_OFFSET
	#define DYLD_ALL_IMAGE_INFOS_OFFSET_OFFSET 0x1010
#endif


#if __i386__ || __ppc__
	#define DYLD_BASE_ADDRESS 0x8fe00000
#elif __x86_64__ || __ppc64__
	#define DYLD_BASE_ADDRESS 0x7fff5fc00000
#elif __arm__
	#define DYLD_BASE_ADDRESS 0x2fe00000
#endif

struct dyld_all_image_infos* getImageInfos()
{
	uint32_t offset = *((uint32_t*)(DYLD_BASE_ADDRESS+DYLD_ALL_IMAGE_INFOS_OFFSET_OFFSET));
	if ( offset > 300000 ) {
		FAIL("all_image_infos: offset appears to be outside dyld");
		exit(0);
	}
	uintptr_t addr = DYLD_BASE_ADDRESS + offset;
	return (struct dyld_all_image_infos*)addr;
}


struct dyld_all_image_infos* getImageInfosFromKernel()
{
	task_dyld_info_data_t task_dyld_info;
	mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;
    
    if ( task_info(mach_task_self(), TASK_DYLD_INFO, (task_info_t)&task_dyld_info, &count) ) {
		FAIL("all_image_infos: task_info() failed");
		exit(0);
	}
	return (struct dyld_all_image_infos*)(uintptr_t)task_dyld_info.all_image_info_addr;
}


int
main()
{
	struct dyld_all_image_infos* infos = getImageInfos();
	if ( infos->version != 7 ) {
		FAIL("all_image_infos: dyld_all_image_infos is not version 7");
		exit(0);
	}

	if ( infos->infoArrayCount < 2 ) {
		FAIL("all_image_infos: dyld_all_image_infos.infoArrayCount is  < 2");
		exit(0);
	}

	if ( infos->infoArray[0].imageLoadAddress != &__dso_handle ) {
		FAIL("all_image_infos: dyld_all_image_infos.infoArray for main executable is wrong");
		exit(0);
	}

	if ( getImageInfosFromKernel() != infos ) {
		FAIL("all_image_infos: task_info and dyld disagree");
		exit(0);
	}

	PASS("all_image_infos");
	return EXIT_SUCCESS;
}


