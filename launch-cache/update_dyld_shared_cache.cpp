/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*- 
 *
 * Copyright (c) 2006-2011 Apple Inc. All rights reserved.
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

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <signal.h>
#include <errno.h>
#include <sys/uio.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/resource.h>
#include <dirent.h>
#include <servers/bootstrap.h>
#include <mach-o/loader.h>
#include <mach-o/fat.h>
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <Security/SecCodeSigner.h>
#include <CommonCrypto/CommonDigest.h>

#include "dyld_cache_format.h"

#include <vector>
#include <set>
#include <map>
#include <unordered_map>

#include "Architectures.hpp"
#include "MachOLayout.hpp"
#include "MachORebaser.hpp"
#include "MachOBinder.hpp"
#include "CacheFileAbstraction.hpp"
#include "dyld_cache_config.h"

#define SELOPT_WRITE
#include "objc-shared-cache.h"

#define FIRST_DYLIB_TEXT_OFFSET 0x8000

#ifndef LC_FUNCTION_STARTS
    #define LC_FUNCTION_STARTS 0x26
#endif

static bool							verbose = false;
static bool							progress = false;
static bool							iPhoneOS = false;
static std::vector<const char*>		warnings;


static void warn(const char *arch, const char *format, ...)
{
    char *msg;

    va_list args;
    va_start(args, format);
    ::vasprintf(&msg, format, args);
    va_end(args);
    
    warnings.push_back(msg);
    
    if ( verbose ) {
        ::fprintf(::stderr, "update_dyld_shared_cache: warning: %s%s%s%s\n", 
                  arch ? "for arch " : "", 
                  arch ? arch : "", 
                  arch ? ", " : "", 
                  msg);
    }
}


class CStringHash {
public:
	size_t operator()(const char* __s) const {
		size_t __h = 0;
		for ( ; *__s; ++__s)
			__h = 5 * __h + *__s;
		return __h;
	};
};
class CStringEquals
{
public:
	bool operator()(const char* left, const char* right) const { return (strcmp(left, right) == 0); }
};



class ArchGraph
{
public:
	typedef std::unordered_map<const char*, const char*, CStringHash, CStringEquals> StringToString;

	static void			addArchPair(ArchPair ap);
	static void			addRoot(const char* vpath, const std::set<ArchPair>& archs);
	static void			findSharedDylibs(ArchPair ap);
	static ArchGraph*	graphForArchPair(ArchPair ap) { return fgPerArchGraph[ap]; }
	static void			setFileSystemRoot(const char* root) { fgFileSystemRoot = root; }
	static void			setFileSystemOverlay(const char* overlay) { fgFileSystemOverlay = overlay; }
	static const char*	archName(ArchPair ap);
	
	ArchPair											getArchPair() { return fArchPair; }
	std::set<const class MachOLayoutAbstraction*>&		getSharedDylibs() { return fSharedDylibs; }
	StringToString&										getDylibAliases() { return fAliasesMap; }
	const char*											archName() { return archName(fArchPair); }
	
private:
	
	class DependencyNode
	{
	public:
										DependencyNode(ArchGraph*, const char* path, const MachOLayoutAbstraction* layout);
		void							loadDependencies(const MachOLayoutAbstraction*);
		void							markNeededByRoot(DependencyNode*);
		const char*						getPath() const { return fPath; }
		const MachOLayoutAbstraction*	getLayout() const { return fLayout; }
		size_t							useCount() const { return fRootsDependentOnThis.size(); }
		bool							allDependentsFound() const { return !fDependentMissing; }
	private:
		ArchGraph*									fGraph;
		const char*									fPath;
		const MachOLayoutAbstraction*				fLayout;
		bool										fDependenciesLoaded;
		bool										fDependentMissing;
		std::set<DependencyNode*>					fDependsOn;
		std::set<DependencyNode*>					fRootsDependentOnThis;
	};

	typedef std::unordered_map<const char*, class DependencyNode*, CStringHash, CStringEquals> PathToNode;


								ArchGraph(ArchPair ap) : fArchPair(ap) {}
	void						addRoot(const char* path, const MachOLayoutAbstraction*);
	DependencyNode*				getNode(const char* path);
	DependencyNode*				getNodeForVirtualPath(const char* vpath);
	static bool					canBeShared(const MachOLayoutAbstraction* layout, ArchPair ap, const std::set<const MachOLayoutAbstraction*>& possibleLibs, std::map<const MachOLayoutAbstraction*, bool>& shareableMap);
	static bool					sharable(const MachOLayoutAbstraction* layout, ArchPair ap, char** msg);

	static std::map<ArchPair, ArchGraph*>	fgPerArchGraph;
	static const char*						fgFileSystemRoot;
	static const char*						fgFileSystemOverlay;
	
	ArchPair									fArchPair;
	std::set<DependencyNode*>					fRoots;
	PathToNode									fNodes;
	std::set<const MachOLayoutAbstraction*>		fSharedDylibs;  // use set to avoid duplicates when installname!=realpath
	StringToString								fAliasesMap;
};
std::map<ArchPair, ArchGraph*>		ArchGraph::fgPerArchGraph;
const char*							ArchGraph::fgFileSystemRoot = "";
const char*							ArchGraph::fgFileSystemOverlay = "";

void ArchGraph::addArchPair(ArchPair ap)
{
	//fprintf(stderr, "adding ArchPair 0x%08X,0x%08X\n", ap.arch, ap.subtype);
	fgPerArchGraph[ap] = new ArchGraph(ap);
}

void ArchGraph::addRoot(const char* vpath, const std::set<ArchPair>& onlyArchs)
{
	//fprintf(stderr, "addRoot(%s)\n", vpath);
	char completePath[MAXPATHLEN];
	const char* path = NULL;
	// check -overlay path first
	if ( fgFileSystemOverlay[0] != '\0' ) {
		strcpy(completePath, fgFileSystemOverlay);
		strcat(completePath, vpath);	// assumes vpath starts with '/'
		struct stat stat_buf;
		if ( stat(completePath, &stat_buf) == 0 )
			path = completePath;
	}
	// if not found in overlay, check for -root
	if ( (path == NULL) && (fgFileSystemRoot[0] != '\0') ) {
		strcpy(completePath, fgFileSystemRoot);
		strcat(completePath, vpath);	// assumes vpath starts with '/'
		struct stat stat_buf;
		if ( stat(completePath, &stat_buf) == 0 )
			path = completePath;
	}
	if ( path == NULL ) 
		path = vpath;
	
	try {
		//fprintf(stderr, "    UniversalMachOLayout::find(%s)\n", path);
		const UniversalMachOLayout& uni = UniversalMachOLayout::find(path, &onlyArchs);
		for(std::set<ArchPair>::iterator ait = onlyArchs.begin(); ait != onlyArchs.end(); ++ait) {
			try {
				const MachOLayoutAbstraction* layout = uni.getSlice(*ait);
				if ( layout != NULL )
					fgPerArchGraph[*ait]->addRoot(path, layout);
			}
			catch (const char* msg) {
				if ( verbose ) 
					fprintf(stderr, "update_dyld_shared_cache: warning for %s can't use root '%s': %s\n", fgPerArchGraph[*ait]->archName(), path, msg);
			}
			
		}
	}
	catch (const char* msg) {
		fprintf(stderr, "update_dyld_shared_cache: warning can't use root '%s': %s\n", path, msg);
	}
}



void ArchGraph::addRoot(const char* path, const MachOLayoutAbstraction* layout)
{
	if ( verbose )
		fprintf(stderr, "update_dyld_shared_cache: adding root: %s\n", path);
	DependencyNode*	node = this->getNode(path);
	fRoots.insert(node);
	const MachOLayoutAbstraction* mainExecutableLayout = NULL;
	if ( layout->getFileType() == MH_EXECUTE )
		mainExecutableLayout = layout;
	node->loadDependencies(mainExecutableLayout);
	node->markNeededByRoot(node);
	if ( layout->getFileType() == MH_DYLIB )
		node->markNeededByRoot(NULL);
}

// a virtual path does not have the fgFileSystemRoot prefix
ArchGraph::DependencyNode* ArchGraph::getNodeForVirtualPath(const char* vpath)
{
	//fprintf(stderr, "getNodeForVirtualPath(%s)\n", vpath);
	char completePath[MAXPATHLEN];
	if ( fgFileSystemOverlay[0] != '\0' ) {
		// using -overlay means if /overlay/path/dylib exists use it, otherwise use /path/dylib
		strcpy(completePath, fgFileSystemOverlay);
		strcat(completePath, vpath);	// assumes vpath starts with '/'
		struct stat stat_buf;
		if ( stat(completePath, &stat_buf) == 0 )
			return this->getNode(completePath);
		else {
			// <rdar://problem/9279770> support when install name is a symlink
			const char* pathToSymlink = vpath;
			if ( fgFileSystemRoot[0] != '\0' ) {
				strcpy(completePath, fgFileSystemRoot);
				strcat(completePath, vpath);
				pathToSymlink = completePath;
			}
			if ( (lstat(pathToSymlink, &stat_buf) == 0) && S_ISLNK(stat_buf.st_mode) ) {
				// requested path did not exist in /overlay, but leaf of path is a symlink in /
				char pathInSymLink[MAXPATHLEN];
				size_t res = readlink(pathToSymlink, pathInSymLink, sizeof(pathInSymLink));
				if ( res != -1 ) {
					pathInSymLink[res] = '\0';
					if ( pathInSymLink[0] != '/' ) {
						char symFullPath[MAXPATHLEN];
						strcpy(symFullPath, vpath);
						char* lastSlash = strrchr(symFullPath, '/');
						if ( lastSlash != NULL ) {
							strcpy(lastSlash+1, pathInSymLink);
							// (re)try looking for what symlink points to, but in /overlay
							return this->getNodeForVirtualPath(symFullPath);
						}
					} 
				}
			}
		}
	}
	if ( fgFileSystemRoot[0] != '\0' ) {
		// using -root means always use /rootpath/usr/lib
		strcpy(completePath, fgFileSystemRoot);
		strcat(completePath, vpath);	// assumes vpath starts with '/'
		return this->getNode(completePath);
	}
	// not found in -overlay or -root not used
	return this->getNode(vpath);
}

ArchGraph::DependencyNode* ArchGraph::getNode(const char* path)
{
	//fprintf(stderr, "getNode(%s)\n", path);
	// look up supplied path to see if node already exists
	PathToNode::iterator pos = fNodes.find(path);
	if ( pos != fNodes.end() )
		return pos->second;
	
	// get real path
	char realPath[MAXPATHLEN];
	if ( realpath(path, realPath) == NULL )
		throwf("realpath() failed on %s\n", path);
	
	// look up real path to see if node already exists
	pos = fNodes.find(realPath);
	if ( pos != fNodes.end() ) {
		// update fAliasesMap with symlinks found
		const char* aliasPath = path;
		if ( (fgFileSystemRoot != NULL) && (strncmp(path, fgFileSystemRoot, strlen(fgFileSystemRoot)) == 0) ) {
			aliasPath = &path[strlen(fgFileSystemRoot)];
		}
		if ( fAliasesMap.find(aliasPath) == fAliasesMap.end() ) {
			if ( strcmp(aliasPath, pos->second->getLayout()->getID().name) != 0 ) {
				fAliasesMap[strdup(aliasPath)] = pos->second->getLayout()->getID().name;
				//fprintf(stderr, "getNode() %s: added alias %s -> %s\n", archName(fArchPair), aliasPath, fAliasesMap[aliasPath]);
			}
		}
		return pos->second;
	}
	
	// still does not exist, so create a new node
	const UniversalMachOLayout& uni = UniversalMachOLayout::find(realPath);
	DependencyNode* node = new DependencyNode(this, realPath, uni.getSlice(fArchPair));
	if ( node->getLayout() == NULL ) {
		throwf("%s is missing arch %s", realPath, archName(fArchPair));
	}
	// add realpath to node map
	fNodes[node->getPath()] = node;
	// if install name is not real path, add install name to node map
	if ( (node->getLayout()->getFileType() == MH_DYLIB) && (strcmp(realPath, node->getLayout()->getID().name) != 0) ) {
		//fprintf(stderr, "adding %s node alias %s for %s\n", archName(fArchPair), node->getLayout()->getID().name, realPath);
		pos = fNodes.find(node->getLayout()->getID().name);
		if ( pos != fNodes.end() ) {
			// get uuids of two dylibs to see if this is accidental copy of a dylib or two differnent dylibs with same -install_name
			uuid_t uuid1;
			uuid_t uuid2;
			node->getLayout()->uuid(uuid1);
			pos->second->getLayout()->uuid(uuid2);
			if ( memcmp(&uuid1, &uuid2, 16) == 0 ) {
				// <rdar://problem/8305479> warn if two dylib in cache have same install_name
				char* msg;
				asprintf(&msg, "update_dyld_shared_cache: warning, found two copies of the same dylib with same install path: %s\n\t%s\n\t%s\n", 
										node->getLayout()->getID().name, pos->second->getPath(), node->getPath());
				fprintf(stderr, "%s", msg);
				warnings.push_back(msg);
			}
			else {
				// <rdar://problem/12763450> update_dyld_shared_cache should fail if two images have same install name
				fprintf(stderr, "update_dyld_shared_cache: found two different dylibs with same install path: %s\n\t%s\n\t%s\n", 
							node->getLayout()->getID().name, pos->second->getPath(), node->getPath());
				exit(1);
			}
		}
		else
			fNodes[node->getLayout()->getID().name] = node;
		// update fAliasesMap with symlinks found
		const char* aliasPath = realPath;
		if ( (fgFileSystemRoot != NULL) && (fgFileSystemRoot[0] != '\0') && (strncmp(realPath, fgFileSystemRoot, strlen(fgFileSystemRoot)) == 0) ) {
			aliasPath = &realPath[strlen(fgFileSystemRoot)];
		}
		// <rdar://problem/11192810> Too many aliases in -overlay mode
		if ( (fgFileSystemOverlay != NULL) && (fgFileSystemOverlay[0] != '\0') && (strncmp(realPath, fgFileSystemOverlay, strlen(fgFileSystemOverlay)) == 0) ) {
			aliasPath = &realPath[strlen(fgFileSystemOverlay)];
		}
		if ( fAliasesMap.find(aliasPath) == fAliasesMap.end() ) {
			if ( strcmp(aliasPath, node->getLayout()->getID().name) != 0 ) {
				fAliasesMap[strdup(aliasPath)] = node->getLayout()->getID().name;
				//fprintf(stderr, "getNode() %s: added alias %s -> %s\n", archName(fArchPair), aliasPath, fAliasesMap[aliasPath]);
			}
		}
	}
	return node;
}
	
	
void ArchGraph::DependencyNode::loadDependencies(const MachOLayoutAbstraction* mainExecutableLayout)
{
	if ( !fDependenciesLoaded ) {
		fDependenciesLoaded = true;
		// add dependencies
		const std::vector<MachOLayoutAbstraction::Library>&	dependsOn = fLayout->getLibraries();
		for(std::vector<MachOLayoutAbstraction::Library>::const_iterator it = dependsOn.begin(); it != dependsOn.end(); ++it) {
			try {
				const char* dependentPath = it->name;
				if ( strncmp(dependentPath, "@executable_path/", 17) == 0 ) {
					if ( mainExecutableLayout == NULL )
						throw "@executable_path without main executable";
					// expand @executable_path path prefix
					const char* executablePath = mainExecutableLayout->getFilePath();
					char newPath[strlen(executablePath) + strlen(dependentPath)+2];
					if ( (fgFileSystemRoot != NULL) && (strncmp(executablePath, fgFileSystemRoot, strlen(fgFileSystemRoot)) == 0) ) {
						// executablePath already has rootPath prefix, need to remove that to get to base virtual path
						strcpy(newPath, &executablePath[strlen(fgFileSystemRoot)]);
					}
					else {
						strcpy(newPath, executablePath);
					}
					char* addPoint = strrchr(newPath,'/');
					if ( addPoint != NULL )
						strcpy(&addPoint[1], &dependentPath[17]);
					else
						strcpy(newPath, &dependentPath[17]);
					dependentPath = strdup(newPath);
				}
				else if ( strncmp(dependentPath, "@loader_path/", 13) == 0 ) {
					// expand @loader_path path prefix
					char newPath[strlen(fPath) + strlen(dependentPath)+2];
					if ( (fgFileSystemRoot != NULL) && (strncmp(fPath, fgFileSystemRoot, strlen(fgFileSystemRoot)) == 0) ) {
						// fPath already has rootPath prefix, need to remove that to get to base virtual path
						strcpy(newPath, &fPath[strlen(fgFileSystemRoot)]);
					}
					else {
						strcpy(newPath, fPath);
					}
					char* addPoint = strrchr(newPath,'/');
					if ( addPoint != NULL )
						strcpy(&addPoint[1], &dependentPath[13]);
					else
						strcpy(newPath, &dependentPath[13]);
					dependentPath = strdup(newPath);
				}
				else if ( strncmp(dependentPath, "@rpath/", 7) == 0 ) {
					throw "@rpath not supported in dyld shared cache";
				}
				// <rdar://problem/9161945> silently ignore dependents from main executables that can't be in shared cache
				bool addDependent = true;
				if ( fLayout->getFileType() == MH_EXECUTE ) {
					if ( (strncmp(dependentPath, "/usr/lib/", 9) != 0) && (strncmp(dependentPath, "/System/Library/", 16) != 0) ) {
						addDependent = false;
					}
				}
				if ( addDependent )
					fDependsOn.insert(fGraph->getNodeForVirtualPath(dependentPath));
			}
			catch (const char* msg) {
				if ( it->weakImport || ! fLayout->hasSplitSegInfo() ) {
					// ok to ignore missing weak imported dylibs from things that are
					// not going to be in the dyld shared cache
				}
				else {
					fprintf(stderr, "warning, could not bind %s because %s\n", fPath, msg);
					fDependentMissing = true;
				}
			}
		}
		// recurse
		for(std::set<DependencyNode*>::iterator it = fDependsOn.begin(); it != fDependsOn.end(); ++it) {
			(*it)->loadDependencies(mainExecutableLayout);
		}
	}
}

void ArchGraph::DependencyNode::markNeededByRoot(ArchGraph::DependencyNode* rootNode)
{
	if ( fRootsDependentOnThis.count(rootNode) == 0 ) {
		fRootsDependentOnThis.insert(rootNode);
		for(std::set<DependencyNode*>::iterator it = fDependsOn.begin(); it != fDependsOn.end(); ++it) {
			(*it)->markNeededByRoot(rootNode);
		}
	}
}


ArchGraph::DependencyNode::DependencyNode(ArchGraph* graph, const char* path, const MachOLayoutAbstraction* layout) 
 : fGraph(graph), fPath(strdup(path)), fLayout(layout), fDependenciesLoaded(false), fDependentMissing(false)
{
	//fprintf(stderr, "new DependencyNode(0x%08X, %s)\n", graph->fArch, path);
}

void ArchGraph::findSharedDylibs(ArchPair ap)
{
	const PathToNode& nodes = fgPerArchGraph[ap]->fNodes;
	std::set<const MachOLayoutAbstraction*> possibleLibs;
	//fprintf(stderr, "shared for arch %s\n", archName(ap));
	for(PathToNode::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
		DependencyNode* node = it->second;
		// <rdar://problem/6127437> put all dylibs in shared cache - not just ones used by more than one app
		if ( node->allDependentsFound() /*&& (node->useCount() > 1)*/ ) {
			const MachOLayoutAbstraction* layout = node->getLayout();
			if ( layout->isDylib() ) {
				char* msg;
				if ( sharable(layout, ap, &msg) ) {
					possibleLibs.insert(layout);
				}
				else {
					if ( layout->getID().name[0] == '@' ) {
						// <rdar://problem/7770139> update_dyld_shared_cache should suppress warnings for embedded frameworks
					}
					else {
						warnings.push_back(msg);
						fprintf(stderr, "update_dyld_shared_cache: for arch %s, %s\n", archName(ap), msg);
					}
				}
			}
		}
	}
	
	// prune so that all shareable libs depend only on other shareable libs
	std::set<const MachOLayoutAbstraction*>& sharedLibs = fgPerArchGraph[ap]->fSharedDylibs;
	std::map<const MachOLayoutAbstraction*,bool> shareableMap;
	for (std::set<const MachOLayoutAbstraction*>::iterator lit = possibleLibs.begin(); lit != possibleLibs.end(); ++lit) {
		if ( canBeShared(*lit, ap, possibleLibs, shareableMap) )
			sharedLibs.insert(*lit);
	}
}

const char*	ArchGraph::archName(ArchPair ap)
{
	switch ( ap.arch ) {
		case CPU_TYPE_I386:
			return "i386";
		case CPU_TYPE_X86_64:
			return "x86_64";
		case CPU_TYPE_ARM:
			switch ( ap.subtype ) {
				case CPU_SUBTYPE_ARM_V4T:
					return "armv4t";
				case CPU_SUBTYPE_ARM_V6:
					return "armv6";
				case CPU_SUBTYPE_ARM_V5TEJ:
					return "armv5";
				case CPU_SUBTYPE_ARM_XSCALE:
					return "arm-xscale";
				case CPU_SUBTYPE_ARM_V7:
					return "armv7";
				case CPU_SUBTYPE_ARM_V7F:
					return "armv7f";
				case CPU_SUBTYPE_ARM_V7K:
					return "armv7k";
				case CPU_SUBTYPE_ARM_V7S:
					return "armv7s";
				default:
					return "arm";
			}
		default:
			return "unknown";
	}
}

