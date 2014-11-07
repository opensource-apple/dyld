/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*- 
 *
 * Copyright (c) 2009-2012 Apple Inc. All rights reserved.
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
#include <mach/mach.h>

#include <map>
#include <vector>

#include "dsc_iterator.h"
#include "dyld_cache_format.h"
#include "Architectures.hpp"
#include "MachOFileAbstraction.hpp"
#include "CacheFileAbstraction.hpp"

enum Mode {
	modeNone,
	modeList,
	modeMap,
	modeDependencies,
	modeSlideInfo,
	modeLinkEdit,
	modeInfo,
	modeSize
};

struct Options {
	Mode		mode;
	const char*	dependentsOfPath;
	const void*	mappedCache;
	bool		printUUIDs;
	bool		printVMAddrs;
    bool		printDylibVersions;
	bool		printInodes;
};

struct TextInfo {
	uint64_t		textSize;
	const char*		path;
};

struct TextInfoSorter {
	bool operator()(const TextInfo& left, const TextInfo& right) {
		return (left.textSize > right.textSize);
	}
};

struct Results {
	std::map<uint32_t, const char*>	pageToContent;
	uint64_t						linkeditBase;
	bool							dependentTargetFound;
	std::vector<TextInfo>			textSegments;
};



void usage() {
	fprintf(stderr, "Usage: dyld_shared_cache_util -list [ -uuid ] [-vmaddr] | -dependents <dylib-path> [ -versions ] | -linkedit | -map [ shared-cache-file ] | -slide_info | -info\n");
}

#if __x86_64__
static bool isHaswell()
{
	// check system is capable of running x86_64h code
	struct host_basic_info info;
	mach_msg_type_number_t count = HOST_BASIC_INFO_COUNT;
	mach_port_t hostPort = mach_host_self();
	kern_return_t result = host_info(hostPort, HOST_BASIC_INFO, (host_info_t)&info, &count);
	mach_port_deallocate(mach_task_self(), hostPort);
	if ( result != KERN_SUCCESS )
		return false;
	return ( info.cpu_subtype == CPU_SUBTYPE_X86_64_H );
}
#endif

/*
 * Get the path to the native shared cache for this host
 */
static const char* default_shared_cache_path() {
#if __i386__
	return MACOSX_DYLD_SHARED_CACHE_DIR DYLD_SHARED_CACHE_BASE_NAME "i386";
#elif __x86_64__ 
	if ( isHaswell() )
		return MACOSX_DYLD_SHARED_CACHE_DIR DYLD_SHARED_CACHE_BASE_NAME "x86_64h";
	else
		return MACOSX_DYLD_SHARED_CACHE_DIR DYLD_SHARED_CACHE_BASE_NAME "x86_64";
#elif __ARM_ARCH_5TEJ__ 
	return IPHONE_DYLD_SHARED_CACHE_DIR DYLD_SHARED_CACHE_BASE_NAME "armv5";
#elif __ARM_ARCH_6K__ 
	return IPHONE_DYLD_SHARED_CACHE_DIR DYLD_SHARED_CACHE_BASE_NAME "armv6";
#elif __ARM_ARCH_7K__ 
	return IPHONE_DYLD_SHARED_CACHE_DIR DYLD_SHARED_CACHE_BASE_NAME "armv7k";
#elif __ARM_ARCH_7A__ 
	return IPHONE_DYLD_SHARED_CACHE_DIR DYLD_SHARED_CACHE_BASE_NAME "armv7";
#elif __ARM_ARCH_7F__ 
	return IPHONE_DYLD_SHARED_CACHE_DIR DYLD_SHARED_CACHE_BASE_NAME "armv7f";
#elif __ARM_ARCH_7S__ 
	return IPHONE_DYLD_SHARED_CACHE_DIR DYLD_SHARED_CACHE_BASE_NAME "armv7s";
#elif __arm64__ 
	return IPHONE_DYLD_SHARED_CACHE_DIR DYLD_SHARED_CACHE_BASE_NAME "arm64";
#else
	#error unsupported architecture
#endif
}

