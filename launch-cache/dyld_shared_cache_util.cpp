/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*- 
 *
 * Copyright (c) 2009-2010 Apple Inc. All rights reserved.
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

#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/syslimits.h>
#include <mach-o/arch.h>
#include <mach-o/loader.h>

#include <vector>
#include <map>
#include <set>

#include "dsc_iterator.h"
#include "dyld_cache_format.h"
#include "Architectures.hpp"
#include "MachOFileAbstraction.hpp"
#include "CacheFileAbstraction.hpp"


#define OP_NULL 0
#define OP_LIST_DEPENDENCIES 1
#define OP_LIST_DYLIBS 2
#define OP_LIST_LINKEDIT 3

#define UUID_BYTES 16

// Define this here so we can work with or without block support
typedef void (*segment_callback_t)(const char* dylib, const char* segName, uint64_t offset, uint64_t sizem, 
													uint64_t mappedddress, uint64_t slide, void* userData);

struct seg_callback_args {
	char *target_path;
	uint32_t target_found;
	void *mapped_cache;
	uint32_t op;
	uint8_t print_uuids;
	uint8_t print_vmaddrs;
    uint8_t print_dylib_versions;
};

void usage() {
	fprintf(stderr, "Usage: dscutil -list [ -uuid ] [-vmaddr] | -dependents <dylib-path> [ -versions ] | -linkedit [ shared-cache-file ]\n");
}

/*
 * Get the path to the native shared cache for this host
 */
static const char* default_shared_cache_path() {
#if __i386__
	return MACOSX_DYLD_SHARED_CACHE_DIR DYLD_SHARED_CACHE_BASE_NAME "i386";
#elif __x86_64__ 
	return MACOSX_DYLD_SHARED_CACHE_DIR DYLD_SHARED_CACHE_BASE_NAME "x86_64";
#elif __ARM_ARCH_5TEJ__ 
	return IPHONE_DYLD_SHARED_CACHE_DIR DYLD_SHARED_CACHE_BASE_NAME "armv5";
#elif __ARM_ARCH_6K__ 
	return IPHONE_DYLD_SHARED_CACHE_DIR DYLD_SHARED_CACHE_BASE_NAME "armv6";
#elif __ARM_ARCH_7A__ 
	return IPHONE_DYLD_SHARED_CACHE_DIR DYLD_SHARED_CACHE_BASE_NAME "armv7";
#elif __ARM_ARCH_7F__ 
	return IPHONE_DYLD_SHARED_CACHE_DIR DYLD_SHARED_CACHE_BASE_NAME "armv7f";
#elif __ARM_ARCH_7K__ 
	return IPHONE_DYLD_SHARED_CACHE_DIR DYLD_SHARED_CACHE_BASE_NAME "armv7k";
#else
	#error unsupported architecture
#endif
}

/*
 * Get a vector of all the load commands from the header pointed to by headerAddr
 */
template <typename A>
std::vector<macho_load_command<typename A::P>* > get_load_cmds(void *headerAddr) {
	typedef typename A::P		P;
	typedef typename A::P::E	E;
	
	std::vector<macho_load_command<P>* > cmd_vector;
	
	const macho_header<P>* mh = (const macho_header<P>*)headerAddr;
	uint32_t ncmds = mh->ncmds();
	const macho_load_command<P>* const cmds = (macho_load_command<P>*)((long)mh + sizeof(macho_header<P>));
	const macho_load_command<P>* cmd = cmds;
	
	for (uint32_t i = 0; i < ncmds; i++) {
		cmd_vector.push_back((macho_load_command<P>*)cmd);
		cmd = (const macho_load_command<P>*)(((uint8_t*)cmd)+cmd->cmdsize());
	}
	return cmd_vector;
}

/*
 * List dependencies from the mach-o header at headerAddr
 * in the same format as 'otool -L'
 */
