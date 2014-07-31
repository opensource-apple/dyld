/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2004-2005 Apple Computer, Inc. All rights reserved.
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

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <mach-o/loader.h>

#include <vector>

#include "mach-o/dyld_gdb.h"


// old gdb interface to dyld only supported on 32-bit ppc and i386 (not ppc64_
#if OLD_GDB_DYLD_INTERFACE

unsigned int gdb_dyld_version = 2;


/*
 * gdb_dyld_state_changed() is a dummy routine called by dyld after images get
 * added or removed/ Gdb is expected to set a break point at
 * gdb_dyld_state_changed() then re-read dyld internal data as specified in
 * the header file dyld_gdb.h
 */
void gdb_dyld_state_changed()
{
	// do nothing
}

#define NLIBRARY_IMAGES 200
#define NOBJECT_IMAGES 1


struct image {   
	const char*			physical_name;		// physical image name (file name)
	uint32_t			vmaddr_slide;		// the slide from the staticly linked address
	const mach_header*  mh;					// address of the mach header of the image
	uint32_t			valid;				// TRUE if this is struct is valid
	const char*			name;				// image name for reporting errors
};


struct library_images { 
	struct image			images[NLIBRARY_IMAGES];
	uint32_t				nimages;
	struct library_images*  next_images;
};
struct object_images { 
	struct image			images[NOBJECT_IMAGES];
	uint32_t				nimages;
	struct library_images*  next_images;
};

unsigned int gdb_nobject_images		= NOBJECT_IMAGES;
unsigned int gdb_object_image_size	= sizeof(image);
unsigned int gdb_nlibrary_images	= NLIBRARY_IMAGES;
unsigned int gdb_library_image_size	= sizeof(image);

extern "C" {
object_images   object_images = { {}, 0 , NULL };
library_images library_images = { {}, 0 , NULL };
void send_event(const struct dyld_event* event);
}


enum dyld_event_type {
    DYLD_IMAGE_ADDED = 0,
    DYLD_IMAGE_REMOVED = 5
};

struct dyld_event {
    enum dyld_event_type		type;
    const struct mach_header*   header;
    uintptr_t					slide;
};


// gdb only notices changes bundles/dylibs loaded at runtime
// if the "send_event()" function in dyld is called...
void send_event(const struct dyld_event* event);
void (*send_event_ptr)(const struct dyld_event* event) = &send_event;

void addImageForgdb(const mach_header* mh, uintptr_t slide, const char* physicalPath, const char* logicalPath)
{
	struct library_images* li = &library_images;
	while ( li->nimages >= NLIBRARY_IMAGES ) {
		if ( li->next_images == NULL ) {
			struct library_images* li2 = new struct library_images();
			li2->nimages = 0;
			li2->next_images = NULL;
			li->next_images = li2;
			li = li2;
		}
		else {
			li = li->next_images;
		}
	}
	image* info = &li->images[li->nimages++];
	info->physical_name		= physicalPath;
	info->vmaddr_slide		= slide;
	info->mh				= mh;
	info->valid				= 1;
	info->name				= logicalPath;
	
	// ping gdb about change
	dyld_event event;
	event.type = DYLD_IMAGE_ADDED;
	event.header = mh;
	event.slide = slide;
	
	// we have to indirect through a function pointer to keep gcc-3.5 from inlining away the function call
	// rdar://problem/3830560
	(*send_event_ptr)(&event);
}

// move this to after use, otherwise gcc will see it has an empty implementation and
// optimize away the call site
void send_event(const struct dyld_event* event)
{
	// This function exists to let gdb set a break point
	// and catch libraries being added...
}


void removeImageForgdb(const mach_header* mh)
{
	for (struct library_images* li = &library_images; li != NULL; li = li->next_images) {
		for( uint32_t n=0; n < li->nimages; ++n) {
			struct image* image = &li->images[n];
			if ( image->mh == mh ) {
				image->physical_name = NULL;
				image->vmaddr_slide = 0;
				image->mh			= 0;
				image->valid		= 0;
				image->name			= NULL;
				return;
			}
		}
	}
}

#endif

static std::vector<dyld_image_info> sImageInfos;



void addImagesToAllImages(uint32_t infoCount, const dyld_image_info info[])
{
	// set infoArray to NULL to denote it is in-use
	dyld_all_image_infos.infoArray = NULL;
	
	// append all new images
	for (uint32_t i=0; i < infoCount; ++i)
		sImageInfos.push_back(info[i]);
	dyld_all_image_infos.infoArrayCount = sImageInfos.size();
	
	// set infoArray back to base address of vector
	dyld_all_image_infos.infoArray = &sImageInfos[0];

	// tell gdb that about the new images
	dyld_all_image_infos.notification(dyld_image_adding, infoCount, info);
}

void removeImageFromAllImages(const struct mach_header* loadAddress)
{
	dyld_image_info goingAway;
	
	// set infoArray to NULL to denote it is in-use
	dyld_all_image_infos.infoArray = NULL;
	
	// remove image from infoArray
	for (std::vector<dyld_image_info>::iterator it=sImageInfos.begin(); it != sImageInfos.end(); it++) {
		if ( it->imageLoadAddress == loadAddress ) {
			goingAway = *it;
			sImageInfos.erase(it);
			break;
		}
	}
	dyld_all_image_infos.infoArrayCount = sImageInfos.size();
	
	// set infoArray back to base address of vector
	dyld_all_image_infos.infoArray = &sImageInfos[0];

	// tell gdb that about the new images
	dyld_all_image_infos.notification(dyld_image_removing, 1, &goingAway);
}


static void gdb_image_notifier(enum dyld_image_mode mode, uint32_t infoCount, const dyld_image_info info[])
{
	// do nothing
	// gdb sets a break point here to catch notifications
	//fprintf(stderr, "dyld: gdb_image_notifier(%s, %d, ...)\n", mode ? "dyld_image_removing" : "dyld_image_adding", infoCount);
	//for (uint32_t i=0; i < dyld_all_image_infos.infoArrayCount; ++i)
	//	fprintf(stderr, "dyld: %d loading at %p %s\n", i, dyld_all_image_infos.infoArray[i].imageLoadAddress, dyld_all_image_infos.infoArray[i].imageFilePath);
}



struct dyld_all_image_infos  dyld_all_image_infos = { 1, 0, NULL, &gdb_image_notifier, false };


 