typedef void (*segment_callback_t)(const dyld_shared_cache_dylib_info* dylibInfo, const dyld_shared_cache_segment_info* segInfo,
									const Options& options, Results& results);



/*
 * List dependencies from the mach-o header at headerAddr
 * in the same format as 'otool -L'
 */
template <typename A>
void print_dependencies(const dyld_shared_cache_dylib_info* dylibInfo, const dyld_shared_cache_segment_info* segInfo, 
																	const Options& options, Results& results) {
	typedef typename A::P		P;
	typedef typename A::P::E	E;
	
	if ( strcmp(options.dependentsOfPath, dylibInfo->path) != 0 ) 
		return;
	if ( strcmp(segInfo->name, "__TEXT") != 0 )
		return;

	const macho_dylib_command<P>* dylib_cmd;
	const macho_header<P>* mh = (const macho_header<P>*)dylibInfo->machHeader;
	const macho_load_command<P>* const cmds = (macho_load_command<P>*)((uintptr_t)dylibInfo->machHeader + sizeof(macho_header<P>));
	const uint32_t cmd_count = mh->ncmds();
	const macho_load_command<P>* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch ( cmd->cmd() ) {
			case LC_LOAD_DYLIB:
			case LC_ID_DYLIB:
			case LC_REEXPORT_DYLIB:
			case LC_LOAD_WEAK_DYLIB:
			case LC_LOAD_UPWARD_DYLIB:
				dylib_cmd = (macho_dylib_command<P>*)cmd;
				if ( options.printDylibVersions ) {
					uint32_t compat_vers = dylib_cmd->compatibility_version();
					uint32_t current_vers = dylib_cmd->current_version();
					printf("\t%s", dylib_cmd->name());
					if ( compat_vers != 0xFFFFFFFF ) {
						printf("(compatibility version %u.%u.%u, current version %u.%u.%u)\n", 
						   (compat_vers >> 16),
						   (compat_vers >> 8) & 0xff,
						   (compat_vers) & 0xff,
						   (current_vers >> 16),
						   (current_vers >> 8) & 0xff,
						   (current_vers) & 0xff);
					}
					else {
						printf("\n");
					}
				} 
				else {
					printf("\t%s\n", dylib_cmd->name());
				}
				break;
		}
		cmd = (const macho_load_command<P>*)(((uint8_t*)cmd)+cmd->cmdsize());
	}
	results.dependentTargetFound = true;
}

/*
 * Print out a dylib from the shared cache, optionally including the UUID or unslid load address
 */
template <typename A>
void print_list(const dyld_shared_cache_dylib_info* dylibInfo, const dyld_shared_cache_segment_info* segInfo, 
																		const Options& options, Results& results) 
{
	if ( strcmp(segInfo->name, "__TEXT") != 0 )
		return;

	if ( options.printVMAddrs )
		printf("0x%08llX ", segInfo->address);
	if ( options.printInodes )
		printf("0x%08llX 0x%08llX ", dylibInfo->inode, dylibInfo->modTime);
	if ( options.printUUIDs ) {
		if ( dylibInfo->uuid != NULL ) {
			const uint8_t* uuid = (uint8_t*)dylibInfo->uuid;;
			printf("<%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X> ",
    				uuid[0],  uuid[1],  uuid[2],  uuid[3],  
					uuid[4],  uuid[5],  uuid[6],  uuid[7], 
    				uuid[8],  uuid[9],  uuid[10], uuid[11], 
					uuid[12], uuid[13], uuid[14], uuid[15]);
		}
	    else
			printf("<     no uuid in dylib               > ");
    }
	if ( dylibInfo->isAlias )
		printf("[alias] %s\n", dylibInfo->path);
	else
		printf("%s\n", dylibInfo->path);
}


template <typename A>
void collect_size(const dyld_shared_cache_dylib_info* dylibInfo, const dyld_shared_cache_segment_info* segInfo, 
																		const Options& options, Results& results) 
{
	if ( strcmp(segInfo->name, "__TEXT") != 0 )
		return;
	if ( dylibInfo->isAlias )
		return;
		
	TextInfo info;
	info.textSize = segInfo->fileSize;
	info.path = dylibInfo->path;
	results.textSegments.push_back(info);
	size_t size = segInfo->fileSize;
}