bool ArchGraph::sharable(const MachOLayoutAbstraction* layout, ArchPair ap, char** msg)
{
	if ( ! layout->isTwoLevelNamespace() ) 
		asprintf(msg, "can't put %s in shared cache because it was built -flat_namespace", layout->getID().name);
	else if ( ! layout->hasSplitSegInfo() ) 
		asprintf(msg, "can't put %s in shared cache because it was not built for %s or later", layout->getID().name, (iPhoneOS ? "iPhoneOS 3.1" : "MacOSX 10.5"));
	else if ( ! layout->isRootOwned() )
		asprintf(msg, "can't put %s in shared cache because it is not owned by root", layout->getID().name);
	else if ( ! layout->inSharableLocation() )
		asprintf(msg, "can't put %s in shared cache because it is not in /usr/lib or /System/Library", layout->getID().name);
	else if ( layout->hasDynamicLookupLinkage() )
		asprintf(msg, "can't put %s in shared cache because it was built with '-undefined dynamic_lookup'", layout->getID().name);
	else if ( layout->hasMainExecutableLookupLinkage() )
		asprintf(msg, "can't put %s in shared cache because it was built with '-bundle_loader'", layout->getID().name);
	//else if ( ! layout->hasDyldInfo() )
	//	asprintf(msg, "can't put %s in shared cache because it was built for older OS", layout->getID().name);
	else
		return true;
	return false;
}

bool ArchGraph::canBeShared(const MachOLayoutAbstraction* layout, ArchPair ap, const std::set<const MachOLayoutAbstraction*>& possibleLibs, std::map<const MachOLayoutAbstraction*, bool>& shareableMap)
{
	// check map which is a cache of results
	std::map<const MachOLayoutAbstraction*, bool>::iterator mapPos = shareableMap.find(layout);
	if ( mapPos != shareableMap.end() ) {
		return mapPos->second;
	}
	// see if possible
	if ( possibleLibs.count(layout) == 0 ) {
		shareableMap[layout] = false;
		char* msg;
		if ( sharable(layout, ap, &msg) )
			asprintf(&msg, "can't put %s in shared cache, unknown reason", layout->getID().name);
		warnings.push_back(msg);
		if ( verbose )
			fprintf(stderr, "update_dyld_shared_cache: for arch %s, %s\n", archName(ap), msg);
		return false;
	}
	// look recursively
	shareableMap[layout] = true; // mark this shareable early in case of circular references
	const PathToNode& nodes = fgPerArchGraph[ap]->fNodes;
	const std::vector<MachOLayoutAbstraction::Library>&	dependents = layout->getLibraries();
	for (std::vector<MachOLayoutAbstraction::Library>::const_iterator dit = dependents.begin(); dit != dependents.end(); ++dit) {
		PathToNode::const_iterator pos = nodes.find(dit->name);
		if ( pos == nodes.end() ) {
			// path from load command does not match any loaded dylibs, maybe there is a temp symlink
			char realPath[MAXPATHLEN];
			if ( realpath(dit->name, realPath) != NULL ) {
				if ( nodes.find(realPath) != nodes.end() )
					continue;
			}
			// handle weak imported dylibs not found
			if ( dit->weakImport )
				continue;
			shareableMap[layout] = false;
			char* msg;
			asprintf(&msg, "can't put %s in shared cache because it depends on %s which can't be found", layout->getID().name, dit->name);
			warnings.push_back(msg);
			if ( verbose )
				fprintf(stderr, "update_dyld_shared_cache: for arch %s, %s\n", archName(ap), msg);
			return false;
		}
		else {
			if ( ! canBeShared(pos->second->getLayout(), ap, possibleLibs, shareableMap) ) {
				shareableMap[layout] = false;
				char* msg;
				asprintf(&msg, "can't put %s in shared cache because it depends on %s which can't be in shared cache", layout->getID().name, dit->name);
				warnings.push_back(msg);
				if ( verbose )
					fprintf(stderr, "update_dyld_shared_cache: for arch %s, %s\n", archName(ap), msg);
				return false;
			}
		}
	}
	return true;
}



class StringPool
{
public:
				StringPool();
	const char*	getBuffer();
	uint32_t	size();
	uint32_t	add(const char* str);
	uint32_t	addUnique(const char* str);
	const char* stringAtIndex(uint32_t) const;
private:
	typedef std::unordered_map<const char*, uint32_t, CStringHash, CStringEquals> StringToOffset;

	char*			fBuffer;
	uint32_t		fBufferAllocated;
	uint32_t		fBufferUsed;
	StringToOffset	fUniqueStrings;
};


StringPool::StringPool() 
	: fBufferUsed(0), fBufferAllocated(48*1024*1024)
{
	fBuffer = (char*)malloc(fBufferAllocated);
}

uint32_t StringPool::add(const char* str)
{
	uint32_t len = strlen(str);
	if ( (fBufferUsed + len + 1) > fBufferAllocated ) {
		// grow buffer
		throw "string buffer exhausted";
	}
	strcpy(&fBuffer[fBufferUsed], str);
	uint32_t result = fBufferUsed;
	fUniqueStrings[&fBuffer[fBufferUsed]] = result;
	fBufferUsed += len+1;
	return result;
}

uint32_t StringPool::addUnique(const char* str)
{
	StringToOffset::iterator pos = fUniqueStrings.find(str);
	if ( pos != fUniqueStrings.end() ) 
		return pos->second;
	else {
		//fprintf(stderr, "StringPool::addUnique() new string: %s\n", str);
		return this->add(str);
	}
}

uint32_t StringPool::size()
{
	return fBufferUsed;
}

const char*	StringPool::getBuffer()
{
	return fBuffer;
}

const char* StringPool::stringAtIndex(uint32_t index) const
{
	return &fBuffer[index];
}



struct LocalSymbolInfo
{
	uint32_t	dylibOffset;
	uint32_t	nlistStartIndex;
	uint32_t	nlistCount;
};


template <typename A>
class SharedCache
{
public:
							SharedCache(ArchGraph* graph, const char* rootPath, const char* overlayPath, const char* cacheDir, bool explicitCacheDir,
											bool alphaSort, bool verify, bool optimize, uint64_t dyldBaseAddress);
	bool					update(bool force, bool optimize, bool deleteExistingFirst, int archIndex, 
										int archCount, bool keepSignatures, bool dontMapLocalSymbols);
	static const char*		cacheFileSuffix(bool optimized, const char* archName);

    // vm address = address AS WRITTEN into the cache
    // mapped address = address AS MAPPED into the update process only
    // file offset = offset relative to start of cache file
    void *					mappedAddressForVMAddress(uint64_t vmaddr);
    uint64_t 				VMAddressForMappedAddress(const void *mapaddr);
	uint64_t				cacheFileOffsetForVMAddress(uint64_t addr) const;
	uint64_t				VMAddressForCacheFileOffset(uint64_t addr) const;

	static const char*		archName();

private:
	typedef typename A::P			P;
    typedef typename A::P::E		E;
    typedef typename A::P::uint_t	pint_t;

	bool					notUpToDate(const char* path, unsigned int aliasCount);
	bool					notUpToDate(const void* cache, unsigned int aliasCount);
	uint8_t*				optimizeLINKEDIT(bool keepSignatures, bool dontMapLocalSymbols);
	void					optimizeObjC(std::vector<void*>& pointersInData);

	static void				getSharedCacheBasAddresses(cpu_type_t arch, uint64_t* baseReadOnly, uint64_t* baseWritable);
	static cpu_type_t		arch();
	static uint64_t			sharedRegionStartAddress();
	static uint64_t			sharedRegionSize();
	static uint64_t			sharedRegionStartWritableAddress(uint64_t);
	static uint64_t			sharedRegionStartReadOnlyAddress(uint64_t, uint64_t);
	static uint64_t			getWritableSegmentNewAddress(uint64_t proposedNewAddress, uint64_t originalAddress, uint64_t executableSlide);
	static bool				addCacheSlideInfo();
	
	static uint64_t			pageAlign(uint64_t addr);
	void					assignNewBaseAddresses(bool verify);

	struct LayoutInfo {
		const MachOLayoutAbstraction*		layout;
		std::vector<const char*>			aliases;
		dyld_cache_image_info				info;
	};
	
	struct ByNameSorter {
		bool operator()(const LayoutInfo& left, const LayoutInfo& right) 
				{ return (strcmp(left.layout->getID().name, right.layout->getID().name) < 0); }
	};
	
	struct ByAddressSorter {
		bool operator()(const LayoutInfo& left, const LayoutInfo& right) { 
			return (left.layout->getSegments()[0].newAddress() < right.layout->getSegments()[0].newAddress()); 
		}
	};

    struct ByCStringSectionSizeSorter {
        bool operator()(const LayoutInfo& left, const LayoutInfo& right) {
            const std::vector<MachOLayoutAbstraction::Segment>& segs_l =
                left.layout->getSegments();
            const std::vector<MachOLayoutAbstraction::Segment>& segs_r = 
                right.layout->getSegments();
            if (segs_l.size() == 0  ||  segs_r.size() == 0) {
                // one image has no segments
                return segs_l.size() > segs_r.size();
            }
            const macho_header<P> *mh_l = (macho_header<P>*)segs_l[0].mappedAddress();
            const macho_header<P> *mh_r = (macho_header<P>*)segs_r[0].mappedAddress();
            const macho_section<P> *cstring_l = mh_l->getSection("__TEXT", "__cstring");
            const macho_section<P> *cstring_r = mh_r->getSection("__TEXT", "__cstring");
            if (!cstring_l  ||  !cstring_r) {
                // one image has no cstrings
                return cstring_l && !cstring_r;
            }

            return cstring_l->size() > cstring_r->size();
        }
    };

	struct Sorter {
		Sorter(std::map<const MachOLayoutAbstraction*, uint32_t>& map): fMap(map) {}
		bool operator()(const LayoutInfo& left, const LayoutInfo& right) {
			return (fMap[left.layout] < fMap[right.layout]); 
		}
	private:
		std::map<const MachOLayoutAbstraction*, uint32_t>& fMap;
	};
	
	
	ArchGraph*							fArchGraph;
	const bool							fVerify;
	bool								fExistingIsNotUpToDate;
	bool								fCacheFileInFinalLocation;
	const char*							fCacheFilePath;
	uint8_t*							fExistingCacheForVerification;
	std::vector<LayoutInfo>				fDylibs;
	std::vector<LayoutInfo>				fDylibAliases;
	std::vector<shared_file_mapping_np>	fMappings;
	std::vector<macho_nlist<P> >		fUnmappedLocalSymbols;
	StringPool							fUnmappedLocalsStringPool;
	std::vector<LocalSymbolInfo>		fLocalSymbolInfos;
	uint32_t							fHeaderSize;
    uint8_t*							fInMemoryCache;
	uint64_t							fDyldBaseAddress;
	uint64_t							fLinkEditsTotalUnoptimizedSize;
	uint64_t							fLinkEditsStartAddress;
	MachOLayoutAbstraction::Segment*	fFirstLinkEditSegment;
	uint32_t							fOffsetOfBindInfoInCombinedLinkedit;
	uint32_t							fOffsetOfWeakBindInfoInCombinedLinkedit;
	uint32_t							fOffsetOfLazyBindInfoInCombinedLinkedit;
	uint32_t							fOffsetOfExportInfoInCombinedLinkedit;
	uint32_t							fOffsetOfOldSymbolTableInfoInCombinedLinkedit;
	uint32_t							fSizeOfOldSymbolTableInfoInCombinedLinkedit;
	uint32_t							fOffsetOfOldExternalRelocationsInCombinedLinkedit;
	uint32_t							fSizeOfOldExternalRelocationsInCombinedLinkedit;
	uint32_t							fOffsetOfOldIndirectSymbolsInCombinedLinkedit;
	uint32_t							fSizeOfOldIndirectSymbolsInCombinedLinkedit;
	uint32_t							fOffsetOfOldStringPoolInCombinedLinkedit;
	uint32_t							fSizeOfOldStringPoolInCombinedLinkedit;
	uint32_t							fOffsetOfFunctionStartsInCombinedLinkedit;
	uint32_t							fSizeOfFunctionStartsInCombinedLinkedit;
	uint32_t							fOffsetOfDataInCodeInCombinedLinkedit;
	uint32_t							fSizeOfDataInCodeInCombinedLinkedit;
	uint32_t							fLinkEditsTotalOptimizedSize;
	uint32_t							fUnmappedLocalSymbolsSize;
};


// Access a section containing a list of pointers
template <typename A, typename T>
class PointerSection 
{
    typedef typename A::P P;
    typedef typename A::P::uint_t pint_t;

    SharedCache<A>* const			fCache;
    const macho_section<P>* const	fSection;
    pint_t * const					fBase;
    uint64_t						fCount;

public:
    PointerSection(SharedCache<A>* cache, const macho_header<P>* header, 
                   const char *segname, const char *sectname)
        : fCache(cache)
        , fSection(header->getSection(segname, sectname))
        , fBase(fSection ? (pint_t *)cache->mappedAddressForVMAddress(fSection->addr()) : 0)
        , fCount(fSection ? fSection->size() / sizeof(pint_t) : 0)
    {
    }

    uint64_t count() const { return fCount; }

    uint64_t getUnmapped(uint64_t index) const {
        if (index >= fCount) throwf("index out of range");
        return P::getP(fBase[index]);
    }

    T get(uint64_t index) const { 
        return (T)fCache->mappedAddressForVMAddress(getUnmapped(index));
    }

    void set(uint64_t index, uint64_t value) {
        if (index >= fCount) throwf("index out of range");
        P::setP(fBase[index], value);
    }
	
    void removeNulls() {
        uint64_t shift = 0;
        for (uint64_t i = 0; i < fCount; i++) {
            pint_t value = fBase[i];
            if (value) {
                fBase[i-shift] = value;
            } else {
                shift++;
            }
        }
        fCount -= shift;
		const_cast<macho_section<P>*>(fSection)->set_size(fCount * sizeof(pint_t));
    }
};

// Access a section containing an array of structures
template <typename A, typename T>
class ArraySection 
{
    typedef typename A::P P;

    SharedCache<A>* const fCache;
    const macho_section<P>* const fSection;
    T * const fBase;
    uint64_t const fCount;

public:
    ArraySection(SharedCache<A>* cache, const macho_header<P>* header, 
                 const char *segname, const char *sectname)
        : fCache(cache)
        , fSection(header->getSection(segname, sectname))
        , fBase(fSection ? (T *)cache->mappedAddressForVMAddress(fSection->addr()) : 0)
        , fCount(fSection ? fSection->size() / sizeof(T) : 0)
    {
    }

    uint64_t count() const { return fCount; }

    T& get(uint64_t index) const { 
        if (index >= fCount) throwf("index out of range");
        return fBase[index];
    }
};


// GrP fixme
#include "ObjCLegacyAbstraction.hpp"
#include "ObjCModernAbstraction.hpp"


	
template <>	 cpu_type_t	SharedCache<x86>::arch()	{ return CPU_TYPE_I386; }
template <>	 cpu_type_t	SharedCache<x86_64>::arch()	{ return CPU_TYPE_X86_64; }
template <>	 cpu_type_t	SharedCache<arm>::arch()	{ return CPU_TYPE_ARM; }

template <>	 uint64_t	SharedCache<x86>::sharedRegionStartAddress()			{ return 0x90000000; }
template <>	 uint64_t	SharedCache<x86_64>::sharedRegionStartAddress()			{ return 0x7FFF80000000LL; }
template <>	 uint64_t	SharedCache<arm>::sharedRegionStartAddress()			{ return ARM_SHARED_REGION_START; }

template <>	 uint64_t	SharedCache<x86>::sharedRegionSize()					{ return 0x20000000; }
template <>	 uint64_t	SharedCache<x86_64>::sharedRegionSize()					{ return 0x40000000; }
template <>	 uint64_t	SharedCache<arm>::sharedRegionSize()					{ return ARM_SHARED_REGION_SIZE; }

template <>	 uint64_t	SharedCache<x86>::sharedRegionStartWritableAddress(uint64_t exEnd)			{ return exEnd + 0x04000000; }
template <>	 uint64_t	SharedCache<x86_64>::sharedRegionStartWritableAddress(uint64_t exEnd)		{ return 0x7FFF70000000LL; }
template <>	 uint64_t	SharedCache<arm>::sharedRegionStartWritableAddress(uint64_t exEnd)			{ return (exEnd + 16383) & (-16384); }

template <>	 uint64_t	SharedCache<x86>::sharedRegionStartReadOnlyAddress(uint64_t wrEnd, uint64_t exEnd)	 { return wrEnd + 0x04000000; }
template <>	 uint64_t	SharedCache<x86_64>::sharedRegionStartReadOnlyAddress(uint64_t wrEnd, uint64_t exEnd){ return exEnd; }
template <>	 uint64_t	SharedCache<arm>::sharedRegionStartReadOnlyAddress(uint64_t wrEnd, uint64_t exEnd)	 { return (wrEnd + 16383) & (-16384); }


template <>	 const char*	SharedCache<x86>::archName()	{ return "i386"; }
template <>	 const char*	SharedCache<x86_64>::archName()	{ return "x86_64"; }
template <>	 const char*	SharedCache<arm>::archName()	{ return "arm"; }

template <>	 const char*	SharedCache<x86>::cacheFileSuffix(bool, const char* archName)	{ return archName; }
template <>	 const char*	SharedCache<x86_64>::cacheFileSuffix(bool, const char* archName){ return archName; }
template <>	 const char*	SharedCache<arm>::cacheFileSuffix(bool, const char* archName)	{ return archName; }


template <>  uint64_t		SharedCache<x86>::pageAlign(uint64_t addr)    { return ( (addr + 4095) & (-4096) ); }
template <>  uint64_t		SharedCache<x86_64>::pageAlign(uint64_t addr) { return ( (addr + 4095) & (-4096) ); }
template <>  uint64_t		SharedCache<arm>::pageAlign(uint64_t addr)    { return ( (addr + 4095) & (-4096) ); }