template <typename A>
void list_dependencies(const char *dylib, void *headerAddr, uint8_t print_dylib_versions) {
	typedef typename A::P		P;
	typedef typename A::P::E	E;
	
	std::vector< macho_load_command<P>* > cmds;
	cmds = get_load_cmds<A>(headerAddr);
	for(typename std::vector<macho_load_command<P>*>::iterator it = cmds.begin(); it != cmds.end(); ++it) {
		uint32_t cmdType = (*it)->cmd();
		if (cmdType == LC_LOAD_DYLIB || 
			cmdType == LC_ID_DYLIB || 
			cmdType == LC_LOAD_WEAK_DYLIB ||
			cmdType == LC_REEXPORT_DYLIB ||
			cmdType == LC_LOAD_UPWARD_DYLIB) {
			macho_dylib_command<P>* dylib_cmd = (macho_dylib_command<P>*)*it;
			const char *name = dylib_cmd->name();
			uint32_t compat_vers = dylib_cmd->compatibility_version();
			uint32_t current_vers = dylib_cmd->current_version();
			
            if (print_dylib_versions) {
                printf("\t%s", name);
                if ( compat_vers != 0xFFFFFFFF )
                    printf("(compatibility version %u.%u.%u, current version %u.%u.%u)\n", 
                       (compat_vers >> 16),
                       (compat_vers >> 8) & 0xff,
                       (compat_vers) & 0xff,
                       (current_vers >> 16),
                       (current_vers >> 8) & 0xff,
                       (current_vers) & 0xff);
                else
                    printf("\n");
            } else {
                printf("\t%s\n", name);
            }			
		}
	}
}

/*
 * Print out a dylib from the shared cache, optionally including the UUID
 */