static void add_linkedit(uint32_t pageStart, uint32_t pageEnd, const char* message, Results& results) 
{	
	for (uint32_t p = pageStart; p <= pageEnd; p += 4096) {
		std::map<uint32_t, const char*>::iterator pos = results.pageToContent.find(p);
		if ( pos == results.pageToContent.end() ) {
			results.pageToContent[p] = strdup(message);
		}
		else {
			const char* oldMessage = pos->second;
			char* newMesssage;
			asprintf(&newMesssage, "%s, %s", oldMessage, message);
			results.pageToContent[p] = newMesssage;
			::free((void*)oldMessage);
		}
	}
}


/*
 * get LINKEDIT info for dylib
 */
template <typename A>
void process_linkedit(const dyld_shared_cache_dylib_info* dylibInfo, const dyld_shared_cache_segment_info* segInfo, 
																			const Options& options, Results& results) {
	typedef typename A::P		P;
	typedef typename A::P::E	E;
	// filter out symlinks 
	if ( dylibInfo->isAlias )
		return;
	const macho_header<P>* mh = (const macho_header<P>*)dylibInfo->machHeader;
	uint32_t ncmds = mh->ncmds();
	const macho_load_command<P>* const cmds = (macho_load_command<P>*)((long)mh + sizeof(macho_header<P>));
	const macho_load_command<P>* cmd = cmds;
	for (uint32_t i = 0; i < ncmds; i++) {
		if ( cmd->cmd() == LC_DYLD_INFO_ONLY ) {
			macho_dyld_info_command<P>* dyldInfo = (macho_dyld_info_command<P>*)cmd;
			char message[1000];
			const char* shortName = strrchr(dylibInfo->path, '/') + 1;
			// add export trie info
			if ( dyldInfo->export_size() != 0 ) {
				//printf("export_off=0x%X\n", dyldInfo->export_off());
				uint32_t exportPageOffsetStart = dyldInfo->export_off() & (-4096);
				uint32_t exportPageOffsetEnd = (dyldInfo->export_off() + dyldInfo->export_size()) & (-4096);
				sprintf(message, "exports from %s", shortName);
				add_linkedit(exportPageOffsetStart, exportPageOffsetEnd, message, results);
			}
			// add binding info
			if ( dyldInfo->bind_size() != 0 ) {
				uint32_t bindPageOffsetStart = dyldInfo->bind_off() & (-4096);
				uint32_t bindPageOffsetEnd = (dyldInfo->bind_off() + dyldInfo->bind_size()) & (-4096);
				sprintf(message, "bindings from %s", shortName);
				add_linkedit(bindPageOffsetStart, bindPageOffsetEnd, message, results);
			}
			// add lazy binding info
			if ( dyldInfo->lazy_bind_size() != 0 ) {
				uint32_t lazybindPageOffsetStart = dyldInfo->lazy_bind_off() & (-4096);
				uint32_t lazybindPageOffsetEnd = (dyldInfo->lazy_bind_off() + dyldInfo->lazy_bind_size()) & (-4096);
				sprintf(message, "lazy bindings from %s", shortName);
				add_linkedit(lazybindPageOffsetStart, lazybindPageOffsetEnd, message, results);
			}
			// add weak binding info
			if ( dyldInfo->weak_bind_size() != 0 ) {
				uint32_t weakbindPageOffsetStart = dyldInfo->weak_bind_off() & (-4096);
				uint32_t weakbindPageOffsetEnd = (dyldInfo->weak_bind_off() + dyldInfo->weak_bind_size()) & (-4096);
				sprintf(message, "weak bindings from %s", shortName);
				add_linkedit(weakbindPageOffsetStart, weakbindPageOffsetEnd, message, results);
			}
		}
		cmd = (const macho_load_command<P>*)(((uint8_t*)cmd)+cmd->cmdsize());
	}
}