template <typename A>
SharedCache<A>::SharedCache(ArchGraph* graph, const char* rootPath, const char* overlayPath, const char* cacheDir, bool explicitCacheDir, bool alphaSort, bool verify, bool optimize, uint64_t dyldBaseAddress) 
  : fArchGraph(graph), fVerify(verify), fExistingIsNotUpToDate(true), 
	fCacheFileInFinalLocation(rootPath[0] == '\0'), fCacheFilePath(NULL),
	fExistingCacheForVerification(NULL), fDyldBaseAddress(dyldBaseAddress),
	fOffsetOfBindInfoInCombinedLinkedit(0), fOffsetOfWeakBindInfoInCombinedLinkedit(0),
	fOffsetOfLazyBindInfoInCombinedLinkedit(0), fOffsetOfExportInfoInCombinedLinkedit(0),
	fOffsetOfOldSymbolTableInfoInCombinedLinkedit(0), fSizeOfOldSymbolTableInfoInCombinedLinkedit(0),
	fOffsetOfOldExternalRelocationsInCombinedLinkedit(0), fSizeOfOldExternalRelocationsInCombinedLinkedit(0),
	fOffsetOfOldIndirectSymbolsInCombinedLinkedit(0), fSizeOfOldIndirectSymbolsInCombinedLinkedit(0),
	fOffsetOfOldStringPoolInCombinedLinkedit(0), fSizeOfOldStringPoolInCombinedLinkedit(0),
	fOffsetOfFunctionStartsInCombinedLinkedit(0), fSizeOfFunctionStartsInCombinedLinkedit(0),
	fOffsetOfDataInCodeInCombinedLinkedit(0), fSizeOfDataInCodeInCombinedLinkedit(0),
	fUnmappedLocalSymbolsSize(0)
{
	if ( fArchGraph->getArchPair().arch != arch() )
		throwf("SharedCache object is wrong architecture: 0x%08X vs 0x%08X", fArchGraph->getArchPair().arch, arch());
		
	// build vector of all shared dylibs
	unsigned int aliasCount = 0;
	std::set<const MachOLayoutAbstraction*>& dylibs = fArchGraph->getSharedDylibs();
	ArchGraph::StringToString& aliases = fArchGraph->getDylibAliases();
	for(std::set<const MachOLayoutAbstraction*>::iterator it = dylibs.begin(); it != dylibs.end(); ++it) {
		const MachOLayoutAbstraction* lib = *it;
		LayoutInfo temp;
		temp.layout = lib;
		temp.info.address = 0;
		temp.info.modTime = lib->getLastModTime();
		temp.info.inode = lib->getInode();
		temp.info.pathFileOffset = lib->getNameFileOffset();  // for now this is the offset within the dylib
		for(ArchGraph::StringToString::iterator ait = aliases.begin(); ait != aliases.end(); ++ait) {
			if ( strcmp(ait->second, lib->getID().name) == 0 ) {
				temp.aliases.push_back(ait->first);
				++aliasCount;
			}
		}
		fDylibs.push_back(temp);
	}

	// create path to cache file
	char cachePathCanonical[MAXPATHLEN];
	strcpy(cachePathCanonical, cacheDir);
	if ( cachePathCanonical[strlen(cachePathCanonical)-1] != '/' )
		strcat(cachePathCanonical, "/");
	strcat(cachePathCanonical, DYLD_SHARED_CACHE_BASE_NAME);
	strcat(cachePathCanonical, cacheFileSuffix(optimize, fArchGraph->archName()));
	char cachePath[MAXPATHLEN];
	if ( explicitCacheDir ) {
		fCacheFilePath = strdup(cachePathCanonical);
	}
	else if ( overlayPath[0] != '\0' ) {
		strcpy(cachePath, overlayPath);
		strcat(cachePath, "/");
		strcat(cachePath, cachePathCanonical);
		fCacheFilePath = strdup(cachePath);
	}
	else if ( rootPath[0] != '\0' ) {
		strcpy(cachePath, rootPath);
		strcat(cachePath, "/");
		strcat(cachePath, cachePathCanonical);
		fCacheFilePath = strdup(cachePath);
	}
	else {
		fCacheFilePath = strdup(cachePathCanonical);
	}
	if ( overlayPath[0] != '\0' ) {
		// in overlay mode if there already is a cache file in the overlay
		// check if it is up to date.  
		struct stat stat_buf;
		if ( stat(fCacheFilePath, &stat_buf) == 0 ) {
			fExistingIsNotUpToDate = this->notUpToDate(fCacheFilePath, aliasCount);
		}
		else if ( rootPath[0] != '\0' ) {
			// using -root and -overlay, but no cache file in overlay, check one in -root
			char cachePathRoot[MAXPATHLEN];
			strcpy(cachePathRoot, rootPath);
			strcat(cachePathRoot, "/");
			strcat(cachePathRoot, cachePathCanonical);
			fExistingIsNotUpToDate = this->notUpToDate(cachePathRoot, aliasCount);
		}
		else {
			// uisng -overlay, but no cache file in overlay, check one in boot volume
			fExistingIsNotUpToDate = this->notUpToDate(cachePathCanonical, aliasCount);
		}
	}
	else {
		fExistingIsNotUpToDate = this->notUpToDate(fCacheFilePath, aliasCount);
	}
	
	// sort shared dylibs
	if ( verify ) {
		// already sorted by notUpToDate()
	}
	else if ( alphaSort ) {
		std::sort(fDylibs.begin(), fDylibs.end(), ByNameSorter());
	}
	else {
		// random sort for Address Space Randomization
		std::map<const MachOLayoutAbstraction*, uint32_t> map;
		for(typename std::vector<struct LayoutInfo>::const_iterator it = fDylibs.begin(); it != fDylibs.end(); ++it) 
			map[it->layout] = arc4random();
		std::sort(fDylibs.begin(), fDylibs.end(), Sorter(map));
	}
	
	// assign segments in each dylib a new address
	this->assignNewBaseAddresses(verify);
	
	// calculate where string pool offset will start
	// calculate cache file header size
	fHeaderSize = sizeof(dyld_cache_header) 
							+ fMappings.size()*sizeof(shared_file_mapping_np) 
							+ (fDylibs.size()+aliasCount)*sizeof(dyld_cache_image_info);
	const uint64_t baseHeaderSize = fHeaderSize;
	//fprintf(stderr, "aliasCount=%d, fHeaderSize=0x%08X\n", aliasCount, fHeaderSize);
	// build list of aliases and compute where each ones path string will go
	for(typename std::vector<struct LayoutInfo>::const_iterator it = fDylibs.begin(); it != fDylibs.end(); ++it) {
		for(std::vector<const char*>::const_iterator ait = it->aliases.begin(); ait != it->aliases.end(); ++ait) {
			LayoutInfo temp = *it;
			// alias looks just like real dylib, but has a different name string
			const char* aliasPath = *ait;
			temp.aliases.clear();
			temp.aliases.push_back(aliasPath);
			temp.info.pathFileOffset = fHeaderSize; 
			fDylibAliases.push_back(temp);
			fHeaderSize += strlen(aliasPath)+1;
		}
	}
	std::sort(fDylibAliases.begin(), fDylibAliases.end(), ByNameSorter());
	//fprintf(stderr, "fHeaderSize=0x%08X, fDylibAliases.size()=%lu\n", fHeaderSize, fDylibAliases.size());
	fHeaderSize = pageAlign(fHeaderSize);
	
	// check that cache we are about to create for verification purposes has same layout as existing cache
	if ( verify ) {
		// if no existing cache, say so
		if ( fExistingCacheForVerification == NULL ) {
			throwf("update_dyld_shared_cache[%u] for arch=%s, could not verify because cache file does not exist in /var/db/dyld/\n",
			 getpid(), fArchGraph->archName());
		}
		const dyldCacheHeader<E>* header = (dyldCacheHeader<E>*)fExistingCacheForVerification;
		const dyldCacheImageInfo<E>* cacheEntry = (dyldCacheImageInfo<E>*)(fExistingCacheForVerification + header->imagesOffset());
		for(typename std::vector<LayoutInfo>::iterator it = fDylibs.begin(); it != fDylibs.end(); ++it, ++cacheEntry) {
			if ( cacheEntry->address() != it->layout->getSegments()[0].newAddress() ) {
				throwf("update_dyld_shared_cache[%u] warning: for arch=%s, could not verify cache because start address of %s is 0x%llX in cache, but should be 0x%llX\n",
							getpid(), fArchGraph->archName(), it->layout->getID().name, cacheEntry->address(), it->layout->getSegments()[0].newAddress());
			}
		}
	}
	
	
	if ( fHeaderSize > FIRST_DYLIB_TEXT_OFFSET )
		throwf("header size overflow: allowed=0x%08X, base=0x%08llX, aliases=0x%08llX", FIRST_DYLIB_TEXT_OFFSET, baseHeaderSize, fHeaderSize-baseHeaderSize);
}


template <typename A>
uint64_t SharedCache<A>::getWritableSegmentNewAddress(uint64_t proposedNewAddress, uint64_t originalAddress, uint64_t executableSlide)
{
	return proposedNewAddress;
}


template <typename A>
void SharedCache<A>::assignNewBaseAddresses(bool verify)
{
	// first layout TEXT for dylibs
	const uint64_t startExecuteAddress = sharedRegionStartAddress();
	uint64_t currentExecuteAddress = startExecuteAddress + FIRST_DYLIB_TEXT_OFFSET;	
	for(typename std::vector<LayoutInfo>::iterator it = fDylibs.begin(); it != fDylibs.end(); ++it) {
		std::vector<MachOLayoutAbstraction::Segment>& segs = ((MachOLayoutAbstraction*)(it->layout))->getSegments();
		for (int i=0; i < segs.size(); ++i) {
			MachOLayoutAbstraction::Segment& seg = segs[i];
			seg.reset();
			if ( seg.executable() && !seg.writable() ) {
				// __TEXT segment
				if ( it->info.address == 0 )
					it->info.address = currentExecuteAddress;
				seg.setNewAddress(currentExecuteAddress);
				currentExecuteAddress += pageAlign(seg.size());
			}
		}
	}

	// layout DATA for dylibs
	const uint64_t startWritableAddress = sharedRegionStartWritableAddress(currentExecuteAddress);
	uint64_t currentWritableAddress = startWritableAddress;
	for(typename std::vector<LayoutInfo>::iterator it = fDylibs.begin(); it != fDylibs.end(); ++it) {
		std::vector<MachOLayoutAbstraction::Segment>& segs = ((MachOLayoutAbstraction*)(it->layout))->getSegments();
		for (int i=0; i < segs.size(); ++i) {
			MachOLayoutAbstraction::Segment& seg = segs[i];
			seg.reset();
			if ( seg.writable() ) {
				if ( seg.executable() ) 
					throw "found writable and executable segment";
				// __DATA segment
				seg.setNewAddress(currentWritableAddress);
				currentWritableAddress = pageAlign(seg.newAddress() + seg.size());
			}
		}
	}

	// layout all read-only (but not LINKEDIT) segments
	const uint64_t startReadOnlyAddress = sharedRegionStartReadOnlyAddress(currentWritableAddress, currentExecuteAddress);
	uint64_t currentReadOnlyAddress = startReadOnlyAddress;
	for(typename std::vector<LayoutInfo>::iterator it = fDylibs.begin(); it != fDylibs.end(); ++it) {
		std::vector<MachOLayoutAbstraction::Segment>& segs = ((MachOLayoutAbstraction*)(it->layout))->getSegments();
		for(int i=0; i < segs.size(); ++i) {
			MachOLayoutAbstraction::Segment& seg = segs[i];
			if ( seg.readable() && !seg.writable() && !seg.executable() && (strcmp(seg.name(), "__LINKEDIT") != 0) ) {
				// __UNICODE segment
				seg.setNewAddress(currentReadOnlyAddress);
				currentReadOnlyAddress += pageAlign(seg.size());
			}
		}
	}	

	// layout all LINKEDIT segments at end of all read-only segments
	fLinkEditsStartAddress = currentReadOnlyAddress;
	fFirstLinkEditSegment = NULL;
	for(typename std::vector<LayoutInfo>::iterator it = fDylibs.begin(); it != fDylibs.end(); ++it) {
		std::vector<MachOLayoutAbstraction::Segment>& segs = ((MachOLayoutAbstraction*)(it->layout))->getSegments();
		for(int i=0; i < segs.size(); ++i) {
			MachOLayoutAbstraction::Segment& seg = segs[i];
			if ( seg.readable() && !seg.writable() && !seg.executable() && (strcmp(seg.name(), "__LINKEDIT") == 0) ) {
				if ( fFirstLinkEditSegment == NULL ) 
					fFirstLinkEditSegment = &seg;
				seg.setNewAddress(currentReadOnlyAddress);
				currentReadOnlyAddress += pageAlign(seg.size());
			}
		}
	}
	fLinkEditsTotalUnoptimizedSize = pageAlign(currentReadOnlyAddress - fLinkEditsStartAddress);

	// populate large mappings
	uint64_t cacheFileOffset = 0;
	if ( currentExecuteAddress > startExecuteAddress ) {
		shared_file_mapping_np  executeMapping;
		executeMapping.sfm_address		= startExecuteAddress;
		executeMapping.sfm_size			= currentExecuteAddress - startExecuteAddress;
		executeMapping.sfm_file_offset	= cacheFileOffset;
		executeMapping.sfm_max_prot		= VM_PROT_READ | VM_PROT_EXECUTE;
		executeMapping.sfm_init_prot	= VM_PROT_READ | VM_PROT_EXECUTE;
		fMappings.push_back(executeMapping);
		cacheFileOffset += executeMapping.sfm_size;
		
		shared_file_mapping_np  writableMapping;
		writableMapping.sfm_address		= startWritableAddress;
		writableMapping.sfm_size		= currentWritableAddress - startWritableAddress;
		writableMapping.sfm_file_offset	= cacheFileOffset;
		writableMapping.sfm_max_prot	= VM_PROT_READ | VM_PROT_WRITE;
		writableMapping.sfm_init_prot	= VM_PROT_READ | VM_PROT_WRITE;
		fMappings.push_back(writableMapping);
		cacheFileOffset += writableMapping.sfm_size;
				
		// make read-only (contains LINKEDIT segments) last, so it can be cut back when optimized
		shared_file_mapping_np  readOnlyMapping;
		readOnlyMapping.sfm_address		= startReadOnlyAddress;
		readOnlyMapping.sfm_size		= currentReadOnlyAddress - startReadOnlyAddress;
		readOnlyMapping.sfm_file_offset	= cacheFileOffset;
		readOnlyMapping.sfm_max_prot	= VM_PROT_READ;
		readOnlyMapping.sfm_init_prot	= VM_PROT_READ;
		fMappings.push_back(readOnlyMapping);
		cacheFileOffset += readOnlyMapping.sfm_size;
	}
	else {
		// empty cache
		shared_file_mapping_np  cacheHeaderMapping;
		cacheHeaderMapping.sfm_address		= startExecuteAddress;
		cacheHeaderMapping.sfm_size			= FIRST_DYLIB_TEXT_OFFSET;
		cacheHeaderMapping.sfm_file_offset	= cacheFileOffset;
		cacheHeaderMapping.sfm_max_prot		= VM_PROT_READ;
		cacheHeaderMapping.sfm_init_prot	= VM_PROT_READ;
		fMappings.push_back(cacheHeaderMapping);
		cacheFileOffset += cacheHeaderMapping.sfm_size;
	}
}


template <typename A>
uint64_t SharedCache<A>::cacheFileOffsetForVMAddress(uint64_t vmaddr) const
{
	for(std::vector<shared_file_mapping_np>::const_iterator it = fMappings.begin(); it != fMappings.end(); ++it) {
		if ( (it->sfm_address <= vmaddr) && (vmaddr < it->sfm_address+it->sfm_size) )
			return it->sfm_file_offset + vmaddr - it->sfm_address;
	}
	throwf("address 0x%0llX is not in cache", vmaddr);
}

template <typename A>
uint64_t SharedCache<A>::VMAddressForCacheFileOffset(uint64_t offset) const
{
    for(std::vector<shared_file_mapping_np>::const_iterator it = fMappings.begin(); it != fMappings.end(); ++it) {
        if ( (it->sfm_file_offset <= offset) && (offset < it->sfm_file_offset+it->sfm_size) )
            return it->sfm_address + offset - it->sfm_file_offset;
    }
    throwf("offset 0x%0llX is not in cache", offset);
}

template <typename A>
void *SharedCache<A>::mappedAddressForVMAddress(uint64_t vmaddr)
{
    if (!vmaddr) return NULL;
    else return fInMemoryCache + cacheFileOffsetForVMAddress(vmaddr);
}

template <typename A>
uint64_t SharedCache<A>::VMAddressForMappedAddress(const void *mapaddr)
{
    if (!mapaddr) return 0;
    uint64_t offset = (uint8_t *)mapaddr - (uint8_t *)fInMemoryCache;
    return VMAddressForCacheFileOffset(offset);
}


template <typename A>
bool SharedCache<A>::notUpToDate(const void* cache, unsigned int aliasCount)
{
	dyldCacheHeader<E>* header = (dyldCacheHeader<E>*)cache;
	// not valid if header signature is wrong
	const char* archPairName = fArchGraph->archName();
	char temp[16];
	strcpy(temp, "dyld_v1        ");
	strcpy(&temp[15-strlen(archPairName)], archPairName);
	if ( strcmp(header->magic(), temp) != 0 ) {
		if ( fVerify ) {
			fprintf(stderr, "update_dyld_shared_cache[%u] cannot verify %s because current cache file has invalid header\n", getpid(), archName());
			return false;
		}
		else {
			fprintf(stderr, "update_dyld_shared_cache[%u] updating cache because current cache file has invalid header\n", getpid());
			return true;
		}
	}
	// not valid if count of images does not match current images needed
	if ( header->imagesCount() != (fDylibs.size()+aliasCount) ) {
		if ( fVerify ) {
			fprintf(stderr, "update_dyld_shared_cache[%u] cannot verify %s because current cache file contains a different set of dylibs\n", getpid(), archName());
			return false;
		}
		else {
			fprintf(stderr, "update_dyld_shared_cache[%u] updating %s cache because current cache file contains a different set of dylibs\n", getpid(), archName());
			return true;
		}
	}
	// get end of TEXT region
	const dyldCacheFileMapping<E>* textMapping = (dyldCacheFileMapping<E>*)((uint8_t*)cache+sizeof(dyldCacheHeader<E>));
	const uint32_t textSize = textMapping->size();
	
	// verify every dylib in constructed graph is in existing cache with same inode and modTime	
	std::map<const MachOLayoutAbstraction*, uint32_t> sortingMap;
	const dyldCacheImageInfo<E>* imagesStart = (dyldCacheImageInfo<E>*)((uint8_t*)cache + header->imagesOffset());
	const dyldCacheImageInfo<E>* imagesEnd = &imagesStart[header->imagesCount()];
	for(typename std::vector<LayoutInfo>::iterator it = fDylibs.begin(); it != fDylibs.end(); ++it) {
		bool found = false;
		//fprintf(stderr, "inode=0x%llX, mTime=0x%llX, path=%s\n", it->info.inode, it->info.modTime, it->layout->getID().name);
		for(const dyldCacheImageInfo<E>* cacheEntry = imagesStart; cacheEntry < imagesEnd; ++cacheEntry) {
			if ( fVerify ) {
				if ( cacheEntry->pathFileOffset() > textSize ) {
					throwf("update_dyld_shared_cache[%u]: for arch=%s, image entries corrupt, bad path offset in %s\n", 
								getpid(), archName(), it->layout->getID().name);
				}
				// in -verify mode, just match by path and warn if file looks different
				if ( strcmp((char*)cache+cacheEntry->pathFileOffset(), it->layout->getID().name) == 0 ) {
					found = true;
					sortingMap[it->layout] = cacheEntry-imagesStart;
					if ( (cacheEntry->inode() != it->info.inode) || (cacheEntry->modTime() != it->info.modTime) ) {
						fprintf(stderr, "update_dyld_shared_cache[%u] warning: for arch=%s, %s has changed since cache was built\n", 
								getpid(), archName(), it->layout->getID().name);
					}
					break;
				}
			}
			else {
				if ( cacheEntry->pathFileOffset() > textSize ) {
					// cache corrupt, needs to be regenerated
					return true;
				}
				// in normal update mode, everything has to match for cache to be up-to-date
				if ( (cacheEntry->inode() == it->info.inode) 
						&& (cacheEntry->modTime() == it->info.modTime) 
						&& (strcmp((char*)cache+cacheEntry->pathFileOffset(), it->layout->getID().name) == 0) ) {
					found = true;
					break;
				}
			}
		}
		if ( !found ) {
			if ( fVerify ) {
				throwf("update_dyld_shared_cache[%u] can't verify %s cache because %s is not in existing cache\n", getpid(), archName(), it->layout->getID().name);
			}
			else {
				fprintf(stderr, "update_dyld_shared_cache[%u] updating %s cache because dylib at %s has changed\n", getpid(), archName(), it->layout->getID().name);
				return true;
			}
		}
	}
	// all dylibs in existing cache file match those determined need to be in shared cache
	if ( fVerify ) {
		// sort fDylibs to match existing cache file so we can compare content
		std::sort(fDylibs.begin(), fDylibs.end(), Sorter(sortingMap));
		//fprintf(stderr, "dylibs sorted like existing cache:\n");
		//for(typename std::vector<LayoutInfo>::iterator it = fDylibs.begin(); it != fDylibs.end(); ++it) {
		//	fprintf(stderr,"   %s\n", it->layout->getID().name);
		//}
		// do regenerate a new cache so we can compare content with existing
		return true;
	}
	else {
		// existing cache file is up-to-date, don't need to regenerate
		return false;
	}
}


template <typename A>
bool SharedCache<A>::notUpToDate(const char* path, unsigned int aliasCount)
{
	// mmap existing cache file 
	int fd = ::open(path, O_RDONLY);	
	if ( fd == -1 )
		return true;
	struct stat stat_buf;
	::fstat(fd, &stat_buf);
    uint32_t cacheFileSize = stat_buf.st_size;
    uint32_t cacheAllocatedSize = pageAlign(cacheFileSize);
    uint8_t* mappingAddr = NULL;
	if ( vm_allocate(mach_task_self(), (vm_address_t*)(&mappingAddr), cacheAllocatedSize, VM_FLAGS_ANYWHERE) != KERN_SUCCESS )
        throwf("can't vm_allocate cache of size %u", cacheFileSize);
    // <rdar://problem/8960832> update_dyld_shared_cache -verify finds differences
 	(void)fcntl(fd, F_NOCACHE, 1);
    ssize_t readResult = pread(fd, mappingAddr, cacheFileSize, 0);
    if ( readResult != cacheFileSize )
        throwf("can't read all of existing cache file (%lu of %u): %s", readResult, cacheFileSize, path);
	::close(fd);

	// validate it
	bool result = this->notUpToDate(mappingAddr, aliasCount);
	if ( fVerify ) {
		// don't unmap yet, leave so it can be verified later
		fExistingCacheForVerification = mappingAddr;
	}
	else {
		// unmap
        vm_deallocate(mach_task_self(), (vm_address_t)mappingAddr, cacheAllocatedSize);
		if ( verbose && !result )
			fprintf(stderr, "update_dyld_shared_cache: %s is up-to-date\n", path);
	}
	return result;
}



template <typename A>
class LinkEditOptimizer
{
public:
											LinkEditOptimizer(const MachOLayoutAbstraction&, const SharedCache<A>&, uint8_t*, StringPool&);
	virtual									~LinkEditOptimizer() {}

		void								copyBindInfo(uint32_t&);
		void								copyWeakBindInfo(uint32_t&);
		void								copyLazyBindInfo(uint32_t&);
		void								copyExportInfo(uint32_t&);
		void								copyLocalSymbols(uint32_t symbolTableOffset, uint32_t&, bool dontMapLocalSymbols,
															uint8_t* cacheStart, StringPool& unmappedLocalsStringPool, 
															std::vector<macho_nlist<typename A::P> >& unmappedSymbols,
															std::vector<LocalSymbolInfo>& info);
		void								copyExportedSymbols(uint32_t symbolTableOffset, uint32_t&);
		void								copyImportedSymbols(uint32_t symbolTableOffset, uint32_t&);
		void								copyExternalRelocations(uint32_t& offset);
		void								copyIndirectSymbolTable(uint32_t& offset);
		void								copyFunctionStarts(uint32_t& offset);
		void								copyDataInCode(uint32_t& offset);
		void								updateLoadCommands(uint64_t newVMAddress, uint64_t size, uint32_t stringPoolOffset, 
																uint32_t linkEditsFileOffset, bool keepSignatures);
	

protected:
	typedef typename A::P					P;
	typedef typename A::P::E				E;
	typedef typename A::P::uint_t			pint_t;
			
private:

	const SharedCache<A>&						fSharedCache;
	const macho_header<P>*						fHeader; 
	uint8_t*									fNewLinkEditStart;	
	uint8_t*									fLinkEditBase;		
	const MachOLayoutAbstraction&				fLayout;
	macho_dyld_info_command<P>*					fDyldInfo;
	macho_dysymtab_command<P>*					fDynamicSymbolTable;
	macho_linkedit_data_command<P>*				fFunctionStarts;
	macho_linkedit_data_command<P>*				fDataInCode;
	macho_symtab_command<P>*					fSymbolTableLoadCommand;
	const macho_nlist<P>*						fSymbolTable;
	const char*									fStrings;
	StringPool&									fNewStringPool;
	std::map<uint32_t,uint32_t>					fOldToNewSymbolIndexes;
	uint32_t									fBindInfoOffsetIntoNewLinkEdit;
	uint32_t									fBindInfoSizeInNewLinkEdit;
	uint32_t									fWeakBindInfoOffsetIntoNewLinkEdit;
	uint32_t									fWeakBindInfoSizeInNewLinkEdit;
	uint32_t									fLazyBindInfoOffsetIntoNewLinkEdit;
	uint32_t									fLazyBindInfoSizeInNewLinkEdit;
	uint32_t									fExportInfoOffsetIntoNewLinkEdit;
	uint32_t									fExportInfoSizeInNewLinkEdit;
	uint32_t									fSymbolTableStartOffsetInNewLinkEdit;
	uint32_t									fLocalSymbolsStartIndexInNewLinkEdit;
	uint32_t									fLocalSymbolsCountInNewLinkEdit;
	uint32_t									fExportedSymbolsStartIndexInNewLinkEdit;
	uint32_t									fExportedSymbolsCountInNewLinkEdit;
	uint32_t									fImportSymbolsStartIndexInNewLinkEdit;
	uint32_t									fImportedSymbolsCountInNewLinkEdit;
	uint32_t									fExternalRelocationsOffsetIntoNewLinkEdit;
	uint32_t									fIndirectSymbolTableOffsetInfoNewLinkEdit;
	uint32_t									fFunctionStartsOffsetInNewLinkEdit;
	uint32_t									fDataInCodeOffsetInNewLinkEdit;
	uint32_t									fUnmappedLocalSymbolsStartIndexInNewLinkEdit;
	uint32_t									fUnmappedLocalSymbolsCountInNewLinkEdit;
};



template <typename A>
LinkEditOptimizer<A>::LinkEditOptimizer(const MachOLayoutAbstraction& layout, const SharedCache<A>& sharedCache, uint8_t* newLinkEdit, StringPool& stringPool)
 : 	fSharedCache(sharedCache), fLayout(layout), fLinkEditBase(NULL), fNewLinkEditStart(newLinkEdit), fDyldInfo(NULL),
	fDynamicSymbolTable(NULL), fFunctionStarts(NULL), fDataInCode(NULL), 
	fSymbolTableLoadCommand(NULL), fSymbolTable(NULL), fStrings(NULL), fNewStringPool(stringPool),
	fBindInfoOffsetIntoNewLinkEdit(0), fBindInfoSizeInNewLinkEdit(0),
	fWeakBindInfoOffsetIntoNewLinkEdit(0), fWeakBindInfoSizeInNewLinkEdit(0),
	fLazyBindInfoOffsetIntoNewLinkEdit(0), fLazyBindInfoSizeInNewLinkEdit(0),
	fExportInfoOffsetIntoNewLinkEdit(0), fExportInfoSizeInNewLinkEdit(0),
	fSymbolTableStartOffsetInNewLinkEdit(0), 
	fLocalSymbolsStartIndexInNewLinkEdit(0), fLocalSymbolsCountInNewLinkEdit(0),
	fExportedSymbolsStartIndexInNewLinkEdit(0), fExportedSymbolsCountInNewLinkEdit(0),
	fImportSymbolsStartIndexInNewLinkEdit(0), fImportedSymbolsCountInNewLinkEdit(0),
	fExternalRelocationsOffsetIntoNewLinkEdit(0), fIndirectSymbolTableOffsetInfoNewLinkEdit(0),
	fFunctionStartsOffsetInNewLinkEdit(0), fDataInCodeOffsetInNewLinkEdit(0),
	fUnmappedLocalSymbolsStartIndexInNewLinkEdit(0), fUnmappedLocalSymbolsCountInNewLinkEdit(0)
	
{
	fHeader = (const macho_header<P>*)fLayout.getSegments()[0].mappedAddress();

	const std::vector<MachOLayoutAbstraction::Segment>& segments = fLayout.getSegments();
	for(std::vector<MachOLayoutAbstraction::Segment>::const_iterator it = segments.begin(); it != segments.end(); ++it) {
		const MachOLayoutAbstraction::Segment& seg = *it;
		if ( strcmp(seg.name(), "__LINKEDIT") == 0 ) 
			fLinkEditBase = (uint8_t*)seg.mappedAddress() - seg.fileOffset();
	}
	if ( fLinkEditBase == NULL )	
		throw "no __LINKEDIT segment";

	const macho_load_command<P>* const cmds = (macho_load_command<P>*)((uint8_t*)fHeader + sizeof(macho_header<P>));
	const uint32_t cmd_count = fHeader->ncmds();
	const macho_load_command<P>* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd()) {
			case LC_SYMTAB:
				{
					fSymbolTableLoadCommand = (macho_symtab_command<P>*)cmd;
					fSymbolTable = (macho_nlist<P>*)(&fLinkEditBase[fSymbolTableLoadCommand->symoff()]);
					fStrings = (char*)&fLinkEditBase[fSymbolTableLoadCommand->stroff()];
				}
				break;
			case LC_DYSYMTAB:
				fDynamicSymbolTable = (macho_dysymtab_command<P>*)cmd;
				break;
			case LC_DYLD_INFO:
			case LC_DYLD_INFO_ONLY:
				fDyldInfo = (macho_dyld_info_command<P>*)cmd;
				break;
			case LC_FUNCTION_STARTS:
				fFunctionStarts = (macho_linkedit_data_command<P>*)cmd;
			case LC_DATA_IN_CODE:
				fDataInCode = (macho_linkedit_data_command<P>*)cmd;
				break;
		}
		cmd = (const macho_load_command<P>*)(((uint8_t*)cmd)+cmd->cmdsize());
	}	
	if ( fSymbolTable == NULL )	
		throw "no LC_SYMTAB";
	if ( fDynamicSymbolTable == NULL )	
		throw "no LC_DYSYMTAB";
	
}


template <typename A>
class SymbolSorter
{
public:
	typedef typename A::P P;
	SymbolSorter(const StringPool& pool) : fStringPool(pool) {}
	bool operator()(const macho_nlist<P>& left, const macho_nlist<P>& right) { 
		return (strcmp(fStringPool.stringAtIndex(left.n_strx()) , fStringPool.stringAtIndex(right.n_strx())) < 0); 
	} 
	
private:
	const StringPool& fStringPool;
};


template <typename A>
void LinkEditOptimizer<A>::copyBindInfo(uint32_t& offset)
{
	if ( (fDyldInfo != NULL) && (fDyldInfo->bind_off() != 0) ) {
		fBindInfoOffsetIntoNewLinkEdit = offset;
		fBindInfoSizeInNewLinkEdit = fDyldInfo->bind_size();
		memcpy(fNewLinkEditStart+offset, &fLinkEditBase[fDyldInfo->bind_off()], fDyldInfo->bind_size());
		offset += fDyldInfo->bind_size();
	}
}

template <typename A>
void LinkEditOptimizer<A>::copyWeakBindInfo(uint32_t& offset)
{
	if ( (fDyldInfo != NULL) && (fDyldInfo->weak_bind_off() != 0) ) {
		fWeakBindInfoOffsetIntoNewLinkEdit = offset;
		fWeakBindInfoSizeInNewLinkEdit = fDyldInfo->weak_bind_size();
		memcpy(fNewLinkEditStart+offset, &fLinkEditBase[fDyldInfo->weak_bind_off()], fDyldInfo->weak_bind_size());
		offset += fDyldInfo->weak_bind_size();
	}
}

template <typename A>
void LinkEditOptimizer<A>::copyLazyBindInfo(uint32_t& offset)
{
	if ( (fDyldInfo != NULL) && (fDyldInfo->lazy_bind_off() != 0) ) {
		fLazyBindInfoOffsetIntoNewLinkEdit = offset;
		fLazyBindInfoSizeInNewLinkEdit = fDyldInfo->lazy_bind_size();
		memcpy(fNewLinkEditStart+offset, &fLinkEditBase[fDyldInfo->lazy_bind_off()], fDyldInfo->lazy_bind_size());
		offset += fDyldInfo->lazy_bind_size();
	}
}

template <typename A>
void LinkEditOptimizer<A>::copyExportInfo(uint32_t& offset)
{
	if ( (fDyldInfo != NULL) && (fLayout.getDyldInfoExports() != NULL) ) {
		fExportInfoOffsetIntoNewLinkEdit = offset;
		fExportInfoSizeInNewLinkEdit = fDyldInfo->export_size();
		memcpy(fNewLinkEditStart+offset, fLayout.getDyldInfoExports(), fDyldInfo->export_size());
		offset += fDyldInfo->export_size();
	}
}


template <typename A>
void LinkEditOptimizer<A>::copyLocalSymbols(uint32_t symbolTableOffset, uint32_t& symbolIndex, bool dontMapLocalSymbols, uint8_t* cacheStart, 
											StringPool&	unmappedLocalsStringPool, std::vector<macho_nlist<P> >& unmappedSymbols,
											std::vector<LocalSymbolInfo>& dylibInfos)
{
	fLocalSymbolsStartIndexInNewLinkEdit = symbolIndex;
	LocalSymbolInfo localInfo;
	localInfo.dylibOffset = ((uint8_t*)fHeader) - cacheStart;
	localInfo.nlistStartIndex = unmappedSymbols.size();
	localInfo.nlistCount = 0;
	fSymbolTableStartOffsetInNewLinkEdit = symbolTableOffset + symbolIndex*sizeof(macho_nlist<P>);
	macho_nlist<P>* const newSymbolTableStart = (macho_nlist<P>*)(fNewLinkEditStart+symbolTableOffset);
	const macho_nlist<P>* const firstLocal = &fSymbolTable[fDynamicSymbolTable->ilocalsym()];
	const macho_nlist<P>* const lastLocal  = &fSymbolTable[fDynamicSymbolTable->ilocalsym()+fDynamicSymbolTable->nlocalsym()];
	uint32_t oldIndex = fDynamicSymbolTable->ilocalsym();
	for (const macho_nlist<P>* entry = firstLocal; entry < lastLocal; ++entry, ++oldIndex) {
		// <rdar://problem/12237639> don't copy stab symbols
		if ( (entry->n_sect() != NO_SECT) && ((entry->n_type() & N_STAB) == 0) ) {
			const char* name = &fStrings[entry->n_strx()];
			macho_nlist<P>* newSymbolEntry = &newSymbolTableStart[symbolIndex];
			*newSymbolEntry = *entry;
			if ( dontMapLocalSymbols ) {
				// if local in __text, add <redacted> symbol name to shared cache so backtraces don't have bogus names
				if ( entry->n_sect() == 1 ) {
					newSymbolEntry->set_n_strx(fNewStringPool.addUnique("<redacted>"));
					++symbolIndex;
				}
				// copy local symbol to unmmapped locals area
				unmappedSymbols.push_back(*entry);			
				unmappedSymbols.back().set_n_strx(unmappedLocalsStringPool.addUnique(name));
			}
			else {
				newSymbolEntry->set_n_strx(fNewStringPool.addUnique(name));
				++symbolIndex;
			}
		}
	}
	fLocalSymbolsCountInNewLinkEdit = symbolIndex - fLocalSymbolsStartIndexInNewLinkEdit;
	localInfo.nlistCount = unmappedSymbols.size() - localInfo.nlistStartIndex;
	dylibInfos.push_back(localInfo);
	//fprintf(stderr, "%u locals starting at %u for %s\n", fLocalSymbolsCountInNewLinkEdit, fLocalSymbolsStartIndexInNewLinkEdit, fLayout.getFilePath());
}


template <typename A>
void LinkEditOptimizer<A>::copyExportedSymbols(uint32_t symbolTableOffset, uint32_t& symbolIndex)
{
	fExportedSymbolsStartIndexInNewLinkEdit = symbolIndex;
	macho_nlist<P>* const newSymbolTableStart = (macho_nlist<P>*)(fNewLinkEditStart+symbolTableOffset);
	const macho_nlist<P>* const firstExport = &fSymbolTable[fDynamicSymbolTable->iextdefsym()];
	const macho_nlist<P>* const lastExport  = &fSymbolTable[fDynamicSymbolTable->iextdefsym()+fDynamicSymbolTable->nextdefsym()];
	uint32_t oldIndex = fDynamicSymbolTable->iextdefsym();
	for (const macho_nlist<P>* entry = firstExport; entry < lastExport; ++entry, ++oldIndex) {
		if ( ((entry->n_type() & N_TYPE) == N_SECT) && (strncmp(&fStrings[entry->n_strx()], ".objc_", 6) != 0)
						&& (strncmp(&fStrings[entry->n_strx()], "$ld$", 4) != 0) ) {
			macho_nlist<P>* newSymbolEntry = &newSymbolTableStart[symbolIndex];
			*newSymbolEntry = *entry;
			newSymbolEntry->set_n_strx(fNewStringPool.addUnique(&fStrings[entry->n_strx()]));
			fOldToNewSymbolIndexes[oldIndex] = symbolIndex-fLocalSymbolsStartIndexInNewLinkEdit;
			++symbolIndex;
		}
	}
	fExportedSymbolsCountInNewLinkEdit = symbolIndex - fExportedSymbolsStartIndexInNewLinkEdit;
	//fprintf(stderr, "%u exports starting at %u for %s\n", fExportedSymbolsCountInNewLinkEdit, fExportedSymbolsStartIndexInNewLinkEdit, fLayout.getFilePath());
	// sort by name, so that dyld does not need a toc
	macho_nlist<P>* newSymbolsStart = &newSymbolTableStart[fExportedSymbolsStartIndexInNewLinkEdit];
	macho_nlist<P>* newSymbolsEnd = &newSymbolTableStart[fExportedSymbolsStartIndexInNewLinkEdit+fExportedSymbolsCountInNewLinkEdit];
	std::sort(newSymbolsStart, newSymbolsEnd, SymbolSorter<A>(fNewStringPool));
	//for (macho_nlist<P>* entry = newSymbolsStart; entry < newSymbolsEnd; ++entry)
	//	fprintf(stderr, "\t%u\t %s\n", (entry-newSymbolsStart)+fExportedSymbolsStartIndexInNewLinkEdit, fNewStringPool.stringAtIndex(entry->n_strx()));
}


template <typename A>
void LinkEditOptimizer<A>::copyImportedSymbols(uint32_t symbolTableOffset, uint32_t& symbolIndex)
{
	fImportSymbolsStartIndexInNewLinkEdit = symbolIndex;
	macho_nlist<P>* const newSymbolTableStart = (macho_nlist<P>*)(fNewLinkEditStart+symbolTableOffset);
	const macho_nlist<P>* const firstImport = &fSymbolTable[fDynamicSymbolTable->iundefsym()];
	const macho_nlist<P>* const lastImport  = &fSymbolTable[fDynamicSymbolTable->iundefsym()+fDynamicSymbolTable->nundefsym()];
	uint32_t oldIndex = fDynamicSymbolTable->iundefsym();
	for (const macho_nlist<P>* entry = firstImport; entry < lastImport; ++entry, ++oldIndex) {
		if ( ((entry->n_type() & N_TYPE) == N_UNDF) && (strncmp(&fStrings[entry->n_strx()], ".objc_", 6) != 0) ) {
			macho_nlist<P>* newSymbolEntry = &newSymbolTableStart[symbolIndex];
			*newSymbolEntry = *entry;
			newSymbolEntry->set_n_strx(fNewStringPool.addUnique(&fStrings[entry->n_strx()]));
			fOldToNewSymbolIndexes[oldIndex] = symbolIndex-fLocalSymbolsStartIndexInNewLinkEdit;
			++symbolIndex;
		}
	}
	fImportedSymbolsCountInNewLinkEdit = symbolIndex - fImportSymbolsStartIndexInNewLinkEdit;
	//fprintf(stderr, "%u imports starting at %u for %s\n", fImportedSymbolsCountInNewLinkEdit, fImportSymbolsStartIndexInNewLinkEdit, fLayout.getFilePath());
	//macho_nlist<P>* newSymbolsStart = &((macho_nlist<P>*)fNewLinkEditStart)[fImportSymbolsStartIndexInNewLinkEdit];
	//macho_nlist<P>* newSymbolsEnd = &((macho_nlist<P>*)fNewLinkEditStart)[fImportSymbolsStartIndexInNewLinkEdit+fImportedSymbolsCountInNewLinkEdit];
	//for (macho_nlist<P>* entry = newSymbolsStart; entry < newSymbolsEnd; ++entry)
	//	fprintf(stderr, "\t%u\t%s\n", (entry-newSymbolsStart)+fImportSymbolsStartIndexInNewLinkEdit, fNewStringPool.stringAtIndex(entry->n_strx()));
}


template <typename A>
void LinkEditOptimizer<A>::copyExternalRelocations(uint32_t& offset)
{
	fExternalRelocationsOffsetIntoNewLinkEdit = offset;
	const macho_relocation_info<P>* const relocsStart = (macho_relocation_info<P>*)(&fLinkEditBase[fDynamicSymbolTable->extreloff()]);
	const macho_relocation_info<P>* const relocsEnd = &relocsStart[fDynamicSymbolTable->nextrel()];
	for (const macho_relocation_info<P>* reloc=relocsStart; reloc < relocsEnd; ++reloc) {
		macho_relocation_info<P>* newReloc = (macho_relocation_info<P>*)(&fNewLinkEditStart[offset]);
		*newReloc = *reloc;
		uint32_t newSymbolIndex = fOldToNewSymbolIndexes[reloc->r_symbolnum()];
		//fprintf(stderr, "copyExternalRelocations() old=%d, new=%u name=%s in %s\n", reloc->r_symbolnum(), newSymbolIndex,
		//	 &fStrings[fSymbolTable[reloc->r_symbolnum()].n_strx()], fLayout.getFilePath());
		newReloc->set_r_symbolnum(newSymbolIndex);
		offset += sizeof(macho_relocation_info<P>);
	}
}

template <typename A>
void LinkEditOptimizer<A>::copyFunctionStarts(uint32_t& offset)
{	
	if ( fFunctionStarts != NULL ) {
		fFunctionStartsOffsetInNewLinkEdit = offset;
		memcpy(&fNewLinkEditStart[offset], &fLinkEditBase[fFunctionStarts->dataoff()], fFunctionStarts->datasize());
		offset += fFunctionStarts->datasize();
	}
}

template <typename A>
void LinkEditOptimizer<A>::copyDataInCode(uint32_t& offset)
{	
	if ( fDataInCode != NULL ) {
		fDataInCodeOffsetInNewLinkEdit = offset;
		memcpy(&fNewLinkEditStart[offset], &fLinkEditBase[fDataInCode->dataoff()], fDataInCode->datasize());
		offset += fDataInCode->datasize();
	}
}


template <typename A>
void LinkEditOptimizer<A>::copyIndirectSymbolTable(uint32_t& offset)
{	
	fIndirectSymbolTableOffsetInfoNewLinkEdit = offset;
	const uint32_t* const indirectTable = (uint32_t*)&this->fLinkEditBase[fDynamicSymbolTable->indirectsymoff()];
	uint32_t* newIndirectTable = (uint32_t*)&fNewLinkEditStart[offset];
	for (int i=0; i < fDynamicSymbolTable->nindirectsyms(); ++i) {
		uint32_t oldSymbolIndex = E::get32(indirectTable[i]); 
		uint32_t newSymbolIndex = oldSymbolIndex;
		if ( (oldSymbolIndex != INDIRECT_SYMBOL_ABS) && (oldSymbolIndex != INDIRECT_SYMBOL_LOCAL) ) {
			newSymbolIndex = fOldToNewSymbolIndexes[oldSymbolIndex];
			//fprintf(stderr, "copyIndirectSymbolTable() old=%d, new=%u name=%s in %s\n", oldSymbolIndex, newSymbolIndex,
			// &fStrings[fSymbolTable[oldSymbolIndex].n_strx()], fLayout.getFilePath());
		}
		E::set32(newIndirectTable[i], newSymbolIndex);
	}
	offset += (fDynamicSymbolTable->nindirectsyms() * 4);
}