template <typename A>
void print_dylib(const char *dylib, void *headerAddr, uint64_t slide, struct seg_callback_args *args) {
	typedef typename A::P		P;
	typedef typename A::P::E	E;
	char uuid_str[UUID_BYTES*3];
	uint8_t got_uuid = 0;
	uint64_t vmaddr = 0;
		
	std::vector< macho_load_command<P>* > cmds;
	cmds = get_load_cmds<A>(headerAddr);
	for(typename std::vector<macho_load_command<P>*>::iterator it = cmds.begin(); it != cmds.end(); ++it) {
		uint32_t cmdType = (*it)->cmd();
		if (cmdType == LC_UUID) {
			macho_uuid_command<P>* uuid_cmd = (macho_uuid_command<P>*)*it;
			const uint8_t *uuid = uuid_cmd->uuid();
			sprintf(uuid_str, "<%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X>",
    				uuid[0], uuid[1],  uuid[2],  uuid[3],  uuid[4],  uuid[5],  uuid[6], uuid[7], 
    				uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
			got_uuid = 1;
		} 
		else if (cmdType == LC_SEGMENT) {
			macho_segment_command<P>* seg_cmd = (macho_segment_command<P>*)*it;
			if (strcmp(seg_cmd->segname(), "__TEXT") == 0) {
				vmaddr = seg_cmd->vmaddr();
			}
		}			
	}

	if (args->print_vmaddrs)
		printf("0x%08llX ", vmaddr+slide);
	if (args->print_uuids) {
		if (got_uuid)
			printf("%s ", uuid_str);
	    else
			printf("<     no uuid in dylib               > ");
    }
	printf("%s\n", dylib);
}


uint64_t					sLinkeditBase = 0;
std::map<uint32_t, char*>	sPageToContent;




static void add_linkedit(uint32_t pageStart, uint32_t pageEnd, char* message) 
{	
	for (uint32_t p = pageStart; p <= pageEnd; p += 4096) {
		std::map<uint32_t, char*>::iterator pos = sPageToContent.find(p);
		if ( pos == sPageToContent.end() ) {
			sPageToContent[p] = strdup(message);
		}
		else {
			char* oldMessage = pos->second;
			char* newMesssage;
			asprintf(&newMesssage, "%s, %s", oldMessage, message);
			sPageToContent[p] = newMesssage;
			free(oldMessage);
		}
	}
}


/*
 * get LINKEDIT info for dylib
 */
template <typename A>
void process_linkedit(const char* dylib, void* headerAddr) {	
	typedef typename A::P		P;
	typedef typename A::P::E	E;
	// filter out symlinks by only handling first path found for each mach header
	static std::set<void*> seenImages;
	if ( seenImages.count(headerAddr) != 0 )
		return;
	seenImages.insert(headerAddr);
	const macho_header<P>* mh = (const macho_header<P>*)headerAddr;
	uint32_t ncmds = mh->ncmds();
	const macho_load_command<P>* const cmds = (macho_load_command<P>*)((long)mh + sizeof(macho_header<P>));
	const macho_load_command<P>* cmd = cmds;
	for (uint32_t i = 0; i < ncmds; i++) {
		if ( cmd->cmd() == LC_DYLD_INFO_ONLY ) {
			macho_dyld_info_command<P>* dyldInfo = (macho_dyld_info_command<P>*)cmd;
			char message[1000];
			const char* shortName = strrchr(dylib, '/') + 1;
			// add export trie info
			if ( dyldInfo->export_size() != 0 ) {
				//printf("export_off=0x%X\n", dyldInfo->export_off());
				uint32_t exportPageOffsetStart = dyldInfo->export_off() & (-4096);
				uint32_t exportPageOffsetEnd = (dyldInfo->export_off() + dyldInfo->export_size()) & (-4096);
				sprintf(message, "exports from %s", shortName);
				add_linkedit(exportPageOffsetStart, exportPageOffsetEnd, message);
			}
			// add binding info
			if ( dyldInfo->bind_size() != 0 ) {
				uint32_t bindPageOffsetStart = dyldInfo->bind_off() & (-4096);
				uint32_t bindPageOffsetEnd = (dyldInfo->bind_off() + dyldInfo->bind_size()) & (-4096);
				sprintf(message, "bindings from %s", shortName);
				add_linkedit(bindPageOffsetStart, bindPageOffsetEnd, message);
			}
			// add lazy binding info
			if ( dyldInfo->lazy_bind_size() != 0 ) {
				uint32_t lazybindPageOffsetStart = dyldInfo->lazy_bind_off() & (-4096);
				uint32_t lazybindPageOffsetEnd = (dyldInfo->lazy_bind_off() + dyldInfo->lazy_bind_size()) & (-4096);
				sprintf(message, "lazy bindings from %s", shortName);
				add_linkedit(lazybindPageOffsetStart, lazybindPageOffsetEnd, message);
			}
			// add weak binding info
			if ( dyldInfo->weak_bind_size() != 0 ) {
				uint32_t weakbindPageOffsetStart = dyldInfo->weak_bind_off() & (-4096);
				uint32_t weakbindPageOffsetEnd = (dyldInfo->weak_bind_off() + dyldInfo->weak_bind_size()) & (-4096);
				sprintf(message, "weak bindings from %s", shortName);
				add_linkedit(weakbindPageOffsetStart, weakbindPageOffsetEnd, message);
			}
		}
		cmd = (const macho_load_command<P>*)(((uint8_t*)cmd)+cmd->cmdsize());
	}
}



/*
 * This callback is used with dsc_iterator, and called once for each segment in the target shared cache
 */
template <typename A>
void segment_callback(const char *dylib, const char *segName, uint64_t offset, uint64_t sizem, 
										uint64_t mappedAddress, uint64_t slide, void *userData) {
	typedef typename A::P    P;
	typedef typename A::P::E E;
	struct seg_callback_args *args = (struct seg_callback_args *)userData;
	if (strncmp(segName, "__TEXT", 6) == 0) {
		int target_match = args->target_path ? (strcmp(args->target_path, dylib) == 0) : 0;
		if (!args->target_path || target_match) {
			if (target_match) {
				args->target_found = 1;
			}
			void *headerAddr = (void*)((long)args->mapped_cache + (long)offset);
			switch (args->op) {
				case OP_LIST_DEPENDENCIES:
					list_dependencies<A>(dylib, headerAddr, args->print_dylib_versions);
					break;
				case OP_LIST_DYLIBS:
					print_dylib<A>(dylib, headerAddr, slide, args);
					break;
				case OP_LIST_LINKEDIT:
					process_linkedit<A>(dylib, headerAddr);
				default:
					break;
			}
		}
	}
	else if (strncmp(segName, "__LINKEDIT", 6) == 0) {
		sLinkeditBase = mappedAddress - offset;
	}
}




int main (int argc, char **argv) {
	struct seg_callback_args args;
	const char *shared_cache_path = NULL;
	void *mapped_cache;
	struct stat statbuf;
	int cache_fd;
	char c;
	bool print_slide_info = false;

    args.target_path = NULL;
	args.op = OP_NULL;
    args.print_uuids = 0;
	args.print_vmaddrs = 0;
    args.print_dylib_versions = 0;
    args.target_found = 0;
    
    for (uint32_t optind = 1; optind < argc; optind++) {
        char *opt = argv[optind];
        if (opt[0] == '-') {
            if (strcmp(opt, "-list") == 0) {
                if (args.op) {
                    fprintf(stderr, "Error: select one of -list or -dependents\n");
                    usage();
                    exit(1);
                }
                args.op = OP_LIST_DYLIBS;
            } else if (strcmp(opt, "-dependents") == 0) {
                if (args.op) {
                    fprintf(stderr, "Error: select one of -list or -dependents\n");
                    usage();
                    exit(1);
                }
                if (!(++optind < argc)) {
                    fprintf(stderr, "Error: option -depdendents requires an argument\n");
                    usage();
                    exit(1);
                }
                args.op = OP_LIST_DEPENDENCIES;
                args.target_path = argv[optind];
            } else if (strcmp(opt, "-uuid") == 0) {
                args.print_uuids = 1;
            } else if (strcmp(opt, "-versions") == 0) {
                args.print_dylib_versions = 1;
            } else if (strcmp(opt, "-vmaddr") == 0) {
				args.print_vmaddrs = 1;
		    } else if (strcmp(opt, "-linkedit") == 0) {
                args.op = OP_LIST_LINKEDIT;
			} else if (strcmp(opt, "-slide_info") == 0) {
				print_slide_info = true;
            } else {
                fprintf(stderr, "Error: unrecognized option %s\n", opt);
                usage();
                exit(1);
            }
        } else {
            shared_cache_path = opt;
        }
    }
    
	if ( !print_slide_info ) {
		if (args.op == OP_NULL) {
			fprintf(stderr, "Error: select one of -list or -dependents\n");
			usage();
			exit(1);
		}
		
		if (args.print_uuids && args.op != OP_LIST_DYLIBS)
			fprintf(stderr, "Warning: -uuid option ignored outside of -list mode\n");
		if (args.print_vmaddrs && args.op != OP_LIST_DYLIBS)
			fprintf(stderr, "Warning: -vmaddr option ignored outside of -list mode\n");
		if (args.print_dylib_versions && args.op != OP_LIST_DEPENDENCIES)
			fprintf(stderr, "Warning: -versions option ignored outside of -dependents mode\n");
		
		if (args.op == OP_LIST_DEPENDENCIES && !args.target_path) {
			fprintf(stderr, "Error: -dependents given, but no dylib path specified\n");
			usage();
			exit(1);
		}
	}
	
	if (!shared_cache_path)
		shared_cache_path = default_shared_cache_path();
		
	if (stat(shared_cache_path, &statbuf)) {
		fprintf(stderr, "Error: stat failed for dyld shared cache at %s\n", shared_cache_path);
		exit(1);
	}
		
	cache_fd = open(shared_cache_path, O_RDONLY);
	if (cache_fd < 0) {
		fprintf(stderr, "Error: failed to open shared cache file at %s\n", shared_cache_path);
		exit(1);
	}
	mapped_cache = mmap(NULL, statbuf.st_size, PROT_READ, MAP_PRIVATE, cache_fd, 0);
	if (mapped_cache == MAP_FAILED) {
		fprintf(stderr, "Error: mmap() for shared cache at %s failed, errno=%d\n", shared_cache_path, errno);
		exit(1);
	}
	
	if ( print_slide_info ) {
		const dyldCacheHeader<LittleEndian>* header = (dyldCacheHeader<LittleEndian>*)mapped_cache;
		if (   (strcmp(header->magic(), "dyld_v1  x86_64") != 0)
			&& (strcmp(header->magic(), "dyld_v1   armv6") != 0)
			&& (strcmp(header->magic(), "dyld_v1   armv7") != 0) 
			&& (strcmp(header->magic(), "dyld_v1  armv7f") != 0) 
			&& (strcmp(header->magic(), "dyld_v1  armv7k") != 0) ) {
			fprintf(stderr, "Error: unrecognized dyld shared cache magic or arch does not support sliding\n");
			exit(1);
		}
		if ( header->slideInfoOffset() == 0 ) {
			fprintf(stderr, "Error: dyld shared cache does not contain slide info\n");
			exit(1);
		}
		const dyldCacheFileMapping<LittleEndian>* mappings = (dyldCacheFileMapping<LittleEndian>*)((char*)mapped_cache + header->mappingOffset());
		const dyldCacheFileMapping<LittleEndian>* dataMapping = &mappings[1];
		uint64_t dataStartAddress = dataMapping->address();
		uint64_t dataSize = dataMapping->size();
		const dyldCacheSlideInfo<LittleEndian>* slideInfoHeader = (dyldCacheSlideInfo<LittleEndian>*)((char*)mapped_cache+header->slideInfoOffset());
		printf("slide info version=%d\n", slideInfoHeader->version());
		printf("toc_count=%d, data page count=%lld\n", slideInfoHeader->toc_count(), dataSize/4096);
		const dyldCacheSlideInfoEntry* entries = (dyldCacheSlideInfoEntry*)((char*)slideInfoHeader + slideInfoHeader->entries_offset());
		for(int i=0; i < slideInfoHeader->toc_count(); ++i) {
			printf("0x%08llX: [% 5d,% 5d] ", dataStartAddress + i*4096, i, slideInfoHeader->toc(i));
			const dyldCacheSlideInfoEntry* entry = &entries[slideInfoHeader->toc(i)];
			for(int j=0; j < slideInfoHeader->entries_size(); ++j)
				printf("%02X", entry->bits[j]);
			printf("\n");
		}
		

	}
	else {
		segment_callback_t callback;
		if ( strcmp((char*)mapped_cache, "dyld_v1    i386") == 0 ) 
			callback = segment_callback<x86>;
		else if ( strcmp((char*)mapped_cache, "dyld_v1  x86_64") == 0 ) 
			callback = segment_callback<x86_64>;
		else if ( strcmp((char*)mapped_cache, "dyld_v1   armv5") == 0 ) 
			callback = segment_callback<arm>;
		else if ( strcmp((char*)mapped_cache, "dyld_v1   armv6") == 0 ) 
			callback = segment_callback<arm>;
		else if ( strcmp((char*)mapped_cache, "dyld_v1   armv7") == 0 ) 
			callback = segment_callback<arm>;
		else if ( strcmp((char*)mapped_cache, "dyld_v1  armv7f") == 0 ) 
			callback = segment_callback<arm>;
		else if ( strcmp((char*)mapped_cache, "dyld_v1  armv7k") == 0 ) 
			callback = segment_callback<arm>;
		else {
			fprintf(stderr, "Error: unrecognized dyld shared cache magic.\n");
			exit(1);
		}
		
		args.mapped_cache = mapped_cache;
		
	#if __BLOCKS__
		// Shim to allow building for the host
		void *argsPtr = &args;
		dyld_shared_cache_iterate_segments_with_slide(mapped_cache, 
										   ^(const char* dylib, const char* segName, uint64_t offset, uint64_t size, uint64_t mappedddress, uint64_t slide ) {
											   (callback)(dylib, segName, offset, size, mappedddress, slide, argsPtr);
										   });
	#else
		dyld_shared_cache_iterate_segments_with_slide_nb(mapped_cache, callback, &args);
	#endif
		
		if (args.op == OP_LIST_LINKEDIT) {
			// dump -linkedit information
			for (std::map<uint32_t, char*>::iterator it = sPageToContent.begin(); it != sPageToContent.end(); ++it) {
				printf("0x%0llX %s\n", sLinkeditBase+it->first, it->second);
			}
		}
		
		
		if (args.target_path && !args.target_found) {
			fprintf(stderr, "Error: could not find '%s' in the shared cache at\n  %s\n", args.target_path, shared_cache_path);
			exit(1);
		}
	}
	return 0;
}