/*
 * Print out a .map file similar to what update_dyld_shared_cache created when the cache file was built
 */
template <typename A>
void print_map(const dyld_shared_cache_dylib_info* dylibInfo, const dyld_shared_cache_segment_info* segInfo, const Options& options, Results& results) {
	if ( !dylibInfo->isAlias )
		printf("0x%08llX - 0x%08llX %s %s\n", segInfo->address, segInfo->address + segInfo->fileSize, segInfo->name, dylibInfo->path);
}


static void checkMode(Mode mode) {
	if ( mode != modeNone ) {
		fprintf(stderr, "Error: select one of: -list, -dependents, -info, -slide_info, -linkedit, -map, or -size\n");
		usage();
		exit(1);
	}
}

int main (int argc, const char* argv[]) {

	const char* sharedCachePath = default_shared_cache_path();

 	Options options;
	options.mode = modeNone;
    options.printUUIDs = false;
	options.printVMAddrs = false;
    options.printDylibVersions = false;
	options.printInodes = false;
    options.dependentsOfPath = NULL;
    
    for (uint32_t i = 1; i < argc; i++) {
        const char* opt = argv[i];
        if (opt[0] == '-') {
            if (strcmp(opt, "-list") == 0) {
				checkMode(options.mode);
				options.mode = modeList;
            } 
			else if (strcmp(opt, "-dependents") == 0) {
				checkMode(options.mode);
				options.mode = modeDependencies;
				options.dependentsOfPath = argv[++i];
                if ( i >= argc ) {
                    fprintf(stderr, "Error: option -depdendents requires an argument\n");
                    usage();
                    exit(1);
                }
            } 
			else if (strcmp(opt, "-linkedit") == 0) {
				checkMode(options.mode);
				options.mode = modeLinkEdit;
			} 
			else if (strcmp(opt, "-info") == 0) {
				checkMode(options.mode);
				options.mode = modeInfo;
			}
			else if (strcmp(opt, "-slide_info") == 0) {
				checkMode(options.mode);
				options.mode = modeSlideInfo;
			} 
			else if (strcmp(opt, "-map") == 0) {
				checkMode(options.mode);
				options.mode = modeMap;
            } 
			else if (strcmp(opt, "-size") == 0) {
				checkMode(options.mode);
				options.mode = modeSize;
            } 
			else if (strcmp(opt, "-uuid") == 0) {
                options.printUUIDs = true;
            } 
			else if (strcmp(opt, "-inode") == 0) {
                options.printInodes = true;
            } 
			else if (strcmp(opt, "-versions") == 0) {
                options.printDylibVersions = true;
            } 
			else if (strcmp(opt, "-vmaddr") == 0) {
				options.printVMAddrs = true;
		    } 
			else {
                fprintf(stderr, "Error: unrecognized option %s\n", opt);
                usage();
                exit(1);
            }
        } 
		else {
            sharedCachePath = opt;
        }
    }
    
	if ( options.mode == modeNone ) {
		fprintf(stderr, "Error: select one of -list, -dependents, -info, -linkedit, or -map\n");
		usage();
		exit(1);
	}
		
	if ( options.mode != modeSlideInfo ) {
		if ( options.printUUIDs && (options.mode != modeList) )
			fprintf(stderr, "Warning: -uuid option ignored outside of -list mode\n");
			
		if ( options.printVMAddrs && (options.mode != modeList) )
			fprintf(stderr, "Warning: -vmaddr option ignored outside of -list mode\n");
			
		if ( options.printDylibVersions && (options.mode != modeDependencies) )
			fprintf(stderr, "Warning: -versions option ignored outside of -dependents mode\n");
		
		if ( (options.mode == modeDependencies) && (options.dependentsOfPath == NULL) ) {
			fprintf(stderr, "Error: -dependents given, but no dylib path specified\n");
			usage();
			exit(1);
		}
	}
			
	struct stat statbuf;
	if ( ::stat(sharedCachePath, &statbuf) == -1 ) {
		fprintf(stderr, "Error: stat() failed for dyld shared cache at %s, errno=%d\n", sharedCachePath, errno);
		exit(1);
	}
		
	int cache_fd = ::open(sharedCachePath, O_RDONLY);
	if ( cache_fd < 0 ) {
		fprintf(stderr, "Error: open() failed for shared cache file at %s, errno=%d\n", sharedCachePath, errno);
		exit(1);
	}
	options.mappedCache = ::mmap(NULL, statbuf.st_size, PROT_READ, MAP_PRIVATE, cache_fd, 0);
	if (options.mappedCache == MAP_FAILED) {
		fprintf(stderr, "Error: mmap() for shared cache at %s failed, errno=%d\n", sharedCachePath, errno);
		exit(1);
	}
	
	if ( options.mode == modeSlideInfo ) {
		const dyldCacheHeader<LittleEndian>* header = (dyldCacheHeader<LittleEndian>*)options.mappedCache;
		if ( header->slideInfoOffset() == 0 ) {
			fprintf(stderr, "Error: dyld shared cache does not contain slide info\n");
			exit(1);
		}
		const dyldCacheFileMapping<LittleEndian>* mappings = (dyldCacheFileMapping<LittleEndian>*)((char*)options.mappedCache + header->mappingOffset());
		const dyldCacheFileMapping<LittleEndian>* dataMapping = &mappings[1];
		uint64_t dataStartAddress = dataMapping->address();
		uint64_t dataSize = dataMapping->size();
		const dyldCacheSlideInfo<LittleEndian>* slideInfoHeader = (dyldCacheSlideInfo<LittleEndian>*)((char*)options.mappedCache+header->slideInfoOffset());
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
	else if ( options.mode == modeInfo ) {
		const dyldCacheHeader<LittleEndian>* header = (dyldCacheHeader<LittleEndian>*)options.mappedCache;
		printf("uuid: ");
		if ( header->mappingOffset() >= 0x68 ) {
			const uint8_t* uuid = header->uuid();
			printf("%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X\n",
				   uuid[0],  uuid[1],  uuid[2],  uuid[3],
				   uuid[4],  uuid[5],  uuid[6],  uuid[7],
				   uuid[8],  uuid[9],  uuid[10], uuid[11],
				   uuid[12], uuid[13], uuid[14], uuid[15]);
		}
		else {
			printf("n/a\n");
		}
		printf("image count: %u\n", header->imagesCount());
		printf("mappings:\n");
		const dyldCacheFileMapping<LittleEndian>* mappings = (dyldCacheFileMapping<LittleEndian>*)((char*)options.mappedCache + header->mappingOffset());
		for (uint32_t i=0; i < header->mappingCount(); ++i) {
			if ( mappings[i].init_prot() & VM_PROT_EXECUTE )
				printf("    __TEXT      %3lluMB,  0x%08llX -> 0x%08llX\n", mappings[i].size()/(1024*1024), mappings[i].address(), mappings[i].address() + mappings[i].size());
			else if ( mappings[i]. init_prot() & VM_PROT_WRITE )
				printf("    __DATA      %3lluMB,  0x%08llX -> 0x%08llX\n", mappings[i].size()/(1024*1024), mappings[i].address(), mappings[i].address() + mappings[i].size());
			else if ( mappings[i].init_prot() & VM_PROT_READ )
				printf("    __LINKEDIT  %3lluMB,  0x%08llX -> 0x%08llX\n", mappings[i].size()/(1024*1024), mappings[i].address(), mappings[i].address() + mappings[i].size());
		}
		if ( header->codeSignatureOffset() != 0 ) {
			uint64_t size = statbuf.st_size - header->codeSignatureOffset(); 
			uint64_t csAddr = mappings[header->mappingCount()-1].address() + mappings[header->mappingCount()-1].size();
				printf("    code sign   %3lluMB,  0x%08llX -> 0x%08llX\n", size/(1024*1024), csAddr, csAddr + size);
		}
	}
	else {
		segment_callback_t callback;
		if ( strcmp((char*)options.mappedCache, "dyld_v1    i386") == 0 ) {
			switch ( options.mode ) {
				case modeList:
					callback = print_list<x86>;
					break;
				case modeMap:
					callback = print_map<x86>;
					break;
				case modeDependencies:
					callback = print_dependencies<x86>;
					break;
				case modeLinkEdit:
					callback = process_linkedit<x86>;
					break;
				case modeSize:
					callback = collect_size<x86>;
					break;
				case modeNone:
				case modeInfo:
				case modeSlideInfo:
					break;
			}
		}		
		else if (  (strcmp((char*)options.mappedCache, "dyld_v1  x86_64") == 0)
				|| (strcmp((char*)options.mappedCache, "dyld_v1 x86_64h") == 0) ) {
			switch ( options.mode ) {
				case modeList:
					callback = print_list<x86_64>;
					break;
				case modeMap:
					callback = print_map<x86_64>;
					break;
				case modeDependencies:
					callback = print_dependencies<x86_64>;
					break;
				case modeLinkEdit:
					callback = process_linkedit<x86_64>;
					break;
				case modeSize:
					callback = collect_size<x86_64>;
					break;
				case modeNone:
				case modeInfo:
				case modeSlideInfo:
					break;
			}
		}		
		else if ( (strncmp((char*)options.mappedCache, "dyld_v1   armv", 14) == 0) 
			   || (strncmp((char*)options.mappedCache, "dyld_v1  armv", 13) == 0) ) {
			switch ( options.mode ) {
				case modeList:
					callback = print_list<arm>;
					break;
				case modeMap:
					callback = print_map<arm>;
					break;
				case modeDependencies:
					callback = print_dependencies<arm>;
					break;
				case modeLinkEdit:
					callback = process_linkedit<arm>;
					break;
				case modeSize:
					callback = collect_size<arm>;
					break;
				case modeNone:
				case modeInfo:
				case modeSlideInfo:
					break;
			}
		}		
		else if ( strcmp((char*)options.mappedCache, "dyld_v1   arm64") == 0 ) {
			switch ( options.mode ) {
				case modeList:
					callback = print_list<arm64>;
					break;
				case modeMap:
					callback = print_map<arm64>;
					break;
				case modeDependencies:
					callback = print_dependencies<arm64>;
					break;
				case modeLinkEdit:
					callback = process_linkedit<arm64>;
					break;
				case modeSize:
					callback = collect_size<arm64>;
					break;
				case modeNone:
				case modeInfo:
				case modeSlideInfo:
					break;
			}
		}		
 		else {
			fprintf(stderr, "Error: unrecognized dyld shared cache magic.\n");
			exit(1);
		}
		
		__block Results results;
		results.dependentTargetFound = false;
		int iterateResult = dyld_shared_cache_iterate(options.mappedCache, (uint32_t)statbuf.st_size, 
										   ^(const dyld_shared_cache_dylib_info* dylibInfo, const dyld_shared_cache_segment_info* segInfo ) {
											   (callback)(dylibInfo, segInfo, options, results);
										   });
		if ( iterateResult != 0 ) {
			fprintf(stderr, "Error: malformed shared cache file\n");
			exit(1);
		}
		
		if ( options.mode == modeLinkEdit ) {
			// dump -linkedit information
			for (std::map<uint32_t, const char*>::iterator it = results.pageToContent.begin(); it != results.pageToContent.end(); ++it) {
				printf("0x%08X %s\n", it->first, it->second);
			}
		}
		else if ( options.mode == modeSize ) {
			std::sort(results.textSegments.begin(), results.textSegments.end(), TextInfoSorter()); 
			for (std::vector<TextInfo>::iterator it = results.textSegments.begin(); it != results.textSegments.end(); ++it) {
				printf(" 0x%08llX  %s\n", it->textSize, it->path);
			}
		}
		
		if ( (options.mode == modeDependencies) && options.dependentsOfPath && !results.dependentTargetFound) {
			fprintf(stderr, "Error: could not find '%s' in the shared cache at\n  %s\n", options.dependentsOfPath, sharedCachePath);
			exit(1);
		}
	}
	return 0;
}