template <typename A>
void LinkEditOptimizer<A>::updateLoadCommands(uint64_t newVMAddress, uint64_t size, uint32_t stringPoolOffset, 
												uint32_t linkEditsFileOffset, bool keepSignatures)
{
	// set LINKEDIT segment commmand to new merged LINKEDIT
	const macho_load_command<P>* const cmds = (macho_load_command<P>*)((uint8_t*)fHeader + sizeof(macho_header<P>));
	const uint32_t cmd_count = fHeader->ncmds();
	const macho_load_command<P>* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		if ( cmd->cmd() == macho_segment_command<P>::CMD ) {
			macho_segment_command<P>* seg = (macho_segment_command<P>*)cmd;
			if ( strcmp(seg->segname(), "__LINKEDIT") == 0 ) {
				seg->set_vmaddr(newVMAddress);
				seg->set_vmsize(size);
				seg->set_filesize(size);
				seg->set_fileoff(linkEditsFileOffset);
			}
			// don't alter __TEXT until <rdar://problem/7022345> is fixed
			else if ( strcmp(seg->segname(), "__TEXT") != 0 ) {
				// update all other segments fileoff to be offset from start of cache file
				pint_t oldFileOff = seg->fileoff();
				seg->set_fileoff(fSharedCache.cacheFileOffsetForVMAddress(seg->vmaddr()));
				pint_t fileOffsetDelta = seg->fileoff() - oldFileOff;
				// update all sections in this segment
				macho_section<P>* const sectionsStart = (macho_section<P>*)((char*)seg + sizeof(macho_segment_command<P>));
				macho_section<P>* const sectionsEnd = &sectionsStart[seg->nsects()];
				for(macho_section<P>* sect = sectionsStart; sect < sectionsEnd; ++sect) {
					if ( sect->offset() != 0 )
						sect->set_offset(sect->offset()+fileOffsetDelta);
				}
			}
		}
		cmd = (const macho_load_command<P>*)(((uint8_t*)cmd)+cmd->cmdsize());
	}	
	
	// update dyld_info with new offsets
	if ( fDyldInfo != NULL ) {
		fDyldInfo->set_rebase_off(0);
		fDyldInfo->set_rebase_size(0);
		fDyldInfo->set_bind_off(linkEditsFileOffset+fBindInfoOffsetIntoNewLinkEdit);
		fDyldInfo->set_bind_size(fBindInfoSizeInNewLinkEdit);
		fDyldInfo->set_weak_bind_off(linkEditsFileOffset+fWeakBindInfoOffsetIntoNewLinkEdit);
		fDyldInfo->set_weak_bind_size(fWeakBindInfoSizeInNewLinkEdit);
		fDyldInfo->set_lazy_bind_off(linkEditsFileOffset+fLazyBindInfoOffsetIntoNewLinkEdit);
		fDyldInfo->set_lazy_bind_size(fLazyBindInfoSizeInNewLinkEdit);
		fDyldInfo->set_export_off(linkEditsFileOffset+fExportInfoOffsetIntoNewLinkEdit);
		fDyldInfo->set_export_size(fExportInfoSizeInNewLinkEdit);
		
//		fprintf(stderr, "dylib %s\n", fLayout.getFilePath());
//		fprintf(stderr, "  bind_off=0x%08X\n", fDyldInfo->bind_off());
//		fprintf(stderr, "  export_off=0x%08X\n", fDyldInfo->export_off());
//		fprintf(stderr, "  export_size=%d\n", fDyldInfo->export_size());
		
	}	
	
	// update symbol table and dynamic symbol table with new offsets
	fSymbolTableLoadCommand->set_symoff(linkEditsFileOffset+fSymbolTableStartOffsetInNewLinkEdit);
	fSymbolTableLoadCommand->set_nsyms(fLocalSymbolsCountInNewLinkEdit+fExportedSymbolsCountInNewLinkEdit+fImportedSymbolsCountInNewLinkEdit);
	fSymbolTableLoadCommand->set_stroff(linkEditsFileOffset+stringPoolOffset);
	fSymbolTableLoadCommand->set_strsize(fNewStringPool.size());
	fDynamicSymbolTable->set_ilocalsym(0);
	fDynamicSymbolTable->set_nlocalsym(fLocalSymbolsCountInNewLinkEdit);
	fDynamicSymbolTable->set_iextdefsym(fExportedSymbolsStartIndexInNewLinkEdit-fLocalSymbolsStartIndexInNewLinkEdit);
	fDynamicSymbolTable->set_nextdefsym(fExportedSymbolsCountInNewLinkEdit);
	fDynamicSymbolTable->set_iundefsym(fImportSymbolsStartIndexInNewLinkEdit-fLocalSymbolsStartIndexInNewLinkEdit);
	fDynamicSymbolTable->set_nundefsym(fImportedSymbolsCountInNewLinkEdit);
	fDynamicSymbolTable->set_tocoff(0);
	fDynamicSymbolTable->set_ntoc(0);
	fDynamicSymbolTable->set_modtaboff(0);
	fDynamicSymbolTable->set_nmodtab(0);
	fDynamicSymbolTable->set_indirectsymoff(linkEditsFileOffset+fIndirectSymbolTableOffsetInfoNewLinkEdit);
	fDynamicSymbolTable->set_extreloff(linkEditsFileOffset+fExternalRelocationsOffsetIntoNewLinkEdit);
	fDynamicSymbolTable->set_locreloff(0);
	fDynamicSymbolTable->set_nlocrel(0);

	// update function starts
	if ( fFunctionStarts != NULL ) {
		fFunctionStarts->set_dataoff(linkEditsFileOffset+fFunctionStartsOffsetInNewLinkEdit);
	}
	// update data-in-code info
	if ( fDataInCode != NULL ) {
		fDataInCode->set_dataoff(linkEditsFileOffset+fDataInCodeOffsetInNewLinkEdit);
	}
	
	// now remove load commands no longer needed
	const macho_load_command<P>* srcCmd = cmds;
	macho_load_command<P>* dstCmd = (macho_load_command<P>*)cmds;
	int32_t newCount = 0;
	for (uint32_t i = 0; i < cmd_count; ++i) {	
		uint32_t cmdSize = srcCmd->cmdsize();
		switch ( srcCmd->cmd() ) {
			case LC_SEGMENT_SPLIT_INFO:
			case LC_DYLIB_CODE_SIGN_DRS:
				// don't copy
				break;
			case LC_CODE_SIGNATURE:
				if ( !keepSignatures )
					break;
				// otherwise fall into copy case
			default:
				memmove(dstCmd, srcCmd, cmdSize);
				dstCmd = (macho_load_command<P>*)(((uint8_t*)dstCmd)+cmdSize);
				++newCount;
				break;
		}
		srcCmd = (const macho_load_command<P>*)(((uint8_t*)srcCmd)+cmdSize);
	}
	// zero out stuff removed
	bzero(dstCmd, (uint8_t*)srcCmd - (uint8_t*)dstCmd);
	
	// update mach_header
	macho_header<P>* writableHeader = (macho_header<P>*)fHeader; 
	writableHeader->set_ncmds(newCount);
	writableHeader->set_sizeofcmds((uint8_t*)dstCmd - ((uint8_t*)fHeader + sizeof(macho_header<P>)));
	
	// this invalidates some ivars
	fDynamicSymbolTable = NULL;
	fSymbolTableLoadCommand = NULL;
	fDyldInfo = NULL;
	fSymbolTable = NULL;
	fStrings = NULL;
}



template <typename A>
uint8_t* SharedCache<A>::optimizeLINKEDIT(bool keepSignatures, bool dontMapLocalSymbols)
{
	// allocate space for optimized LINKEDIT area
	uint8_t* newLinkEdit = new uint8_t[fLinkEditsTotalUnoptimizedSize];
	bzero(newLinkEdit, fLinkEditsTotalUnoptimizedSize);
	
	// make a string pool 
	StringPool stringPool;
	
	// create optimizer object for each LINKEDIT segment
	std::vector<LinkEditOptimizer<A>*> optimizers;
	for(typename std::vector<LayoutInfo>::const_iterator it = fDylibs.begin(); it != fDylibs.end(); ++it) {
		optimizers.push_back(new LinkEditOptimizer<A>(*it->layout, *this, newLinkEdit, stringPool));
	}

	// rebase info is not copied because images in shared cache are never rebased
	
	// copy weak bind info
	uint32_t offset = 0;
	fOffsetOfWeakBindInfoInCombinedLinkedit = offset;
	for(typename std::vector<LinkEditOptimizer<A>*>::iterator it = optimizers.begin(); it != optimizers.end(); ++it) {
		(*it)->copyWeakBindInfo(offset);
	}
	
	// copy export info
	fOffsetOfExportInfoInCombinedLinkedit = offset;
	for(typename std::vector<LinkEditOptimizer<A>*>::iterator it = optimizers.begin(); it != optimizers.end(); ++it) {
		(*it)->copyExportInfo(offset);
	}

	// copy bind info
	fOffsetOfBindInfoInCombinedLinkedit = offset;
	for(typename std::vector<LinkEditOptimizer<A>*>::iterator it = optimizers.begin(); it != optimizers.end(); ++it) {
		(*it)->copyBindInfo(offset);
	}
	
	// copy lazy bind info
	fOffsetOfLazyBindInfoInCombinedLinkedit = offset;
	for(typename std::vector<LinkEditOptimizer<A>*>::iterator it = optimizers.begin(); it != optimizers.end(); ++it) {
		(*it)->copyLazyBindInfo(offset);
	}

	// copy symbol table entries
	fOffsetOfOldSymbolTableInfoInCombinedLinkedit = offset;
	uint32_t symbolTableOffset = offset;
	uint32_t symbolTableIndex = 0;
	if ( dontMapLocalSymbols ) 
		fUnmappedLocalSymbols.reserve(16384);
	for(typename std::vector<LinkEditOptimizer<A>*>::iterator it = optimizers.begin(); it != optimizers.end(); ++it) {
		(*it)->copyLocalSymbols(symbolTableOffset, symbolTableIndex, dontMapLocalSymbols, fInMemoryCache,
								fUnmappedLocalsStringPool, fUnmappedLocalSymbols, fLocalSymbolInfos);
		(*it)->copyExportedSymbols(symbolTableOffset, symbolTableIndex);
		(*it)->copyImportedSymbols(symbolTableOffset, symbolTableIndex);
	}
	fSizeOfOldSymbolTableInfoInCombinedLinkedit =  symbolTableIndex * sizeof(macho_nlist<typename A::P>);
	offset = symbolTableOffset + fSizeOfOldSymbolTableInfoInCombinedLinkedit & (-8);
	
	// copy external relocations, 8-byte aligned after end of symbol table
	fOffsetOfOldExternalRelocationsInCombinedLinkedit = offset;
	for(typename std::vector<LinkEditOptimizer<A>*>::iterator it = optimizers.begin(); it != optimizers.end(); ++it) {
		(*it)->copyExternalRelocations(offset);
	}
	fSizeOfOldExternalRelocationsInCombinedLinkedit = offset - fOffsetOfOldExternalRelocationsInCombinedLinkedit;
	
	// copy function starts
	fOffsetOfFunctionStartsInCombinedLinkedit = offset;
	for(typename std::vector<LinkEditOptimizer<A>*>::iterator it = optimizers.begin(); it != optimizers.end(); ++it) {
		(*it)->copyFunctionStarts(offset);
	}
	fSizeOfFunctionStartsInCombinedLinkedit = offset - fOffsetOfFunctionStartsInCombinedLinkedit;

	// copy data-in-code info
	fOffsetOfDataInCodeInCombinedLinkedit = offset;
	for(typename std::vector<LinkEditOptimizer<A>*>::iterator it = optimizers.begin(); it != optimizers.end(); ++it) {
		(*it)->copyDataInCode(offset);
	}
	fSizeOfDataInCodeInCombinedLinkedit = offset - fOffsetOfDataInCodeInCombinedLinkedit;

	// copy indirect symbol tables
	fOffsetOfOldIndirectSymbolsInCombinedLinkedit = offset;
	for(typename std::vector<LinkEditOptimizer<A>*>::iterator it = optimizers.begin(); it != optimizers.end(); ++it) {
		(*it)->copyIndirectSymbolTable(offset);
	}
	fSizeOfOldIndirectSymbolsInCombinedLinkedit = offset - fOffsetOfOldIndirectSymbolsInCombinedLinkedit;
		
	// copy string pool
	fOffsetOfOldStringPoolInCombinedLinkedit = offset;
	memcpy(&newLinkEdit[offset], stringPool.getBuffer(), stringPool.size());
	fSizeOfOldStringPoolInCombinedLinkedit = stringPool.size();
	
	// total new size round up to page size
	fLinkEditsTotalOptimizedSize = pageAlign(fOffsetOfOldStringPoolInCombinedLinkedit + fSizeOfOldStringPoolInCombinedLinkedit);
	
	// choose new linkedit file offset 
	uint32_t linkEditsFileOffset = cacheFileOffsetForVMAddress(fLinkEditsStartAddress);
//	uint32_t linkEditsFileOffset = fLinkEditsStartAddress - sharedRegionStartAddress();	

	// update load commands so that all dylibs shared different areas of the same LINKEDIT segment
	for(typename std::vector<LinkEditOptimizer<A>*>::iterator it = optimizers.begin(); it != optimizers.end(); ++it) {
		(*it)->updateLoadCommands(fLinkEditsStartAddress, fLinkEditsTotalUnoptimizedSize, fOffsetOfOldStringPoolInCombinedLinkedit, linkEditsFileOffset, keepSignatures);
	}

	//fprintf(stderr, "fLinkEditsTotalUnoptimizedSize=%llu, fLinkEditsTotalOptimizedSize=%u\n", fLinkEditsTotalUnoptimizedSize, fLinkEditsTotalOptimizedSize);
	//printf(stderr, "mega link edit mapped starting at: %p\n", fFirstLinkEditSegment->mappedAddress());

	// overwrite mapped LINKEDIT area with new optimized LINKEDIT segment
	memcpy(fFirstLinkEditSegment->mappedAddress(), newLinkEdit, fLinkEditsTotalUnoptimizedSize);
	
	// update all LINKEDIT Segment objects to point to same merged LINKEDIT area
	for(typename std::vector<LayoutInfo>::iterator it = fDylibs.begin(); it != fDylibs.end(); ++it) {
		std::vector<MachOLayoutAbstraction::Segment>& segs = ((MachOLayoutAbstraction*)(it->layout))->getSegments();
		for(int i=0; i < segs.size(); ++i) {
			MachOLayoutAbstraction::Segment& seg = segs[i];
			if ( !seg.writable() && !seg.executable() && (strcmp(seg.name(), "__LINKEDIT") == 0) ) {
				seg.setNewAddress(fLinkEditsStartAddress);
				seg.setMappedAddress(fFirstLinkEditSegment->mappedAddress());
				seg.setSize(fLinkEditsTotalOptimizedSize);
				seg.setFileSize(fLinkEditsTotalOptimizedSize);
				seg.setFileOffset(linkEditsFileOffset);
			}
		}
	}
	
	// return new end of cache
	return (uint8_t*)fFirstLinkEditSegment->mappedAddress() + fLinkEditsTotalOptimizedSize;
}


template <typename A>
class ObjCSelectorUniquer
{
private:
    objc_opt::string_map fSelectorStrings;
    SharedCache<A> *fCache;
    size_t fCount;

public:

    ObjCSelectorUniquer(SharedCache<A> *newCache)
        : fSelectorStrings()
        , fCache(newCache)
        , fCount(0)
    { }

    typename A::P::uint_t visit(typename A::P::uint_t oldValue) 
    {
        fCount++;
        const char *s = (const char *)
            fCache->mappedAddressForVMAddress(oldValue);
        objc_opt::string_map::iterator element = 
            fSelectorStrings.insert(objc_opt::string_map::value_type(s, oldValue)).first;
        return (typename A::P::uint_t)element->second;
    }

    objc_opt::string_map& strings() { 
        return fSelectorStrings;
    }

    size_t count() const { return fCount; }
};


template <typename A>
class ClassListBuilder
{
private:
    typedef typename A::P P;

    objc_opt::string_map fClassNames;
    objc_opt::class_map fClasses;
    size_t fCount;
    HeaderInfoOptimizer<A>& fHinfos;

public:

    ClassListBuilder(HeaderInfoOptimizer<A>& hinfos)
        : fClassNames()
        , fClasses()
        , fCount(0)
        , fHinfos(hinfos)
    { }

    void visitClass(SharedCache<A>* cache, 
                    const macho_header<P>* header,
                    objc_class_t<A>* cls) 
    {
        if (cls->isMetaClass(cache)) return;

        const char *name = cls->getName(cache);
        uint64_t name_vmaddr = cache->VMAddressForMappedAddress(name);
        uint64_t cls_vmaddr = cache->VMAddressForMappedAddress(cls);
        uint64_t hinfo_vmaddr = cache->VMAddressForMappedAddress(fHinfos.hinfoForHeader(cache, header));
        fClassNames.insert(objc_opt::string_map::value_type(name, name_vmaddr));
        fClasses.insert(objc_opt::class_map::value_type(name, std::pair<uint64_t, uint64_t>(cls_vmaddr, hinfo_vmaddr)));
        fCount++;
    }

    objc_opt::string_map& classNames() { 
        return fClassNames;
    }

    objc_opt::class_map& classes() { 
        return fClasses;
    }

    size_t count() const { return fCount; }
};


static int percent(size_t num, size_t denom) {
    if (denom) return (int)(num / (double)denom * 100);
    else return 100;
}

template <typename A>
void SharedCache<A>::optimizeObjC(std::vector<void*>& pointersInData)
{
    const char *err;

    if ( verbose ) {
        fprintf(stderr, "update_dyld_shared_cache: for %s, optimizing objc metadata\n", archName());
    }

    size_t headerSize = P::round_up(sizeof(objc_opt::objc_opt_t));
    if (headerSize != sizeof(objc_opt::objc_opt_t)) {
		warn(archName(), "libobjc's optimization structure size is wrong (metadata not optimized)");
    }

    // Find libobjc's empty sections to fill in
    const macho_section<P> *optROSection = NULL;
    const macho_section<P> *optRWSection = NULL;
	for(typename std::vector<LayoutInfo>::const_iterator it = fDylibs.begin(); it != fDylibs.end(); ++it) {
        if ( strstr(it->layout->getFilePath(), "libobjc") != NULL ) {
			const macho_header<P>* mh = (const macho_header<P>*)(*it->layout).getSegments()[0].mappedAddress();
			optROSection = mh->getSection("__TEXT", "__objc_opt_ro");
			optRWSection = mh->getSection("__DATA", "__objc_opt_rw");
			break;
		}
	}
    
	if ( optROSection == NULL ) {
		warn(archName(), "libobjc's read-only section missing (metadata not optimized)");
		return;
	}
	
	if ( optRWSection == NULL ) {
		warn(archName(), "libobjc's read/write section missing (metadata not optimized)");
		return;
	}

	uint8_t* optROData = (uint8_t*)mappedAddressForVMAddress(optROSection->addr());
    size_t optRORemaining = optROSection->size();

	uint8_t* optRWData = (uint8_t*)mappedAddressForVMAddress(optRWSection->addr());
    size_t optRWRemaining = optRWSection->size();
	
	if (optRORemaining < headerSize) {
		warn(archName(), "libobjc's read-only section is too small (metadata not optimized)");
		return;
	}
	objc_opt::objc_opt_t* optROHeader = (objc_opt::objc_opt_t *)optROData;
    optROData += headerSize;
    optRORemaining -= headerSize;

	if (E::get32(optROHeader->version) != objc_opt::VERSION) {
		warn(archName(), "libobjc's read-only section version is unrecognized (metadata not optimized)");
		return;
	}

    // Write nothing to optROHeader until everything else is written.
    // If something fails below, libobjc will not use the section.

    // Find objc-containing dylibs
    std::vector<LayoutInfo> objcDylibs;
    for(typename std::vector<LayoutInfo>::const_iterator it = fDylibs.begin(); it != fDylibs.end(); ++it) {
        macho_header<P> *mh = (macho_header<P>*)(*it->layout).getSegments()[0].mappedAddress();
        if (mh->getSection("__DATA", "__objc_imageinfo")  ||  mh->getSegment("__OBJC")) {
            objcDylibs.push_back(*it);
        }
    }

    // Build image list

    // This is SAFE: the binaries themselves are unmodified.

    std::vector<LayoutInfo> addressSortedDylibs = objcDylibs;
    std::sort(addressSortedDylibs.begin(), addressSortedDylibs.end(), ByAddressSorter());

    uint64_t hinfoVMAddr = optRWSection->addr() + optRWSection->size() - optRWRemaining;
    HeaderInfoOptimizer<A> hinfoOptimizer;
    err = hinfoOptimizer.init(objcDylibs.size(), optRWData, optRWRemaining);
    if (err) {
		warn(archName(), err);
		return;
    }
    for(typename std::vector<LayoutInfo>::const_iterator it = addressSortedDylibs.begin(); it != addressSortedDylibs.end(); ++it) {
        const macho_header<P> *mh = (const macho_header<P>*)(*it->layout).getSegments()[0].mappedAddress();
        hinfoOptimizer.update(this, mh, pointersInData);
    }


    // Update selector references and build selector list

    // This is SAFE: if we run out of room for the selector table, 
    // the modified binaries are still usable.

    // Heuristic: choose selectors from libraries with more cstring data first.
    // This tries to localize selector cstring memory.
    ObjCSelectorUniquer<A> uniq(this);
    std::vector<LayoutInfo> sizeSortedDylibs = objcDylibs;
    std::sort(sizeSortedDylibs.begin(), sizeSortedDylibs.end(), ByCStringSectionSizeSorter());

    SelectorOptimizer<A, ObjCSelectorUniquer<A> > selOptimizer(uniq);
	for(typename std::vector<LayoutInfo>::const_iterator it = sizeSortedDylibs.begin(); it != sizeSortedDylibs.end(); ++it) {
        const macho_header<P> *mh = (const macho_header<P>*)(*it->layout).getSegments()[0].mappedAddress();
        LegacySelectorUpdater<A, ObjCSelectorUniquer<A> >::update(this, mh, uniq);
        selOptimizer.optimize(this, mh);
	}

    uint64_t seloptVMAddr = optROSection->addr() + optROSection->size() - optRORemaining;
    objc_opt::objc_selopt_t *selopt = new(optROData) objc_opt::objc_selopt_t;
    err = selopt->write(seloptVMAddr, optRORemaining, uniq.strings());
    if (err) {
        warn(archName(), err);
        return;
    }
    optROData += selopt->size();
    optRORemaining -= selopt->size();
    selopt->byteswap(E::little_endian), selopt = NULL;


    // Build class table.

    // This is SAFE: the binaries themselves are unmodified.

    ClassListBuilder<A> classes(hinfoOptimizer);
    ClassWalker< A, ClassListBuilder<A> > classWalker(classes);
	for(typename std::vector<LayoutInfo>::const_iterator it = sizeSortedDylibs.begin(); it != sizeSortedDylibs.end(); ++it) {
        const macho_header<P> *mh = (const macho_header<P>*)(*it->layout).getSegments()[0].mappedAddress();
        classWalker.walk(this, mh);
	}

    uint64_t clsoptVMAddr = optROSection->addr() + optROSection->size() - optRORemaining;
    objc_opt::objc_clsopt_t *clsopt = new(optROData) objc_opt::objc_clsopt_t;
    err = clsopt->write(clsoptVMAddr, optRORemaining, 
                        classes.classNames(), classes.classes(), verbose);
    if (err) {
        warn(archName(), err);
        return;
    }
    optROData += clsopt->size();
    optRORemaining -= clsopt->size();
    size_t duplicateCount = clsopt->duplicateCount();
    clsopt->byteswap(E::little_endian), clsopt = NULL;


    // Sort method lists.

    // This is SAFE: modified binaries are still usable as unsorted lists.
    // This must be done AFTER uniquing selectors.

    MethodListSorter<A> methodSorter;
    for(typename std::vector<LayoutInfo>::const_iterator it = sizeSortedDylibs.begin(); it != sizeSortedDylibs.end(); ++it) {
        macho_header<P> *mh = (macho_header<P>*)(*it->layout).getSegments()[0].mappedAddress();
        methodSorter.optimize(this, mh);
    }


    // Repair ivar offsets.

    // This is SAFE: the runtime always validates ivar offsets at runtime.

    IvarOffsetOptimizer<A> ivarOffsetOptimizer;
	for(typename std::vector<LayoutInfo>::const_iterator it = sizeSortedDylibs.begin(); it != sizeSortedDylibs.end(); ++it) {
        const macho_header<P> *mh = (const macho_header<P>*)(*it->layout).getSegments()[0].mappedAddress();
        ivarOffsetOptimizer.optimize(this, mh);
	}
    

    // Success. Mark dylibs as optimized.
	for(typename std::vector<LayoutInfo>::const_iterator it = sizeSortedDylibs.begin(); it != sizeSortedDylibs.end(); ++it) {
        const macho_header<P> *mh = (const macho_header<P>*)(*it->layout).getSegments()[0].mappedAddress();
        const macho_section<P> *imageInfoSection;
        imageInfoSection = mh->getSection("__DATA", "__objc_imageinfo");
        if (!imageInfoSection) {
            imageInfoSection = mh->getSection("__OBJC", "__image_info");
        }
        if (imageInfoSection) {
            objc_image_info<A> *info = (objc_image_info<A> *)
                mappedAddressForVMAddress(imageInfoSection->addr());
            info->setOptimizedByDyld();
        }
    }


    // Success. Update RO header last.
    E::set32(optROHeader->selopt_offset, seloptVMAddr - optROSection->addr());
    E::set32(optROHeader->clsopt_offset, clsoptVMAddr - optROSection->addr());
    E::set32(optROHeader->headeropt_offset, hinfoVMAddr - optROSection->addr());

    if ( verbose ) {
        size_t roSize = optROSection->size() - optRORemaining;
        size_t rwSize = optRWSection->size() - optRWRemaining;
        fprintf(stderr, "update_dyld_shared_cache: for %s, %zu/%llu bytes "
                "(%d%%) used in libobjc read-only optimization section\n", 
                archName(), roSize, optROSection->size(), 
                percent(roSize, optROSection->size()));
        fprintf(stderr, "update_dyld_shared_cache: for %s, %zu/%llu bytes "
                "(%d%%) used in libobjc read/write optimization section\n", 
                archName(), rwSize, optRWSection->size(), 
                percent(rwSize, optRWSection->size()));
        fprintf(stderr, "update_dyld_shared_cache: for %s, "
                "uniqued %zu selectors\n", 
                archName(), uniq.strings().size());
        fprintf(stderr, "update_dyld_shared_cache: for %s, "
                "updated %zu selector references\n", 
                archName(), uniq.count());
        fprintf(stderr, "update_dyld_shared_cache: for %s, "
                "updated %zu ivar offsets\n", 
                archName(), ivarOffsetOptimizer.optimized());
        fprintf(stderr, "update_dyld_shared_cache: for %s, "
                "sorted %zu method lists\n", 
                archName(), methodSorter.optimized());
        fprintf(stderr, "update_dyld_shared_cache: for %s, "
                "recorded %zu classes (%zu duplicates)\n", 
                archName(), classes.classNames().size(), duplicateCount);
        fprintf(stderr, "update_dyld_shared_cache: for %s, "
                "wrote objc metadata optimization version %d\n", 
                archName(), objc_opt::VERSION);
    }

    return;
}


static const char* sCleanupFile = NULL;
static void cleanup(int sig)
{
	::signal(sig, SIG_DFL);
	if ( sCleanupFile != NULL )
		::unlink(sCleanupFile);
	//if ( verbose )
	//	fprintf(stderr, "update_dyld_shared_cache: deleting temp file in response to a signal\n");
	if ( sig == SIGINT )
		::exit(1);
}


// <rdar://problem/10730767> update_dyld_shared_cache should use sync_volume_np() instead of sync() 
static void sync_volume(const char* volumePath)
{
#if __MAC_OS_X_VERSION_MIN_REQUIRED >= 1080
	int error = sync_volume_np(volumePath, SYNC_VOLUME_FULLSYNC|SYNC_VOLUME_FULLSYNC);
#else
	int full_sync = 3; // SYNC_VOLUME_FULLSYNC | SYNC_VOLUME_FULLSYNC
	int error = 0;
	if ( fsctl(volumePath, 0x80004101 /*FSCTL_SYNC_VOLUME*/, &full_sync, 0) == -1) 
		error = errno;
#endif
	if ( error )
		::sync();
}


// <rdar://problem/12552226> update shared cache should sign the shared cache
static bool adhoc_codesign_share_cache(const char* path)
{
	CFURLRef target = ::CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), FALSE);
	if ( target == NULL )
		return false;

	SecStaticCodeRef code;
	OSStatus status = ::SecStaticCodeCreateWithPath(target, kSecCSDefaultFlags, &code);
	CFRelease(target);
	if ( status ) {
		::fprintf(stderr, "codesign: failed to create url to signed object\n");
		return false;
	}

	const void * keys[1] = { (void *)kSecCodeSignerIdentity } ;
	const void * values[1] = { (void *)kCFNull };
	CFDictionaryRef params = ::CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if ( params == NULL ) {
		CFRelease(code);
		return false;
	}
	
	SecCodeSignerRef signer;
	status = ::SecCodeSignerCreate(params, kSecCSDefaultFlags, &signer);
	CFRelease(params);
	if ( status ) {
		CFRelease(code);
		::fprintf(stderr, "codesign: failed to create signer object\n");
		return false;
	}

	status = ::SecCodeSignerAddSignatureWithErrors(signer, code, kSecCSDefaultFlags, NULL);
	CFRelease(code);
	CFRelease(signer);
	if ( status ) {
		::fprintf(stderr, "codesign: failed to sign object: %s\n", path);
		return false;
	}

	if ( verbose )
		::fprintf(stderr, "codesigning complete of %s\n", path);
	
	return true;
}



template <>	 bool	SharedCache<x86_64>::addCacheSlideInfo(){ return true; }
template <>	 bool	SharedCache<arm>::addCacheSlideInfo()	{ return true; }
template <>	 bool	SharedCache<x86>::addCacheSlideInfo()	{ return false; }



template <typename A>
bool SharedCache<A>::update(bool force, bool optimize, bool deleteExistingFirst, int archIndex,
								int archCount, bool keepSignatures, bool dontMapLocalSymbols)
{
	bool didUpdate = false;
	
	// already up to date?
	if ( force || fExistingIsNotUpToDate ) {
		if ( verbose )
			fprintf(stderr, "update_dyld_shared_cache: regenerating %s\n", fCacheFilePath);
		if ( fDylibs.size() == 0 ) {
			fprintf(stderr, "update_dyld_shared_cache: warning, empty cache not generated for arch %s\n", archName());
			return false;
		}
		// delete existing cache while building the new one
		// this is a flag to dyld to stop pinging update_dyld_shared_cache
		if ( deleteExistingFirst )
			::unlink(fCacheFilePath);
		uint8_t* inMemoryCache = NULL;
		uint32_t allocatedCacheSize = 0;
		char tempCachePath[strlen(fCacheFilePath)+16];
		sprintf(tempCachePath, "%s.tmp%u", fCacheFilePath, getpid());
		try {
			// allocate a memory block to hold cache
			uint32_t cacheFileSize = 0;
			for(std::vector<shared_file_mapping_np>::iterator it = fMappings.begin(); it != fMappings.end(); ++it) {
				uint32_t end = it->sfm_file_offset + it->sfm_size;
				if ( end > cacheFileSize ) 
					cacheFileSize = end;
			}
			if ( vm_allocate(mach_task_self(), (vm_address_t*)(&inMemoryCache), cacheFileSize, VM_FLAGS_ANYWHERE) != KERN_SUCCESS )
				throwf("can't vm_allocate cache of size %u", cacheFileSize);
			allocatedCacheSize = cacheFileSize;
            fInMemoryCache = inMemoryCache;
			
			// fill in header
			dyldCacheHeader<E>* header = (dyldCacheHeader<E>*)inMemoryCache;
			const char* archPairName = fArchGraph->archName();
			char temp[16];
			strcpy(temp, "dyld_v1        ");
			strcpy(&temp[15-strlen(archPairName)], archPairName);
			header->set_magic(temp);
			//header->set_architecture(arch());
			header->set_mappingOffset(sizeof(dyldCacheHeader<E>)); 
			header->set_mappingCount(fMappings.size());
			header->set_imagesOffset(header->mappingOffset() + fMappings.size()*sizeof(dyldCacheFileMapping<E>));	
			header->set_imagesCount(fDylibs.size()+fDylibAliases.size());
			header->set_dyldBaseAddress(fDyldBaseAddress);
			header->set_codeSignatureOffset(cacheFileSize);
			header->set_codeSignatureSize(0);
			header->set_slideInfoOffset(0);
			header->set_slideInfoSize(0);
			header->set_localSymbolsOffset(0);
			header->set_localSymbolsSize(0);
			
			// fill in mappings
			dyldCacheFileMapping<E>* mapping = (dyldCacheFileMapping<E>*)&inMemoryCache[sizeof(dyldCacheHeader<E>)];
			for(std::vector<shared_file_mapping_np>::iterator it = fMappings.begin(); it != fMappings.end(); ++it) {
				if ( verbose )
					fprintf(stderr, "update_dyld_shared_cache: cache mappings: address=0x%0llX, size=0x%0llX, fileOffset=0x%0llX, prot=0x%X\n", 
									it->sfm_address, it->sfm_size, it->sfm_file_offset, it->sfm_init_prot);
				mapping->set_address(it->sfm_address);
				mapping->set_size(it->sfm_size);
				mapping->set_file_offset(it->sfm_file_offset);
				mapping->set_max_prot(it->sfm_max_prot);
				mapping->set_init_prot(it->sfm_init_prot);
				++mapping;
			}
			
			// fill in image table
			dyldCacheImageInfo<E>* image = (dyldCacheImageInfo<E>*)mapping;
			for(typename std::vector<LayoutInfo>::iterator it = fDylibs.begin(); it != fDylibs.end(); ++it) {
				image->set_address(it->info.address);
				image->set_modTime(it->info.modTime);
				image->set_inode(it->info.inode);
				image->set_pathFileOffset(cacheFileOffsetForVMAddress(it->info.address+it->info.pathFileOffset));
				++image;
			}
			
			// add aliases to end of image table
			for(typename std::vector<LayoutInfo>::iterator it = fDylibAliases.begin(); it != fDylibAliases.end(); ++it) {
				image->set_address(it->info.address);
				image->set_modTime(it->info.modTime);
				image->set_inode(it->info.inode);
				image->set_pathFileOffset(it->info.pathFileOffset);
				strcpy((char*)inMemoryCache+it->info.pathFileOffset, it->aliases[0]);
				//fprintf(stderr, "adding alias to offset 0x%08X %s\n", it->info.pathFileOffset, it->aliases[0]);
				++image;
			}
						
			// copy each segment to cache buffer
			const int dylibCount = fDylibs.size();
			int dylibIndex = 0;
			int progressIndex = 0;
			for(typename std::vector<LayoutInfo>::const_iterator it = fDylibs.begin(); it != fDylibs.end(); ++it, ++dylibIndex) {
				const char* path = it->layout->getFilePath();
				int src = ::open(path, O_RDONLY, 0);
				if ( src == -1 )
					throwf("can't open file %s, errnor=%d", it->layout->getID().name, errno);
				// mark source as "don't cache"
				(void)fcntl(src, F_NOCACHE, 1);
				// verify file has not changed since dependency analysis
				struct stat stat_buf;
				if ( fstat(src, &stat_buf) == -1)
					throwf("can't stat open file %s, errno=%d", path, errno);
				if ( (it->layout->getInode() != stat_buf.st_ino) || (it->layout->getLastModTime() != stat_buf.st_mtime) )
					throwf("file modified during cache creation: %s", path);

				if ( verbose )
					fprintf(stderr, "update_dyld_shared_cache: copying %s to cache\n", it->layout->getFilePath());
				try {
					const std::vector<MachOLayoutAbstraction::Segment>& segs = it->layout->getSegments();
					for (int i=0; i < segs.size(); ++i) {
						const MachOLayoutAbstraction::Segment& seg = segs[i];
						if ( verbose )
							fprintf(stderr, "\t\tsegment %s, size=0x%0llX, cache address=0x%0llX\n", seg.name(), seg.fileSize(), seg.newAddress());
						if ( seg.size() > 0 ) {
							const uint64_t segmentSrcStartOffset = it->layout->getOffsetInUniversalFile()+seg.fileOffset();
							const uint64_t segmentSize = seg.fileSize();
							const uint64_t segmentDstStartOffset = cacheFileOffsetForVMAddress(seg.newAddress());
							ssize_t readResult = ::pread(src, &inMemoryCache[segmentDstStartOffset], segmentSize, segmentSrcStartOffset);
							if ( readResult != segmentSize ) {
								if ( readResult == -1 )
									throwf("read failure copying dylib errno=%d for %s", errno, it->layout->getID().name);
								else
									throwf("read failure copying dylib. Read of %lld bytes at file offset %lld returned %ld for %s", 
											segmentSize, segmentSrcStartOffset, readResult, it->layout->getID().name);
							}
						}
					}
				}
				catch (const char* msg) {
					throwf("%s while copying %s to shared cache", msg, it->layout->getID().name);
				}
				::close(src);
				if ( progress ) {
					// assuming read takes 40% of time
					int nextProgressIndex = archIndex*100+(40*dylibIndex)/dylibCount;
					if ( nextProgressIndex != progressIndex )
						fprintf(stdout, "%3u/%u\n", nextProgressIndex, archCount*100);
					progressIndex = nextProgressIndex;
				}
			}
						
			// set mapped address for each segment
			for(typename std::vector<LayoutInfo>::const_iterator it = fDylibs.begin(); it != fDylibs.end(); ++it) {
				std::vector<MachOLayoutAbstraction::Segment>& segs = ((MachOLayoutAbstraction*)(it->layout))->getSegments();
				for (int i=0; i < segs.size(); ++i) {
					MachOLayoutAbstraction::Segment& seg = segs[i];
					if ( seg.size() > 0 )
						seg.setMappedAddress(inMemoryCache + cacheFileOffsetForVMAddress(seg.newAddress()));
					//fprintf(stderr, "%s at %p to %p for %s\n", seg.name(), seg.mappedAddress(), (char*)seg.mappedAddress()+ seg.size(), it->layout->getID().name);
				}
			}
	
			// also construct list of all pointers in cache to other things in cache
			std::vector<void*> pointersInData;
			pointersInData.reserve(1024);
			
			// rebase each dylib in shared cache
			for(typename std::vector<LayoutInfo>::const_iterator it = fDylibs.begin(); it != fDylibs.end(); ++it) {
				try {
					Rebaser<A> r(*it->layout);
					r.rebase(pointersInData);
					//if ( verbose )
					//	fprintf(stderr, "update_dyld_shared_cache: for %s, rebasing dylib into cache for %s\n", archName(), it->layout->getID().name);
				}
				catch (const char* msg) {
					throwf("%s in %s", msg, it->layout->getID().name);
				}
			}
			
			if ( verbose )
				fprintf(stderr, "update_dyld_shared_cache: for %s, updating binding information for %lu files:\n", archName(), fDylibs.size());
			// instantiate a Binder for each image and add to map
			typename Binder<A>::Map map;
			std::vector<Binder<A>*> binders;
			for(typename std::vector<LayoutInfo>::const_iterator it = fDylibs.begin(); it != fDylibs.end(); ++it) {
				//fprintf(stderr, "binding %s\n", it->layout->getID().name);
				Binder<A>* binder = new Binder<A>(*it->layout, fDyldBaseAddress);
				binders.push_back(binder);
				// only add dylibs to map
				if ( it->layout->getID().name != NULL )
					map[it->layout->getID().name] = binder;
			}
  			
			// tell each Binder about the others
			for(typename std::vector<Binder<A>*>::iterator it = binders.begin(); it != binders.end(); ++it) {
				(*it)->setDependentBinders(map);
			}
			// perform binding
			for(typename std::vector<Binder<A>*>::iterator it = binders.begin(); it != binders.end(); ++it) {
				if ( verbose )
					fprintf(stderr, "update_dyld_shared_cache: for %s, updating binding information in cache for %s\n", archName(), (*it)->getDylibID());
				try {
					(*it)->bind(pointersInData);
				}
				catch (const char* msg) {
					throwf("%s in %s", msg, (*it)->getDylibID());
				}
			}
			// optimize binding
			for(typename std::vector<Binder<A>*>::iterator it = binders.begin(); it != binders.end(); ++it) {
				try {
					(*it)->optimize();
				}
				catch (const char* msg) {
					throwf("%s in %s", msg, (*it)->getDylibID());
				}
			}
			// delete binders
			for(typename std::vector<Binder<A>*>::iterator it = binders.begin(); it != binders.end(); ++it) {
				delete *it;
			}
	
			// merge/optimize all LINKEDIT segments
			if ( optimize ) {
				//fprintf(stderr, "update_dyld_shared_cache: original cache file size %uMB\n", cacheFileSize/(1024*1024));
				cacheFileSize = (this->optimizeLINKEDIT(keepSignatures, dontMapLocalSymbols) - inMemoryCache);
				//fprintf(stderr, "update_dyld_shared_cache: optimized cache file size %uMB\n", cacheFileSize/(1024*1024));
				// update header to reduce mapping size
				dyldCacheHeader<E>* cacheHeader = (dyldCacheHeader<E>*)inMemoryCache;
				dyldCacheFileMapping<E>* mappings = (dyldCacheFileMapping<E>*)&inMemoryCache[sizeof(dyldCacheHeader<E>)];
				dyldCacheFileMapping<E>* lastMapping = &mappings[cacheHeader->mappingCount()-1];
				lastMapping->set_size(cacheFileSize-lastMapping->file_offset());
				// update fMappings so .map file will print correctly
				fMappings.back().sfm_size = cacheFileSize-fMappings.back().sfm_file_offset;
				// update header
				//fprintf(stderr, "update_dyld_shared_cache: changing end of cache address from 0x%08llX to 0x%08llX\n", 
				//		header->codeSignatureOffset(), fMappings.back().sfm_address + fMappings.back().sfm_size);
				header->set_codeSignatureOffset(fMappings.back().sfm_file_offset + fMappings.back().sfm_size);
			}
			
			// unique objc selectors and update other objc metadata
            if ( optimize ) {
				optimizeObjC(pointersInData);
				if ( progress ) {
					// assuming objc optimizations takes 15% of time
					fprintf(stdout, "%3u/%u\n", (archIndex+1)*55, archCount*100);
				}
			}

			if ( addCacheSlideInfo() ) {
				// build bitmap of which pointers need sliding
				uint8_t* const dataStart = &inMemoryCache[fMappings[1].sfm_file_offset]; // R/W mapping is always second
				uint8_t* const dataEnd   = &inMemoryCache[fMappings[1].sfm_file_offset+fMappings[1].sfm_size];
				const int bitmapSize = (dataEnd - dataStart)/(4*8);
				uint8_t* bitmap = (uint8_t*)calloc(bitmapSize, 1);
				void* lastPointer = inMemoryCache;
				for(std::vector<void*>::iterator pit=pointersInData.begin(); pit != pointersInData.end(); ++pit) {
					if ( *pit != lastPointer ) {
						void* p = *pit;
						if ( (p < dataStart) || ( p > dataEnd) )
							throwf("DATA pointer for sliding, out of range 0x%08lX\n", (long)((uint8_t*)p-inMemoryCache));
						long offset = (long)((uint8_t*)p - dataStart);
						if ( (offset % 4) != 0 )
							throwf("pointer not 4-byte aligned in DATA offset 0x%08lX\n", offset);
						long byteIndex = offset / (4*8);
						long bitInByte =  (offset % 32) >> 2;
						bitmap[byteIndex] |= (1 << bitInByte);
						lastPointer = p;
					}
				}

				// allocate worst case size block of all slide info
				const int entry_size = 4096/(8*4); // 8 bits per byte, possible pointer every 4 bytes.
				const int toc_count = bitmapSize/entry_size;
				int slideInfoSize = sizeof(dyldCacheSlideInfo<E>) + 2*toc_count + entry_size*(toc_count+1);
				dyldCacheSlideInfo<E>* slideInfo = (dyldCacheSlideInfo<E>*)calloc(slideInfoSize, 1);
				slideInfo->set_version(1);
				slideInfo->set_toc_offset(sizeof(dyldCacheSlideInfo<E>));
				slideInfo->set_toc_count(toc_count);
				slideInfo->set_entries_offset((slideInfo->toc_offset()+2*toc_count+127)&(-128));
				slideInfo->set_entries_count(0);
				slideInfo->set_entries_size(entry_size);
				// append each unique entry 
				const dyldCacheSlideInfoEntry* bitmapAsEntries = (dyldCacheSlideInfoEntry*)bitmap;
				dyldCacheSlideInfoEntry* const entriesInSlidInfo = (dyldCacheSlideInfoEntry*)((char*)slideInfo+slideInfo->entries_offset());
				int entry_count = 0;
				for (int i=0; i < toc_count; ++i) {
					const dyldCacheSlideInfoEntry* thisEntry = &bitmapAsEntries[i];
					// see if it is same as one already added
					bool found = false;
					for (int j=0; j < entry_count; ++j) {
						if ( memcmp(thisEntry, &entriesInSlidInfo[j], entry_size) == 0 ) {
							//fprintf(stderr, "toc[%d] optimized to %d\n", i, j);
							slideInfo->set_toc(i, j);
							found = true;
							break;
						}	
					}
					if ( ! found ) {
						// append to end
						memcpy(&entriesInSlidInfo[entry_count], thisEntry, entry_size);
						slideInfo->set_toc(i, entry_count++);
					}
				}
				slideInfo->set_entries_count(entry_count);
	
				int slideInfoPageSize = pageAlign(slideInfo->entries_offset() + entry_count*entry_size);
				cacheFileSize += slideInfoPageSize;
			
				// update mappings to increase RO size
				dyldCacheHeader<E>* cacheHeader = (dyldCacheHeader<E>*)inMemoryCache;
				dyldCacheFileMapping<E>* mappings = (dyldCacheFileMapping<E>*)&inMemoryCache[sizeof(dyldCacheHeader<E>)];
				dyldCacheFileMapping<E>* lastMapping = &mappings[cacheHeader->mappingCount()-1];
				lastMapping->set_size(lastMapping->size()+slideInfoPageSize);
				
				// update header to show location of slidePointers
				cacheHeader->set_slideInfoOffset(cacheHeader->codeSignatureOffset());
				cacheHeader->set_slideInfoSize(slideInfoPageSize);
				cacheHeader->set_codeSignatureOffset(cacheHeader->codeSignatureOffset()+slideInfoPageSize);
				
				// update fMappings so .map file will print correctly
				fMappings.back().sfm_size = cacheFileSize-fMappings.back().sfm_file_offset;
				
				// copy compressed into into buffer
				memcpy(&inMemoryCache[cacheHeader->slideInfoOffset()], slideInfo, slideInfoPageSize);	
			}
			
			// make sure after all optimizations, that whole cache file fits into shared region address range
			{
				dyldCacheHeader<E>* cacheHeader = (dyldCacheHeader<E>*)inMemoryCache;
				dyldCacheFileMapping<E>* mappings = (dyldCacheFileMapping<E>*)&inMemoryCache[cacheHeader->mappingOffset()];
				for (int i=0; i < cacheHeader->mappingCount(); ++i) {
					uint64_t endAddr = mappings[i].address() + mappings[i].size();
					if ( endAddr > (sharedRegionStartAddress() + sharedRegionSize()) ) {
						throwf("update_dyld_shared_cache[%u] for arch=%s, shared cache will not fit in address space: 0x%llX\n",
							getpid(), fArchGraph->archName(), endAddr);
					}
				}
			}
			
			// append local symbol info in an unmapped region
			if ( dontMapLocalSymbols ) {
				uint32_t spaceAtEnd = allocatedCacheSize - cacheFileSize;
				uint32_t localSymbolsOffset = pageAlign(cacheFileSize);
				dyldCacheLocalSymbolsInfo<E>* infoHeader = (dyldCacheLocalSymbolsInfo<E>*)(&inMemoryCache[localSymbolsOffset]);
				const uint32_t entriesOffset = sizeof(dyldCacheLocalSymbolsInfo<E>);
				const uint32_t entriesCount = fLocalSymbolInfos.size();
				const uint32_t nlistOffset = entriesOffset + entriesCount * sizeof(dyldCacheLocalSymbolEntry<E>);
				const uint32_t nlistCount = fUnmappedLocalSymbols.size();
				const uint32_t stringsOffset = nlistOffset + nlistCount * sizeof(macho_nlist<P>);
				const uint32_t stringsSize = fUnmappedLocalsStringPool.size();
				if ( stringsOffset+stringsSize > spaceAtEnd ) 
					throwf("update_dyld_shared_cache[%u] for arch=%s, out of space for local symbols. Have 0x%X, Need 0x%X\n",
							getpid(), fArchGraph->archName(), spaceAtEnd, stringsOffset+stringsSize);
				// fill in local symbols info
				infoHeader->set_nlistOffset(nlistOffset);
				infoHeader->set_nlistCount(nlistCount);
				infoHeader->set_stringsOffset(stringsOffset);
				infoHeader->set_stringsSize(stringsSize);
				infoHeader->set_entriesOffset(entriesOffset);
				infoHeader->set_entriesCount(entriesCount);
				// copy info for each dylib
				dyldCacheLocalSymbolEntry<E>* entries = (dyldCacheLocalSymbolEntry<E>*)(&inMemoryCache[localSymbolsOffset+entriesOffset]);
				for (int i=0; i < entriesCount; ++i) {
					entries[i].set_dylibOffset(fLocalSymbolInfos[i].dylibOffset);
					entries[i].set_nlistStartIndex(fLocalSymbolInfos[i].nlistStartIndex);
					entries[i].set_nlistCount(fLocalSymbolInfos[i].nlistCount);
				}
				// copy nlists
				memcpy(&inMemoryCache[localSymbolsOffset+nlistOffset], &fUnmappedLocalSymbols[0], nlistCount*sizeof(macho_nlist<P>));
				// copy string pool
				memcpy(&inMemoryCache[localSymbolsOffset+stringsOffset], fUnmappedLocalsStringPool.getBuffer(), stringsSize);
				
				// update state
				fUnmappedLocalSymbolsSize = pageAlign(stringsOffset + stringsSize);
				cacheFileSize = localSymbolsOffset + fUnmappedLocalSymbolsSize;
				
				// update header to show location of slidePointers
				dyldCacheHeader<E>* cacheHeader = (dyldCacheHeader<E>*)inMemoryCache;
				cacheHeader->set_localSymbolsOffset(localSymbolsOffset);
				cacheHeader->set_localSymbolsSize(stringsOffset+stringsSize);
				cacheHeader->set_codeSignatureOffset(cacheFileSize);
			}
			
			// compute UUID of whole cache
			uint8_t digest[16];
			CC_MD5(inMemoryCache, cacheFileSize, digest);
			// <rdar://problem/6723729> uuids should conform to RFC 4122 UUID version 4 & UUID version 5 formats
			digest[6] = ( digest[6] & 0x0F ) | ( 3 << 4 );
			digest[8] = ( digest[8] & 0x3F ) | 0x80;
			((dyldCacheHeader<E>*)inMemoryCache)->set_uuid(digest);
			
			if ( fVerify ) {
				// if no existing cache, say so
				if ( fExistingCacheForVerification == NULL ) {
					throwf("update_dyld_shared_cache[%u] for arch=%s, could not verify because cache file does not exist in /var/db/dyld/\n",
					 getpid(), archName());
				}
				// new cache is built, compare header entries
				const dyldCacheHeader<E>* newHeader = (dyldCacheHeader<E>*)inMemoryCache;
				const dyldCacheHeader<E>* oldHeader = (dyldCacheHeader<E>*)fExistingCacheForVerification;
				if ( newHeader->mappingCount() != oldHeader->mappingCount() ) {
					throwf("update_dyld_shared_cache[%u] for arch=%s, could not verify cache because caches have a different number of mappings\n",
					 getpid(), archName());
				}
				const dyldCacheFileMapping<E>* newMappings = (dyldCacheFileMapping<E>*)&inMemoryCache[newHeader->mappingOffset()];
				const dyldCacheFileMapping<E>* oldMappings = (dyldCacheFileMapping<E>*)&fExistingCacheForVerification[oldHeader->mappingOffset()];
				for (int i=0; i < newHeader->mappingCount(); ++i) {
					if ( newMappings[i].address() != oldMappings[i].address() ) {
						throwf("update_dyld_shared_cache[%u] for arch=%s, could not verify cache because mapping %d starts at a different address 0x%0llX vs 0x%0llX\n", 
							getpid(), archName(), i, newMappings[i].address(), oldMappings[i].address() );
					}
					if ( newMappings[i].size() != oldMappings[i].size() ) {
						throwf("update_dyld_shared_cache[%u] for arch=%s, could not verify cache because mapping %d has a different size\n",
						 getpid(), archName(), i);
					}
				}
				
				//fprintf(stderr, "%s existing cache = %p\n", archName(), fExistingCacheForVerification);
				//fprintf(stderr, "%s new cache = %p\n", archName(), inMemoryCache);
				// compare content to existing cache page by page
				for (int offset=0; offset < cacheFileSize; offset += 4096) {
					if ( memcmp(&inMemoryCache[offset], &fExistingCacheForVerification[offset], 4096) != 0 ) {
						fprintf(stderr, "verifier found differences on page offset 0x%08X for %s:\n", offset, archName());
						for(typename std::vector<LayoutInfo>::const_iterator it = fDylibs.begin(); it != fDylibs.end(); ++it, ++dylibIndex) {
							const std::vector<MachOLayoutAbstraction::Segment>& segs = it->layout->getSegments();
							for(std::vector<MachOLayoutAbstraction::Segment>::const_iterator sit = segs.begin(); sit != segs.end(); ++sit) {
								const MachOLayoutAbstraction::Segment& seg = *sit;
								if ( (seg.mappedAddress() <= &inMemoryCache[offset]) && (&inMemoryCache[offset] < ((uint8_t*)seg.mappedAddress() + seg.fileSize())) ) {
									// all LINKEDITs point to the same region, so just print one
									if ( strcmp(seg.name(), "__LINKEDIT") == 0 ) 
										fprintf(stderr, "  in merged LINKEDIT segment\n");
									else
										fprintf(stderr, "  in segment %s of dylib %s\n", seg.name(), it->layout->getID().name);
									break;
								}
							}
						}
						for (int po=0; po < 4096; po += 16) {
							if ( memcmp(&inMemoryCache[offset+po], &fExistingCacheForVerification[offset+po], 16) != 0 ) {
								fprintf(stderr, "   existing: 0x%08X: ", offset+po);
								for ( int j=0; j < 16; ++j)
									fprintf(stderr, " 0x%02X", fExistingCacheForVerification[offset+po+j]);
								fprintf(stderr, "\n");
								fprintf(stderr, "  should be: 0x%08X: ", offset+po);
								for ( int j=0; j < 16; ++j)
									fprintf(stderr, " 0x%02X", inMemoryCache[offset+po+j]);
								fprintf(stderr, "\n");
							}
						}
					}
				}
			}
			else {
				// install signal handlers to delete temp file if program is killed 
				sCleanupFile = tempCachePath;
				::signal(SIGINT, cleanup);
				::signal(SIGBUS, cleanup);
				::signal(SIGSEGV, cleanup);
				
				// create var/db/dyld dirs if needed
				char dyldDirs[1024];
				strcpy(dyldDirs, fCacheFilePath);
				char* lastSlash = strrchr(dyldDirs, '/');
				if ( lastSlash != NULL )
					lastSlash[1] = '\0';
				struct stat stat_buf;
				if ( stat(dyldDirs, &stat_buf) != 0 ) {
					const char* afterSlash = &dyldDirs[1];
					char* slash;
					while ( (slash = strchr(afterSlash, '/')) != NULL ) {
						*slash = '\0';
						::mkdir(dyldDirs, S_IRWXU | S_IRGRP|S_IXGRP | S_IROTH|S_IXOTH);
						*slash = '/';
						afterSlash = slash+1;
					}
				}
				
				// create temp file for cache
				int fd = ::open(tempCachePath, O_CREAT | O_RDWR | O_TRUNC, 0644);	
				if ( fd == -1 )
					throwf("can't create temp file %s, errnor=%d", tempCachePath, errno);
					
				// try to allocate whole cache file contiguously
				fstore_t fcntlSpec = { F_ALLOCATECONTIG|F_ALLOCATEALL, F_PEOFPOSMODE, 0, cacheFileSize, 0 };
				::fcntl(fd, F_PREALLOCATE, &fcntlSpec);

				// write out cache file
				if ( verbose )
					fprintf(stderr, "update_dyld_shared_cache: writing cache to disk: %s\n", tempCachePath);
				if ( ::pwrite(fd, inMemoryCache, cacheFileSize, 0) != cacheFileSize )
					throwf("write() failure creating cache file, errno=%d", errno);
				if ( progress ) {
					// assuming write takes 35% of time
					fprintf(stdout, "%3u/%u\n", (archIndex+1)*90, archCount*100);
				}
				
				// flush to disk and close
				int result = ::fcntl(fd, F_FULLFSYNC, NULL);
				if ( result == -1 ) 
					fprintf(stderr, "update_dyld_shared_cache: warning, fcntl(F_FULLFSYNC) failed with errno=%d for %s\n", errno, tempCachePath);
				result = ::close(fd);
				if ( result != 0 ) 
					fprintf(stderr, "update_dyld_shared_cache: warning, close() failed with errno=%d for %s\n", errno, tempCachePath);
				
				if ( !iPhoneOS )
					adhoc_codesign_share_cache(tempCachePath);

				// <rdar://problem/7901042> Make life easier for the kernel at shutdown.
				// If we just move the new cache file over the old, the old file
				// may need to exist in the open-unlink state.  But because it
				// may be mapped into the shared region, it cannot be deleted
				// until all user processes are terminated.  That leaves are
				// small to non-existent window for the kernel to delete the
				// old cache file.
				if ( fCacheFileInFinalLocation ) {
					char tmpDirPath[64];
					const char* pathLastSlash = strrchr(fCacheFilePath, '/');
					if ( pathLastSlash != NULL ) {
						sprintf(tmpDirPath, "/var/run%s.old.%u", pathLastSlash, getpid());
						// move existing cache file to /var/run to be clean up next boot
						result = ::rename(fCacheFilePath, tmpDirPath);
						if ( result != 0 ) {
							if ( errno != ENOENT )
								fprintf(stderr, "update_dyld_shared_cache: warning, unable to move existing cache to %s errno=%d for %s\n", tmpDirPath, errno, fCacheFilePath);
						}
					}
				}
				
				// move new cache file to correct location for use after reboot
				if ( verbose )
					fprintf(stderr, "update_dyld_shared_cache: atomically moving cache file into place: %s\n", fCacheFilePath);
				result = ::rename(tempCachePath, fCacheFilePath);
				if ( result != 0 ) 
					throwf("can't swap newly create dyld shared cache file: rename(%s,%s) returned errno=%d", tempCachePath, fCacheFilePath, errno);
				
				
				// flush everything to disk to assure rename() gets recorded
				sync_volume(fCacheFilePath);
				didUpdate = true;
				
				// restore default signal handlers
				::signal(SIGINT, SIG_DFL);
				::signal(SIGBUS, SIG_DFL);
				::signal(SIGSEGV, SIG_DFL);

				// generate human readable "map" file that shows the layout of the cache file
				if ( verbose )
					fprintf(stderr, "update_dyld_shared_cache: writing .map file to disk\n");
				char mapFilePath[strlen(fCacheFilePath)+16];
				sprintf(mapFilePath, "%s.map", fCacheFilePath);
				char tempMapFilePath[strlen(fCacheFilePath)+32];
				sprintf(tempMapFilePath, "%s.map%u", fCacheFilePath, getpid());
				FILE* fmap = ::fopen(tempMapFilePath, "w");	
				if ( fmap == NULL ) {
					fprintf(stderr, "can't create map file %s, errnor=%d", tempCachePath, errno);
				}
				else {
					for(std::vector<shared_file_mapping_np>::iterator it = fMappings.begin(); it != fMappings.end(); ++it) {
						const char* prot = "RW";
						if ( it->sfm_init_prot == (VM_PROT_EXECUTE|VM_PROT_READ) )
							prot = "EX";
						else if ( it->sfm_init_prot == VM_PROT_READ )
							prot = "RO";
						else if ( it->sfm_init_prot == (VM_PROT_EXECUTE|VM_PROT_WRITE|VM_PROT_READ) )
							prot = "WX";
						if ( it->sfm_size > 1024*1024 )
							fprintf(fmap, "mapping  %s %4lluMB 0x%0llX -> 0x%0llX\n", prot, it->sfm_size/(1024*1024),
																it->sfm_address, it->sfm_address+it->sfm_size);
						else
							fprintf(fmap, "mapping  %s %4lluKB 0x%0llX -> 0x%0llX\n", prot, it->sfm_size/1024,
																it->sfm_address, it->sfm_address+it->sfm_size);
					}

					fprintf(fmap, " linkedit   %4uKB 0x%0llX -> 0x%0llX weak binding info\n",		
								(fOffsetOfExportInfoInCombinedLinkedit-fOffsetOfWeakBindInfoInCombinedLinkedit)/1024,
								fLinkEditsStartAddress+fOffsetOfWeakBindInfoInCombinedLinkedit,
								fLinkEditsStartAddress+fOffsetOfExportInfoInCombinedLinkedit);
					fprintf(fmap, " linkedit   %4uKB 0x%0llX -> 0x%0llX export info\n",		
								(fOffsetOfBindInfoInCombinedLinkedit-fOffsetOfExportInfoInCombinedLinkedit)/1024,
								fLinkEditsStartAddress+fOffsetOfExportInfoInCombinedLinkedit,
								fLinkEditsStartAddress+fOffsetOfBindInfoInCombinedLinkedit);
					fprintf(fmap, " linkedit   %4uKB 0x%0llX -> 0x%0llX binding info\n",		
								(fOffsetOfLazyBindInfoInCombinedLinkedit-fOffsetOfBindInfoInCombinedLinkedit)/1024,
								fLinkEditsStartAddress+fOffsetOfBindInfoInCombinedLinkedit,
								fLinkEditsStartAddress+fOffsetOfLazyBindInfoInCombinedLinkedit);
					fprintf(fmap, " linkedit   %4uKB 0x%0llX -> 0x%0llX lazy binding info\n",		
								(fOffsetOfOldSymbolTableInfoInCombinedLinkedit-fOffsetOfLazyBindInfoInCombinedLinkedit)/1024,
								fLinkEditsStartAddress+fOffsetOfLazyBindInfoInCombinedLinkedit,
								fLinkEditsStartAddress+fOffsetOfOldSymbolTableInfoInCombinedLinkedit);
					fprintf(fmap, " linkedit   %4uMB 0x%0llX -> 0x%0llX non-dyld symbol table size\n",		
								(fSizeOfOldSymbolTableInfoInCombinedLinkedit)/(1024*1024),
								fLinkEditsStartAddress+fOffsetOfOldSymbolTableInfoInCombinedLinkedit,
								fLinkEditsStartAddress+fOffsetOfOldSymbolTableInfoInCombinedLinkedit+fSizeOfOldSymbolTableInfoInCombinedLinkedit);				
					if ( fSizeOfFunctionStartsInCombinedLinkedit != 0 )
						fprintf(fmap, " linkedit   %4uKB 0x%0llX -> 0x%0llX non-dyld functions starts size\n",		
								fSizeOfFunctionStartsInCombinedLinkedit/1024,
								fLinkEditsStartAddress+fOffsetOfFunctionStartsInCombinedLinkedit,
								fLinkEditsStartAddress+fOffsetOfFunctionStartsInCombinedLinkedit+fSizeOfFunctionStartsInCombinedLinkedit);				
					if ( fSizeOfDataInCodeInCombinedLinkedit != 0 )
						fprintf(fmap, " linkedit   %4uKB 0x%0llX -> 0x%0llX non-dyld data-in-code info size\n",		
								fSizeOfDataInCodeInCombinedLinkedit/1024,
								fLinkEditsStartAddress+fOffsetOfDataInCodeInCombinedLinkedit,
								fLinkEditsStartAddress+fOffsetOfDataInCodeInCombinedLinkedit+fSizeOfDataInCodeInCombinedLinkedit);				
					if ( fSizeOfOldExternalRelocationsInCombinedLinkedit != 0 )
						fprintf(fmap, " linkedit   %4uKB 0x%0llX -> 0x%0llX non-dyld external relocs size\n",		
								fSizeOfOldExternalRelocationsInCombinedLinkedit/1024,
								fLinkEditsStartAddress+fOffsetOfOldExternalRelocationsInCombinedLinkedit,
								fLinkEditsStartAddress+fOffsetOfOldExternalRelocationsInCombinedLinkedit+fSizeOfOldExternalRelocationsInCombinedLinkedit);				
					fprintf(fmap, " linkedit   %4uKB 0x%0llX -> 0x%0llX non-dyld indirect symbol table size\n",		
								fSizeOfOldIndirectSymbolsInCombinedLinkedit/1024,
								fLinkEditsStartAddress+fOffsetOfOldIndirectSymbolsInCombinedLinkedit,
								fLinkEditsStartAddress+fOffsetOfOldIndirectSymbolsInCombinedLinkedit+fSizeOfOldIndirectSymbolsInCombinedLinkedit);				
					fprintf(fmap, " linkedit   %4uMB 0x%0llX -> 0x%0llX non-dyld string pool\n",		
								(fSizeOfOldStringPoolInCombinedLinkedit)/(1024*1024),
								fLinkEditsStartAddress+fOffsetOfOldStringPoolInCombinedLinkedit,
								fLinkEditsStartAddress+fOffsetOfOldStringPoolInCombinedLinkedit+fSizeOfOldStringPoolInCombinedLinkedit);				
					
					fprintf(fmap, "unmapped -- %4uMB local symbol info\n", fUnmappedLocalSymbolsSize/(1024*1024));					
					
					uint64_t endMappingAddr = fMappings[2].sfm_address + fMappings[2].sfm_size;
					fprintf(fmap, "total map   %4lluMB\n", (endMappingAddr - sharedRegionStartAddress())/(1024*1024));	
					if ( sharedRegionStartWritableAddress(0) == 0x7FFF70000000LL ) {
						// x86_64 has different slide constraints
						uint64_t freeSpace = 256*1024*1024 - fMappings[1].sfm_size;
						fprintf(fmap, "r/w space   %4lluMB -> %d bits of entropy for ASLR\n\n", freeSpace/(1024*1024), (int)log2(freeSpace/4096));
					}
					else {
						uint64_t freeSpace = sharedRegionStartAddress() + sharedRegionSize() - endMappingAddr;
						fprintf(fmap, "free space  %4lluMB -> %d bits of entropy for ASLR\n\n", freeSpace/(1024*1024), (int)log2(freeSpace/4096));
					}
					
					for(typename std::vector<LayoutInfo>::const_iterator it = fDylibs.begin(); it != fDylibs.end(); ++it) {
						fprintf(fmap, "%s\n", it->layout->getID().name);
						for (std::vector<const char*>::const_iterator ait = it->aliases.begin(); ait != it->aliases.end(); ++ait) 
							fprintf(fmap, "%s\n", *ait);
						const std::vector<MachOLayoutAbstraction::Segment>&	segs = it->layout->getSegments();
						for (int i=0; i < segs.size(); ++i) {
							const MachOLayoutAbstraction::Segment& seg = segs[i];
							fprintf(fmap, "\t%16s 0x%0llX -> 0x%0llX\n", seg.name(), seg.newAddress(), seg.newAddress()+seg.size());
						}
					}
					if ( warnings.size() > 0 ) {
						fprintf(fmap, "# Warnings:\n");
						for (std::vector<const char*>::iterator it=warnings.begin(); it != warnings.end(); ++it) {
							fprintf(fmap, "# %s\n", *it);
						}
					}
					fclose(fmap);
					result = ::rename(tempMapFilePath, mapFilePath);
				}
			}
			
			// free in memory cache
			vm_deallocate(mach_task_self(), (vm_address_t)inMemoryCache, allocatedCacheSize);
			inMemoryCache = NULL;
			if ( progress ) {
				// finished
				fprintf(stdout, "%3u/%u\n", (archIndex+1)*100, archCount*100);
			}
		}
		catch (...){
			// remove temp cache file
			::unlink(tempCachePath);
			// remove in memory cache
			if ( inMemoryCache != NULL ) 
				vm_deallocate(mach_task_self(), (vm_address_t)inMemoryCache, allocatedCacheSize);
			throw;
		}
	}
	return didUpdate;
}



//
//	The shared cache is driven by /var/db/dyld/shared_region_roots which contains
//	the paths used to search for dylibs that should go in the shared cache  
//
//	Leading and trailing white space is ignored
//	Blank lines are ignored
//	Lines starting with # are ignored
//
static void parsePathsFile(const char* filePath, std::vector<const char*>& paths)
{
	// read in whole file
	int fd = open(filePath, O_RDONLY, 0);
	if ( fd == -1 ) {
		fprintf(stderr, "update_dyld_shared_cache: can't open file: %s\n", filePath);
		exit(1);
	}
	struct stat stat_buf;
	fstat(fd, &stat_buf);
	char* p = (char*)malloc(stat_buf.st_size);
	if ( p == NULL ) {
		fprintf(stderr, "update_dyld_shared_cache: malloc failure\n");
		exit(1);
	}	
	if ( read(fd, p, stat_buf.st_size) != stat_buf.st_size ) {
		fprintf(stderr, "update_dyld_shared_cache: can't read file: %s\n", filePath);
		exit(1);
	}	
	::close(fd);
	
	// parse into paths and add to vector
	char * const end = &p[stat_buf.st_size];
	enum { lineStart, inSymbol, inComment } state = lineStart;
	char* symbolStart = NULL;
	for (char* s = p; s < end; ++s ) {
		switch ( state ) {
			case lineStart:
				if ( *s =='#' ) {
					state = inComment;
				}
				else if ( !isspace(*s) ) {
					state = inSymbol;
					symbolStart = s;
				}
				break;
			case inSymbol:
				if ( *s == '\n' ) {
					*s = '\0';
					// removing any trailing spaces
					char* last = s-1;
					while ( isspace(*last) ) {
						*last = '\0';
						--last;
					}
					// <rdar://problem/8305479> images in shared cache are bound against different IOKit than found at runtime
					// HACK:  Just ignore the known bad IOKit
					if ( strcmp(symbolStart, "/System/Library/Frameworks/IOKit.framework/IOKit") == 0 ) {
						// Disable warning because after three years <rdar://problem/7089957> has still not been fixed...
						//fprintf(stderr, "update_dyld_shared_cache: warning, ignoring /System/Library/Frameworks/IOKit.framework/IOKit\n");
						//warnings.push_back("update_dyld_shared_cache: warning, ignoring /System/Library/Frameworks/IOKit.framework/IOKit\n");
					}
					else {
						paths.push_back(symbolStart);
					}
					symbolStart = NULL;
					state = lineStart;
				}
				break;
			case inComment:
				if ( *s == '\n' )
					state = lineStart;
				break;
		}
	}
	// Note: we do not free() the malloc buffer, because the strings in it are used by exec()
}



static void setSharedDylibs(const char* rootPath, const char* overlayPath, const std::set<ArchPair>& onlyArchs, std::vector<const char*> rootsPaths)
{
	// set file system root
	ArchGraph::setFileSystemRoot(rootPath);
	ArchGraph::setFileSystemOverlay(overlayPath);

	// initialize all architectures requested
	for(std::set<ArchPair>::iterator a = onlyArchs.begin(); a != onlyArchs.end(); ++a)
		ArchGraph::addArchPair(*a);

	// add roots to graph
	for(std::vector<const char*>::const_iterator it = rootsPaths.begin(); it != rootsPaths.end(); ++it) 
		ArchGraph::addRoot(*it, onlyArchs);

	// determine shared dylibs
	for(std::set<ArchPair>::iterator a = onlyArchs.begin(); a != onlyArchs.end(); ++a)
		ArchGraph::findSharedDylibs(*a);
}


static void scanForSharedDylibs(const char* rootPath, const char* overlayPath, const char* dirOfPathFiles, const std::set<ArchPair>& onlyArchs)
{
	char rootDirOfPathFiles[strlen(rootPath)+strlen(dirOfPathFiles)+2];
	// in -root mode, look for roots in /rootpath/var/db/dyld
	if ( rootPath[0] != '\0' ) {
		strcpy(rootDirOfPathFiles, rootPath);
		strcat(rootDirOfPathFiles, dirOfPathFiles);
		dirOfPathFiles = rootDirOfPathFiles;
	}

	// extract all root paths from files in "/var/db/dyld/shared_region_roots/"
	if ( verbose )
		fprintf(stderr, "update_dyld_shared_cache: finding roots in: %s\n", dirOfPathFiles);
	std::vector<const char*> rootsPaths;
	DIR* dir = ::opendir(dirOfPathFiles);
	if ( dir == NULL )
		throwf("%s does not exist, errno=%d\n", dirOfPathFiles, errno);
	for (dirent* entry = ::readdir(dir); entry != NULL; entry = ::readdir(dir)) {
		if ( entry->d_type == DT_REG || entry->d_type == DT_UNKNOWN ) {
			// only look at regular files ending in .paths
			if ( strcmp(&entry->d_name[entry->d_namlen-6], ".paths") == 0 ) {
				struct stat tmpStatPathsFile;
				char fullPath[strlen(dirOfPathFiles)+entry->d_namlen+2];
				strcpy(fullPath, dirOfPathFiles);
				strcat(fullPath, "/");
				strcat(fullPath, entry->d_name);
				if ( lstat(fullPath, &tmpStatPathsFile) == -1 ) {
					fprintf(stderr, "update_dyld_shared_cache: can't access %s\n", fullPath);
				} 
				else if ( S_ISREG(tmpStatPathsFile.st_mode) ) {
					parsePathsFile(fullPath, rootsPaths);
				} 
				else {
					fprintf(stderr, "update_dyld_shared_cache: wrong file type for %s\n", fullPath);
				}
			}
			else {
				fprintf(stderr, "update_dyld_shared_cache: warning, ignore file with wrong extension: %s\n", entry->d_name);
			}
		}
	}
	::closedir(dir);
	
	if ( rootsPaths.size() == 0 )
		fprintf(stderr, "update_dyld_shared_cache: warning, no entries found in shared_region_roots\n");
	setSharedDylibs(rootPath, overlayPath, onlyArchs, rootsPaths);
}

static void setSharedDylibs(const char* rootPath, const char* overlayPath, const char* pathsFile, const std::set<ArchPair>& onlyArchs)
{
	std::vector<const char*> rootsPaths;
	parsePathsFile(pathsFile, rootsPaths);
	setSharedDylibs(rootPath, overlayPath, onlyArchs, rootsPaths);
}


// If the 10.5.0 version of update_dyld_shared_cache was killed or crashed, it 
// could leave large half written cache files laying around.  The function deletes
// those files.  To prevent the deletion of tmp files being created by another
// copy of update_dyld_shared_cache, it only deletes the temp cache file if its 
// creation time was before the last restart of this machine.
static void deleteOrphanTempCacheFiles()
{
	DIR* dir = ::opendir(MACOSX_DYLD_SHARED_CACHE_DIR);
	if ( dir != NULL ) {
		std::vector<const char*> filesToDelete;
		for (dirent* entry = ::readdir(dir); entry != NULL; entry = ::readdir(dir)) {
			if ( entry->d_type == DT_REG ) {
				// only look at files with .tmp in name
				if ( strstr(entry->d_name, ".tmp") != NULL ) {
					char fullPath[strlen(MACOSX_DYLD_SHARED_CACHE_DIR)+entry->d_namlen+2];
					strcpy(fullPath, MACOSX_DYLD_SHARED_CACHE_DIR);
					strcat(fullPath, "/");
					strcat(fullPath, entry->d_name);
					struct stat tmpFileStatInfo;
					if ( stat(fullPath, &tmpFileStatInfo) != -1 ) {
						int mib[2] = {CTL_KERN, KERN_BOOTTIME};
						struct timeval boottime;
						size_t size = sizeof(boottime);
						if ( (sysctl(mib, 2, &boottime, &size, NULL, 0) != -1) && (boottime.tv_sec != 0) ) {	
							// make sure this file is older than the boot time of this machine
							if ( tmpFileStatInfo.st_mtime < boottime.tv_sec ) {
								filesToDelete.push_back(strdup(fullPath));
							}
						}
					}
				}
			}
		}
		::closedir(dir);
		for(std::vector<const char*>::iterator it = filesToDelete.begin(); it != filesToDelete.end(); ++it) {
			fprintf(stderr, "update_dyld_shared_cache: deleting old temp cache file: %s\n", *it);
			::unlink(*it);
		}
	}
}



static bool updateSharedeCacheFile(const char* rootPath, const char* overlayPath, const char* cacheDir, bool explicitCacheDir, const std::set<ArchPair>& onlyArchs, 
									bool force, bool alphaSort, bool optimize, bool deleteExistingFirst, bool verify, bool keepSignatures, bool dontMapLocalSymbols)
{
	bool didUpdate = false;
	// get dyld load address info
	UniversalMachOLayout* dyldLayout = NULL;
	char dyldPath[1024];
	strlcpy(dyldPath, rootPath, 1024);
	strlcat(dyldPath, "/usr/lib/dyld", 1024);
	struct stat stat_buf;
	if ( stat(dyldPath, &stat_buf) == 0 ) {
		dyldLayout = new UniversalMachOLayout(dyldPath, &onlyArchs);
	}
	else {
		dyldLayout = new UniversalMachOLayout("/usr/lib/dyld", &onlyArchs);
	}
	const int archCount = onlyArchs.size();
	int index = 0;
	for(std::set<ArchPair>::iterator a = onlyArchs.begin(); a != onlyArchs.end(); ++a, ++index) {
		const MachOLayoutAbstraction* dyldLayoutForArch = dyldLayout->getSlice(*a);
		uint64_t dyldBaseAddress = 0;
		if ( dyldLayoutForArch != NULL )
			dyldBaseAddress = dyldLayoutForArch->getBaseAddress();
		else
			fprintf(stderr, "update_dyld_shared_cache: warning, dyld not available for specified architectures\n");
		switch ( a->arch ) {
			case CPU_TYPE_I386:
				{
					SharedCache<x86> cache(ArchGraph::graphForArchPair(*a), rootPath, overlayPath, cacheDir, explicitCacheDir, alphaSort, verify, optimize, dyldBaseAddress);
					didUpdate |= cache.update(force, optimize, deleteExistingFirst, index, archCount, keepSignatures, dontMapLocalSymbols);
				}
				break;
			case CPU_TYPE_X86_64:
				{
					SharedCache<x86_64> cache(ArchGraph::graphForArchPair(*a), rootPath, overlayPath, cacheDir, explicitCacheDir, alphaSort, verify, optimize, dyldBaseAddress);
					didUpdate |= cache.update(force, optimize, deleteExistingFirst, index, archCount, keepSignatures, dontMapLocalSymbols);
				}
				break;
			case CPU_TYPE_ARM:
				{
					SharedCache<arm> cache(ArchGraph::graphForArchPair(*a), rootPath, overlayPath, cacheDir, explicitCacheDir, alphaSort, verify, optimize, dyldBaseAddress);
					didUpdate |= cache.update(force, optimize, deleteExistingFirst, index, archCount, keepSignatures, dontMapLocalSymbols);
				}
				break;
		}
	}
	
	if ( !iPhoneOS )
		deleteOrphanTempCacheFiles();
	
	return didUpdate;
}


static void usage()
{
	fprintf(stderr, "update_dyld_shared_cache [-force] [-root dir] [-overlay dir] [-arch arch] [-debug]\n");
}


int main(int argc, const char* argv[])
{
	std::set<ArchPair> onlyArchs;
	const char* rootPath = "";
	const char* overlayPath = "";
	const char* dylibListFile = NULL;
	bool force = false;
	bool alphaSort = false;
	bool optimize = true;
	bool verify = false;
	bool keepSignatures = false;
	bool explicitCacheDir = false;
	bool dontMapLocalSymbols = false;
	const char* cacheDir = NULL;
	
	try {
		// parse command line options
		for(int i=1; i < argc; ++i) {
			const char* arg = argv[i];
			if ( arg[0] == '-' ) {
				if ( strcmp(arg, "-debug") == 0 ) {
					verbose = true;
				}
				else if ( strcmp(arg, "-force") == 0 ) {
					force = true;
				}
				else if ( strcmp(arg, "-verify") == 0 ) {
					verify = true;
				}
				else if ( strcmp(arg, "-sort_by_name") == 0 ) {
					alphaSort = true;
				}
				else if ( strcmp(arg, "-progress") == 0 ) {
					progress = true;
				}
				else if ( strcmp(arg, "-opt") == 0 ) {
					optimize = true;
				}
				else if ( strcmp(arg, "-no_opt") == 0 ) {
					optimize = false;
				}
				else if ( strcmp(arg, "-dont_map_local_symbols") == 0 ) {
					dontMapLocalSymbols = true;
				}
				else if ( strcmp(arg, "-iPhone") == 0 ) {
					iPhoneOS = true;
					alphaSort = true;
				}
				else if ( strcmp(arg, "-dylib_list") == 0 ) {
					dylibListFile = argv[++i];
					if ( dylibListFile == NULL )
						throw "-dylib_list missing path argument";
				}
				else if ( (strcmp(arg, "-root") == 0) || (strcmp(arg, "--root") == 0) ) {
					rootPath = argv[++i];
					if ( rootPath == NULL )
						throw "-root missing path argument";
				}
				else if ( strcmp(arg, "-overlay") == 0 ) {
					overlayPath = argv[++i];
					if ( overlayPath == NULL )
						throw "-overlay missing path argument";
				}
				else if ( strcmp(arg, "-cache_dir") == 0 ) {
					cacheDir = argv[++i];
					if ( cacheDir == NULL )
						throw "-cache_dir missing path argument";
					explicitCacheDir = true;
				}
				else if ( strcmp(arg, "-arch") == 0 ) {
					const char* arch = argv[++i];
					if ( strcmp(arch, "i386") == 0 )
						onlyArchs.insert(ArchPair(CPU_TYPE_I386, CPU_SUBTYPE_I386_ALL));
					else if ( strcmp(arch, "x86_64") == 0 )
						onlyArchs.insert(ArchPair(CPU_TYPE_X86_64, CPU_SUBTYPE_X86_64_ALL));
					else if ( strcmp(arch, "armv4t") == 0 )
						onlyArchs.insert(ArchPair(CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V4T));
					else if ( strcmp(arch, "armv5") == 0 )
						onlyArchs.insert(ArchPair(CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V5TEJ));
					else if ( strcmp(arch, "armv6") == 0 )
						onlyArchs.insert(ArchPair(CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V6));
					else if ( strcmp(arch, "armv7") == 0 )
						onlyArchs.insert(ArchPair(CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V7));
					else if ( strcmp(arch, "armv7f") == 0 )
						onlyArchs.insert(ArchPair(CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V7F));
					else if ( strcmp(arch, "armv7k") == 0 )
						onlyArchs.insert(ArchPair(CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V7K));
					else if ( strcmp(arch, "armv7s") == 0 )
						onlyArchs.insert(ArchPair(CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V7S));
					else 
						throwf("unknown architecture %s", arch);
				}
				else if ( strcmp(arg, "-universal_boot") == 0 ) {
					onlyArchs.insert(ArchPair(CPU_TYPE_X86_64, CPU_SUBTYPE_X86_64_ALL));
					onlyArchs.insert(ArchPair(CPU_TYPE_I386, CPU_SUBTYPE_I386_ALL));
				}
				else {
					usage();
					throwf("unknown option: %s\n", arg);
				}
			}
			else {
				usage();
				throwf("unknown option: %s\n", arg);
			}
		}
				
		// strip tailing slashes on -root 
		// make it a real path so as to not make all dylibs look like symlink aliases
		if ( rootPath[0] != '\0' ) {
			char realRootPath[MAXPATHLEN];
			if ( realpath(rootPath, realRootPath) == NULL )
				throwf("realpath() failed on %s\n", rootPath);
			rootPath = strdup(realRootPath);
		}
		
		// strip tailing slashes on -overlay
		if ( overlayPath[0] != '\0' ) {
			char realOverlayPath[MAXPATHLEN];
			if ( realpath(overlayPath, realOverlayPath) == NULL )
				throwf("realpath() failed on %s\n", overlayPath);
			overlayPath = strdup(realOverlayPath);
		}

		// set default location to write cache dir
		if ( cacheDir == NULL ) 
			cacheDir = (iPhoneOS ? IPHONE_DYLD_SHARED_CACHE_DIR : MACOSX_DYLD_SHARED_CACHE_DIR);

		// if no restrictions specified, use architectures that work on this machine
		if ( onlyArchs.size() == 0 ) {
			if ( iPhoneOS ) {
				onlyArchs.insert(ArchPair(CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V6));
				onlyArchs.insert(ArchPair(CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V7));
			}
			else {
				int available;
				size_t len = sizeof(int);
			#if __i386__ || __x86_64__
				onlyArchs.insert(ArchPair(CPU_TYPE_I386, CPU_SUBTYPE_I386_ALL));
				// check system is capable of running 64-bit programs
				if ( (sysctlbyname("hw.optional.x86_64", &available, &len, NULL, 0) == 0) && available )
					onlyArchs.insert(ArchPair(CPU_TYPE_X86_64, CPU_SUBTYPE_X86_64_ALL));
			#else
				#error unsupported architecture
			#endif
			}
		}
		
		if ( !verify && (geteuid() != 0) )
			throw "you must be root to run this tool";
		
		// build list of shared dylibs
		if ( dylibListFile != NULL )
			setSharedDylibs(rootPath, overlayPath, dylibListFile, onlyArchs);
		else
			scanForSharedDylibs(rootPath, overlayPath, "/var/db/dyld/shared_region_roots/", onlyArchs);
		bool didUpdate = updateSharedeCacheFile(rootPath, overlayPath, cacheDir, explicitCacheDir, onlyArchs, force, alphaSort, optimize,
								false, verify, keepSignatures, dontMapLocalSymbols);
								
		if ( didUpdate && !iPhoneOS ) {
			void* handle = dlopen("/usr/lib/libspindump.dylib", RTLD_LAZY);
			if ( handle != NULL ) {
				typedef bool (*dscsym_proc_t)(const char *root);
				dscsym_proc_t proc = (dscsym_proc_t)dlsym(handle, "dscsym_save_nuggets_for_current_caches");
				const char* nuggetRootPath = "/";
				if ( overlayPath[0] != '\0' ) 
					nuggetRootPath = overlayPath;
				else if ( rootPath[0] != '\0' )
					nuggetRootPath = rootPath;
				(*proc)(nuggetRootPath);
			}
			dlclose(handle);
		}
	}
	catch (const char* msg) {
		fprintf(stderr, "update_dyld_shared_cache failed: %s\n", msg);
		return 1;
	}
	
	return 0;
}



