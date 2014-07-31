/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*- 
 *
 * Copyright (c) 2006-2007 Apple Inc. All rights reserved.
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
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
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

#include "dyld_cache_format.h"

#include <vector>
#include <set>
#include <map>
#include <ext/hash_map>

#include "Architectures.hpp"
#include "MachOLayout.hpp"
#include "MachORebaser.hpp"
#include "MachOBinder.hpp"
#include "CacheFileAbstraction.hpp"

extern "C" { 
	#include "dyld_shared_cache_server.h"
}


static bool							verbose = false;
static std::vector<const char*>		warnings;


static uint64_t pageAlign(uint64_t addr) { return ( (addr + 4095) & (-4096) ); }

class ArchGraph
{
public:
	static void			addArch(cpu_type_t arch);
	static void			addRoot(const char* vpath, const std::set<cpu_type_t>& archs);
	static void			findSharedDylibs(cpu_type_t arch);
	static ArchGraph*	getArch(cpu_type_t arch) { return fgPerArchGraph[arch]; }
	static void			setFileSystemRoot(const char* root) { fgFileSystemRoot = root; }
	static const char*	archName(cpu_type_t arch);
	
	cpu_type_t											getArch() { return fArch; }
	std::set<const class MachOLayoutAbstraction*>&		getSharedDylibs() { return fSharedDylibs; }
	
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

	struct CStringEquals {
		bool operator()(const char* left, const char* right) const { return (strcmp(left, right) == 0); }
	};
	typedef __gnu_cxx::hash_map<const char*, class DependencyNode*, __gnu_cxx::hash<const char*>, CStringEquals> PathToNode;


								ArchGraph(cpu_type_t arch) : fArch(arch) {}
	static void					addRootForArch(const char* path, const MachOLayoutAbstraction*);
	void						addRoot(const char* path, const MachOLayoutAbstraction*);
	DependencyNode*				getNode(const char* path);
	DependencyNode*				getNodeForVirtualPath(const char* vpath);
	static bool					canBeShared(const MachOLayoutAbstraction* layout, cpu_type_t arch, const std::set<const MachOLayoutAbstraction*>& possibleLibs, std::map<const MachOLayoutAbstraction*, bool>& shareableMap);

	static std::map<cpu_type_t, ArchGraph*>	fgPerArchGraph;
	static const char*						fgFileSystemRoot;
	
	cpu_type_t									fArch;
	std::set<DependencyNode*>					fRoots;
	PathToNode									fNodes;
	std::set<const MachOLayoutAbstraction*>		fSharedDylibs;  // use set to avoid duplicates when installname!=realpath
};
std::map<cpu_type_t, ArchGraph*>	ArchGraph::fgPerArchGraph;
const char*							ArchGraph::fgFileSystemRoot = "";

void ArchGraph::addArch(cpu_type_t arch)
{
	//fprintf(stderr, "adding arch 0x%08X\n", arch);
	fgPerArchGraph[arch] = new ArchGraph(arch);
}

void ArchGraph::addRoot(const char* vpath, const std::set<cpu_type_t>& archs)
{
	char completePath[strlen(fgFileSystemRoot)+strlen(vpath)+2];
	const char* path;
	if ( strlen(fgFileSystemRoot) == 0 ) {
		path = vpath;
	}
	else {
		strcpy(completePath, fgFileSystemRoot);
		strcat(completePath, vpath);	// assumes vpath starts with '/'
		path = completePath;
	}
	try {
		const UniversalMachOLayout* uni = UniversalMachOLayout::find(path, &archs);
		const std::vector<MachOLayoutAbstraction*>& layouts = uni->getArchs();
		for(std::vector<MachOLayoutAbstraction*>::const_iterator it = layouts.begin(); it != layouts.end(); ++it) {
			const MachOLayoutAbstraction* layout = *it;
			if ( archs.count(layout->getArchitecture()) > 0 )
				ArchGraph::addRootForArch(path, layout);
		}
		// don't delete uni, it is owned by UniversalMachOLayout cache
	}
	catch (const char* msg) {
		fprintf(stderr, "update_dyld_shared_cache: warning can't use root %s: %s\n", path, msg);
	}
}

void ArchGraph::addRootForArch(const char* path, const MachOLayoutAbstraction* layout)
{
	ArchGraph* graph = fgPerArchGraph[layout->getArchitecture()];
	graph->addRoot(path, layout);
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
	if ( fgFileSystemRoot == NULL ) {
		return this->getNode(vpath);
	}
	else {
		char completePath[strlen(fgFileSystemRoot)+strlen(vpath)+2];
		strcpy(completePath, fgFileSystemRoot);
		strcat(completePath, vpath);	// assumes vpath starts with '/'
		return this->getNode(completePath);
	}
}

ArchGraph::DependencyNode* ArchGraph::getNode(const char* path)
{
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
	if ( pos != fNodes.end() )
		return pos->second;
	
	// still does not exist, so create a new node
	const UniversalMachOLayout* uni = UniversalMachOLayout::find(realPath);
	DependencyNode* node = new DependencyNode(this, realPath, uni->getArch(fArch));
	if ( node->getLayout() == NULL ) {
		throwf("%s is missing arch %s", realPath, archName(fArch));
	}
	// add realpath to node map
	fNodes[node->getPath()] = node;
	// if install name is not real path, add install name to node map
	if ( (node->getLayout()->getFileType() == MH_DYLIB) && (strcmp(realPath, node->getLayout()->getID().name) != 0) ) {
		//fprintf(stderr, "adding node alias 0x%08X %s for %s\n", fArch, node->getLayout()->getID().name, realPath);
		fNodes[node->getLayout()->getID().name] = node;
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
					strcpy(newPath, executablePath);
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
					strcpy(newPath, fPath);
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
				fDependsOn.insert(fGraph->getNodeForVirtualPath(dependentPath));
			}
			catch (const char* msg) {
				fprintf(stderr, "warning, could not bind %s because %s\n", fPath, msg);
				fDependentMissing = true;
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

void ArchGraph::findSharedDylibs(cpu_type_t arch)
{
	const PathToNode& nodes = fgPerArchGraph[arch]->fNodes;
	std::set<const MachOLayoutAbstraction*> possibleLibs;
	//fprintf(stderr, "shared for arch 0x%08X\n", arch);
	for(PathToNode::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
		DependencyNode* node = it->second;
		if ( node->allDependentsFound() && (node->useCount() > 1) ) {
			if ( node->getLayout()->hasSplitSegInfo() ) 
				possibleLibs.insert(node->getLayout());
			//fprintf(stderr, "\t%s\n", it->first);
		}
	}
	
	// prune so that all shareable libs depend only on other shareable libs
	std::set<const MachOLayoutAbstraction*>& sharedLibs = fgPerArchGraph[arch]->fSharedDylibs;
	std::map<const MachOLayoutAbstraction*,bool> shareableMap;
	for (std::set<const MachOLayoutAbstraction*>::iterator lit = possibleLibs.begin(); lit != possibleLibs.end(); ++lit) {
		if ( canBeShared(*lit, arch, possibleLibs, shareableMap) )
			sharedLibs.insert(*lit);
	}
}

const char*	ArchGraph::archName(cpu_type_t arch)
{
	switch ( arch ) {
		case CPU_TYPE_POWERPC:
			return "ppc";
		case CPU_TYPE_POWERPC64:
			return "ppc64";
		case CPU_TYPE_I386:
			return "i386";
		case CPU_TYPE_X86_64:
			return "x86_64";
		default:
			return "unknown";
	}
}

bool ArchGraph::canBeShared(const MachOLayoutAbstraction* layout, cpu_type_t arch, const std::set<const MachOLayoutAbstraction*>& possibleLibs, std::map<const MachOLayoutAbstraction*, bool>& shareableMap)
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
		if ( ! layout->hasSplitSegInfo() )
			asprintf(&msg, "can't put %s in shared cache because it was not built for 10.5", layout->getID().name);
		else
			asprintf(&msg, "can't put %s in shared cache", layout->getID().name);
		warnings.push_back(msg);
		if ( verbose )
			fprintf(stderr, "update_dyld_shared_cache: for arch %s, %s\n", archName(arch), msg);
		return false;
	}
	// look recursively
	shareableMap[layout] = true; // mark this shareable early in case of circular references
	const PathToNode& nodes = fgPerArchGraph[arch]->fNodes;
	const std::vector<MachOLayoutAbstraction::Library>&	dependents = layout->getLibraries();
	for (std::vector<MachOLayoutAbstraction::Library>::const_iterator dit = dependents.begin(); dit != dependents.end(); ++dit) {
		PathToNode::const_iterator pos = nodes.find(dit->name);
		if ( pos == nodes.end() ) {
			shareableMap[layout] = false;
			char* msg;
			asprintf(&msg, "can't put %s in shared cache because it depends on %s which can't be found", layout->getID().name, dit->name);
			warnings.push_back(msg);
			if ( verbose )
				fprintf(stderr, "update_dyld_shared_cache: for arch %s, %s\n", archName(arch), msg);
			return false;
		}
		else {
			if ( ! canBeShared(pos->second->getLayout(), arch, possibleLibs, shareableMap) ) {
				shareableMap[layout] = false;
				char* msg;
				asprintf(&msg, "can't put %s in shared cache because it depends on %s which can't be in shared cache", layout->getID().name, dit->name);
				warnings.push_back(msg);
				if ( verbose )
					fprintf(stderr, "update_dyld_shared_cache: for arch %s, %s\n", archName(arch), msg);
				return false;
			}
		}
	}
	return true;
}


template <typename A>
class SharedCache
{
public:
							SharedCache(ArchGraph* graph, bool alphaSort, uint64_t dyldBaseAddress);
	bool					update(const char* rootPath, const char* cacheDir, bool force, bool optimize, bool deleteExistingFirst, int archIndex, int archCount);
	static const char*		filename(bool optimized);

private:
	typedef typename A::P::E	E;

	bool					notUpToDate(const char* cachePath);
	bool					notUpToDate(const void* cache);
	uint8_t*				optimizeLINKEDIT();

	static void				getSharedCacheBasAddresses(cpu_type_t arch, uint64_t* baseReadOnly, uint64_t* baseWritable);
	static cpu_type_t		arch();
	static const char*		archName();
	static uint64_t			sharedRegionReadOnlyStartAddress();
	static uint64_t			sharedRegionWritableStartAddress();
	static uint64_t			sharedRegionReadOnlySize();
	static uint64_t			sharedRegionWritableSize();
	static uint64_t			getWritableSegmentNewAddress(uint64_t proposedNewAddress, uint64_t originalAddress, uint64_t executableSlide);
	
	
	void					assignNewBaseAddresses();
	uint64_t				cacheFileOffsetForAddress(uint64_t addr);

	struct LayoutInfo {
		const MachOLayoutAbstraction*		layout;
		dyld_cache_image_info				info;
	};
	
	struct ByNameSorter {
		bool operator()(const LayoutInfo& left, const LayoutInfo& right) 
				{ return (strcmp(left.layout->getID().name, right.layout->getID().name) < 0); }
	};

	struct RandomSorter {
		RandomSorter(const std::vector<LayoutInfo>& infos) {
			for(typename std::vector<struct LayoutInfo>::const_iterator it = infos.begin(); it != infos.end(); ++it) 
				fMap[it->layout] = arc4random();
		}
		bool operator()(const LayoutInfo& left, const LayoutInfo& right) {
			return (fMap[left.layout] < fMap[right.layout]); 
		}
	private:
		std::map<const MachOLayoutAbstraction*, uint32_t> fMap;
	};
	

	ArchGraph*							fArchGraph;
	std::vector<LayoutInfo>				fDylibs;
	std::vector<shared_file_mapping_np>	fMappings;
	uint32_t							fHeaderSize;
	uint64_t							fDyldBaseAddress;
	uint64_t							fLinkEditsTotalUnoptimizedSize;
	uint64_t							fLinkEditsStartAddress;
	MachOLayoutAbstraction::Segment*	fFirstLinkEditSegment;
};



	
template <>	 cpu_type_t	SharedCache<ppc>::arch()	{ return CPU_TYPE_POWERPC; }
template <>	 cpu_type_t	SharedCache<ppc64>::arch()	{ return CPU_TYPE_POWERPC64; }
template <>	 cpu_type_t	SharedCache<x86>::arch()	{ return CPU_TYPE_I386; }
template <>	 cpu_type_t	SharedCache<x86_64>::arch()	{ return CPU_TYPE_X86_64; }

template <>	 uint64_t	SharedCache<ppc>::sharedRegionReadOnlyStartAddress()	{ return 0x90000000; }
template <>	 uint64_t	SharedCache<ppc64>::sharedRegionReadOnlyStartAddress()	{ return 0x7FFF80000000LL; }
template <>	 uint64_t	SharedCache<x86>::sharedRegionReadOnlyStartAddress()	{ return 0x90000000; }
template <>	 uint64_t	SharedCache<x86_64>::sharedRegionReadOnlyStartAddress()	{ return 0x7FFF80000000LL; }

template <>	 uint64_t	SharedCache<ppc>::sharedRegionWritableStartAddress()	{ return 0xA0000000; }
template <>	 uint64_t	SharedCache<ppc64>::sharedRegionWritableStartAddress()	{ return 0x7FFF70000000LL; }
template <>	 uint64_t	SharedCache<x86>::sharedRegionWritableStartAddress()	{ return 0xA0000000; }
template <>	 uint64_t	SharedCache<x86_64>::sharedRegionWritableStartAddress()	{ return 0x7FFF70000000LL; }

template <>	 uint64_t	SharedCache<ppc>::sharedRegionReadOnlySize()			{ return 0x10000000; }
template <>	 uint64_t	SharedCache<ppc64>::sharedRegionReadOnlySize()			{ return 0x7FE00000; }
template <>	 uint64_t	SharedCache<x86>::sharedRegionReadOnlySize()			{ return 0x10000000; }
template <>	 uint64_t	SharedCache<x86_64>::sharedRegionReadOnlySize()			{ return 0x7FE00000; }

template <>	 uint64_t	SharedCache<ppc>::sharedRegionWritableSize()			{ return 0x10000000; }
template <>	 uint64_t	SharedCache<ppc64>::sharedRegionWritableSize()			{ return 0x20000000; }
template <>	 uint64_t	SharedCache<x86>::sharedRegionWritableSize()			{ return 0x10000000; }
template <>	 uint64_t	SharedCache<x86_64>::sharedRegionWritableSize()			{ return 0x20000000; }


template <>	 const char*	SharedCache<ppc>::archName()	{ return "ppc"; }
template <>	 const char*	SharedCache<ppc64>::archName()	{ return "ppc64"; }
template <>	 const char*	SharedCache<x86>::archName()	{ return "i386"; }
template <>	 const char*	SharedCache<x86_64>::archName()	{ return "x86_64"; }

template <>	 const char*	SharedCache<ppc>::filename(bool optimized)	{ return optimized ? "ppc" : "rosetta"; }
template <>	 const char*	SharedCache<ppc64>::filename(bool)	{ return "ppc64"; }
template <>	 const char*	SharedCache<x86>::filename(bool)	{ return "i386"; }
template <>	 const char*	SharedCache<x86_64>::filename(bool)	{ return "x86_64"; }

template <typename A>
SharedCache<A>::SharedCache(ArchGraph* graph, bool alphaSort, uint64_t dyldBaseAddress) 
  : fArchGraph(graph), fDyldBaseAddress(dyldBaseAddress)
{
	if ( fArchGraph->getArch() != arch() )
		throw "wrong architecture";
	
	// build vector of all shared dylibs
	std::set<const MachOLayoutAbstraction*>& dylibs = fArchGraph->getSharedDylibs();
	for(std::set<const MachOLayoutAbstraction*>::iterator it = dylibs.begin(); it != dylibs.end(); ++it) {
		const MachOLayoutAbstraction* lib = *it;
		LayoutInfo temp;
		temp.layout = lib;
		temp.info.address = 0;
		temp.info.modTime = lib->getLastModTime();
		temp.info.inode = lib->getInode();
		temp.info.pathFileOffset = lib->getNameFileOffset();
		fDylibs.push_back(temp);
	}
	
	// sort shared dylibs
	if ( alphaSort )
		std::sort(fDylibs.begin(), fDylibs.end(), ByNameSorter());
	else
		std::sort(fDylibs.begin(), fDylibs.end(), RandomSorter(fDylibs));
		
	
	// assign segments in each dylib a new address
	this->assignNewBaseAddresses();
	
	// calculate cache file header size
	fHeaderSize = pageAlign(sizeof(dyld_cache_header) 
							+ fMappings.size()*sizeof(shared_file_mapping_np) 
							+ fDylibs.size()*sizeof(dyld_cache_image_info) );
							//+ fDependencyPool.size()*sizeof(uint16_t));
	
	if ( fHeaderSize > 0x3000 )
		throwf("header size miscalculation 0x%08X", fHeaderSize);
}


template <typename A>
uint64_t SharedCache<A>::getWritableSegmentNewAddress(uint64_t proposedNewAddress, uint64_t originalAddress, uint64_t executableSlide)
{
	return proposedNewAddress;
}

template <>
uint64_t SharedCache<ppc>::getWritableSegmentNewAddress(uint64_t proposedNewAddress, uint64_t originalAddress, uint64_t executableSlide)
{
	// for ppc64 writable segments can only move in increments of 64K (so only hi16 instruction needs to be modified)
	return (((executableSlide & 0x000000000000F000ULL) - ((proposedNewAddress - originalAddress) & 0x000000000000F000ULL)) & 0x000000000000F000ULL) + proposedNewAddress;
}

template <>
uint64_t SharedCache<ppc64>::getWritableSegmentNewAddress(uint64_t proposedNewAddress, uint64_t originalAddress, uint64_t executableSlide)
{
	// for ppc64 writable segments can only move in increments of 64K (so only hi16 instruction needs to be modified)
	return (((executableSlide & 0x000000000000F000ULL) - ((proposedNewAddress - originalAddress) & 0x000000000000F000ULL)) & 0x000000000000F000ULL) + proposedNewAddress;
}


template <typename A>
void SharedCache<A>::assignNewBaseAddresses()
{
	// first layout TEXT and DATA for split-seg (or can be split-seg) dylibs
	uint64_t currentExecuteAddress = sharedRegionReadOnlyStartAddress() + 0x3000;	
	uint64_t currentWritableAddress = sharedRegionWritableStartAddress();
	for(typename std::vector<LayoutInfo>::iterator it = fDylibs.begin(); it != fDylibs.end(); ++it) {
		std::vector<MachOLayoutAbstraction::Segment>& segs = ((MachOLayoutAbstraction*)(it->layout))->getSegments();
		MachOLayoutAbstraction::Segment* executableSegment = NULL;
		for (int i=0; i < segs.size(); ++i) {
			MachOLayoutAbstraction::Segment& seg = segs[i];
			if ( seg.writable() ) {
				if ( seg.executable() && it->layout->hasSplitSegInfo() ) {
					// skip __IMPORT segments in this pass
				}
				else {
					// __DATA segment
					// for ppc, writable segments have to move in 64K increments
					if (  it->layout->hasSplitSegInfo() ) {
						if ( executableSegment == NULL )
							throwf("first segment in dylib is not executable for %s", it->layout->getID().name);
						seg.setNewAddress(getWritableSegmentNewAddress(currentWritableAddress, seg.address(), executableSegment->newAddress() - executableSegment->address()));
					}
					else
						seg.setNewAddress(currentWritableAddress);
					currentWritableAddress = pageAlign(seg.newAddress() + seg.size());
				}
			}
			else {
				if ( seg.executable() ) {
					// __TEXT segment
					if ( it->info.address == 0 )
						it->info.address = currentExecuteAddress;
					executableSegment = &seg;
					seg.setNewAddress(currentExecuteAddress);
					currentExecuteAddress += pageAlign(seg.size());
				}
				else {
					// skip read-only segments in this pass
					// any non-LINKEDIT read-only segments leave a hole so that all R/W segment slide together
					if ( (strcmp(seg.name(), "__LINKEDIT") != 0) && (i < (segs.size()-2)) ) {
						fprintf(stderr, "update_dyld_shared_cache: warning %s segment in %s leaves a hole\n", seg.name(), it->layout->getID().name);
						currentWritableAddress = pageAlign(currentWritableAddress + seg.size());
					}
				}
			}
		}
	}

	// append all read-only (but not LINKEDIT) segments at end of all TEXT segments
	// append all IMPORT segments at end of all DATA segments rounded to next 2MB 
	uint64_t currentReadOnlyAddress = currentExecuteAddress;
	uint64_t startWritableExecutableAddress = (currentWritableAddress + 0x200000 - 1) & (-0x200000);
	uint64_t currentWritableExecutableAddress = startWritableExecutableAddress;
	for(typename std::vector<LayoutInfo>::iterator it = fDylibs.begin(); it != fDylibs.end(); ++it) {
		std::vector<MachOLayoutAbstraction::Segment>& segs = ((MachOLayoutAbstraction*)(it->layout))->getSegments();
		for(int i=0; i < segs.size(); ++i) {
			MachOLayoutAbstraction::Segment& seg = segs[i];
			if ( !seg.writable() && !seg.executable() && (strcmp(seg.name(), "__LINKEDIT") != 0) ) {
				// allocate non-executable,read-only segments from end of read only shared region
				seg.setNewAddress(currentReadOnlyAddress);
				currentReadOnlyAddress += pageAlign(seg.size());
			}
			else if ( seg.writable() && seg.executable() && it->layout->hasSplitSegInfo() ) {
				// allocate IMPORT segments to end of writable shared region
				seg.setNewAddress(currentWritableExecutableAddress);
				seg.setWritable(false); // __IMPORT segments are not-writable in shared cache
				currentWritableExecutableAddress += pageAlign(seg.size());
			}
		}
	}	

	// append all LINKEDIT segments at end of all read-only segments
	fLinkEditsStartAddress = currentReadOnlyAddress;
	fFirstLinkEditSegment = NULL;
	for(typename std::vector<LayoutInfo>::iterator it = fDylibs.begin(); it != fDylibs.end(); ++it) {
		std::vector<MachOLayoutAbstraction::Segment>& segs = ((MachOLayoutAbstraction*)(it->layout))->getSegments();
		for(int i=0; i < segs.size(); ++i) {
			MachOLayoutAbstraction::Segment& seg = segs[i];
			if ( !seg.writable() && !seg.executable() && (strcmp(seg.name(), "__LINKEDIT") == 0) ) {
				if ( fFirstLinkEditSegment == NULL ) 
					fFirstLinkEditSegment = &seg;
				// allocate non-executable,read-only segments from end of read only shared region
				seg.setNewAddress(currentReadOnlyAddress);
				currentReadOnlyAddress += pageAlign(seg.size());
			}
		}
	}
	fLinkEditsTotalUnoptimizedSize = (currentReadOnlyAddress - fLinkEditsStartAddress + 4095) & (-4096);


	// populate large mappings
	uint64_t cacheFileOffset = 0;
	if ( currentExecuteAddress > sharedRegionReadOnlyStartAddress() + 0x3000 ) {
		shared_file_mapping_np  executeMapping;
		executeMapping.sfm_address		= sharedRegionReadOnlyStartAddress();
		executeMapping.sfm_size			= currentExecuteAddress - sharedRegionReadOnlyStartAddress();
		executeMapping.sfm_file_offset	= cacheFileOffset;
		executeMapping.sfm_max_prot		= VM_PROT_READ | VM_PROT_EXECUTE;
		executeMapping.sfm_init_prot	= VM_PROT_READ | VM_PROT_EXECUTE;
		fMappings.push_back(executeMapping);
		cacheFileOffset += executeMapping.sfm_size;
		
		shared_file_mapping_np  writableMapping;
		writableMapping.sfm_address		= sharedRegionWritableStartAddress();
		writableMapping.sfm_size		= currentWritableAddress - sharedRegionWritableStartAddress();
		writableMapping.sfm_file_offset	= cacheFileOffset;
		writableMapping.sfm_max_prot	= VM_PROT_READ | VM_PROT_WRITE;
		writableMapping.sfm_init_prot	= VM_PROT_READ | VM_PROT_WRITE;
		fMappings.push_back(writableMapping);
		cacheFileOffset += writableMapping.sfm_size;
		
		if ( currentWritableExecutableAddress > startWritableExecutableAddress ) {
			shared_file_mapping_np  writableExecutableMapping;
			writableExecutableMapping.sfm_address	= startWritableExecutableAddress;
			writableExecutableMapping.sfm_size		= currentWritableExecutableAddress - startWritableExecutableAddress;
			writableExecutableMapping.sfm_file_offset= cacheFileOffset;
			writableExecutableMapping.sfm_max_prot	= VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE;
			// __IMPORT segments in shared cache are not writable 
			writableExecutableMapping.sfm_init_prot	= VM_PROT_READ | VM_PROT_EXECUTE; 
			fMappings.push_back(writableExecutableMapping);
			cacheFileOffset += writableExecutableMapping.sfm_size;
		}
		
		// make read-only (contains LINKEDIT segments) last, so it can be cut back when optimized
		shared_file_mapping_np  readOnlyMapping;
		readOnlyMapping.sfm_address		= currentExecuteAddress;
		readOnlyMapping.sfm_size		= currentReadOnlyAddress - currentExecuteAddress;
		readOnlyMapping.sfm_file_offset	= cacheFileOffset;
		readOnlyMapping.sfm_max_prot	= VM_PROT_READ;
		readOnlyMapping.sfm_init_prot	= VM_PROT_READ;
		fMappings.push_back(readOnlyMapping);
		cacheFileOffset += readOnlyMapping.sfm_size;
	}
	else {
		// empty cache
		shared_file_mapping_np  cacheHeaderMapping;
		cacheHeaderMapping.sfm_address		= sharedRegionWritableStartAddress();
		cacheHeaderMapping.sfm_size			= 0x3000;
		cacheHeaderMapping.sfm_file_offset	= cacheFileOffset;
		cacheHeaderMapping.sfm_max_prot		= VM_PROT_READ;
		cacheHeaderMapping.sfm_init_prot	= VM_PROT_READ;
		fMappings.push_back(cacheHeaderMapping);
		cacheFileOffset += cacheHeaderMapping.sfm_size;
	}
}


template <typename A>
uint64_t SharedCache<A>::cacheFileOffsetForAddress(uint64_t addr)
{
	for(std::vector<shared_file_mapping_np>::iterator it = fMappings.begin(); it != fMappings.end(); ++it) {
		if ( (it->sfm_address <= addr) && (addr < it->sfm_address+it->sfm_size) )
			return it->sfm_file_offset + addr - it->sfm_address;
	}
	throwf("address 0x%0llX is not in cache", addr);
}


template <typename A>
bool SharedCache<A>::notUpToDate(const void* cache)
{
	dyldCacheHeader<E>* header = (dyldCacheHeader<E>*)cache;
	// not valid if header signature is wrong
	char temp[16];
	strcpy(temp, "dyld_v1        ");
	strcpy(&temp[15-strlen(archName())], archName());
	if ( strcmp(header->magic(), temp) != 0 ) 
		return true;
	// not valid if count of images does not match current images needed
	if ( header->imagesCount() != fDylibs.size() )
		return true;
	// verify every dylib in constructed graph is in existing cache with same inode and modTime	
	const dyldCacheImageInfo<E>* imagesStart = (dyldCacheImageInfo<E>*)((uint8_t*)cache + header->imagesOffset());
	const dyldCacheImageInfo<E>* imagesEnd = &imagesStart[header->imagesCount()];
	for(typename std::vector<LayoutInfo>::iterator it = fDylibs.begin(); it != fDylibs.end(); ++it) {
		bool found = false;
		//fprintf(stderr, "inode=0x%llX, mTime=0x%llX, path=%s\n", it->info.inode, it->info.modTime, it->layout->getID().name);
		for(const dyldCacheImageInfo<E>* cacheEntry = imagesStart; cacheEntry < imagesEnd; ++cacheEntry) {
			if ( (cacheEntry->inode() == it->info.inode) 
			 && (cacheEntry->modTime() == it->info.modTime) 
			 && (strcmp((char*)cache+cacheEntry->pathFileOffset(), it->layout->getID().name) == 0) ) {
					found = true;
					break;
			}
		}
		if ( !found ) {
			fprintf(stderr, "update_dyld_shared_cache[%u] current cache invalid because %s has changed\n", getpid(), it->layout->getID().name);
			return true;
		}
	}
	return false;
}


template <typename A>
bool SharedCache<A>::notUpToDate(const char* cachePath)
{
	// mmap existing cache file 
	int fd = ::open(cachePath, O_RDONLY);	
	if ( fd == -1 )
		return true;
	struct stat stat_buf;
	::fstat(fd, &stat_buf);
	uint8_t* mappingAddr = (uint8_t*)mmap(NULL, stat_buf.st_size, PROT_READ , MAP_FILE | MAP_PRIVATE, fd, 0);
	::close(fd);
	if ( mappingAddr == (uint8_t*)(-1) )
		return true;

	// validate it
	bool result = this->notUpToDate(mappingAddr);
	// unmap
	::munmap(mappingAddr, stat_buf.st_size);
	if ( verbose && !result )
		fprintf(stderr, "update_dyld_shared_cache: %s is up-to-date\n", cachePath);

	return result;
}

class CStringEquals
{
public:
	bool operator()(const char* left, const char* right) const { return (strcmp(left, right) == 0); }
};

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
	typedef __gnu_cxx::hash_map<const char*, uint32_t, __gnu_cxx::hash<const char*>, CStringEquals> StringToOffset;

	char*			fBuffer;
	uint32_t		fBufferAllocated;
	uint32_t		fBufferUsed;
	StringToOffset	fUniqueStrings;
};


StringPool::StringPool() 
	: fBufferUsed(0), fBufferAllocated(4*1024*1024)
{
	fBuffer = (char*)malloc(fBufferAllocated);
}

uint32_t StringPool::add(const char* str)
{
	uint32_t len = strlen(str);
	if ( (fBufferUsed + len + 1) > fBufferAllocated ) {
		// grow buffer
		fBufferAllocated = fBufferAllocated*2;
		fBuffer = (char*)realloc(fBuffer, fBufferAllocated);
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


template <typename A>
class LinkEditOptimizer
{
public:
											LinkEditOptimizer(const MachOLayoutAbstraction&, uint8_t*, StringPool&);
	virtual									~LinkEditOptimizer() {}

	static void								makeDummyLocalSymbol(uint32_t&, uint8_t*, StringPool&);
		void								copyLocalSymbols();
		void								copyExportedSymbols(uint32_t&);
		void								copyImportedSymbols(uint32_t&);
		void								copyExternalRelocations(uint32_t&);
		void								copyIndirectSymbolTable(uint32_t&);
		void								updateLoadCommands(uint64_t newVMAddress, uint64_t size, uint32_t stringPoolOffset);
	

protected:
	typedef typename A::P					P;
	typedef typename A::P::E				E;
	typedef typename A::P::uint_t			pint_t;
			
private:

	const macho_header<P>*						fHeader; 
	uint8_t*									fNewLinkEditStart;	
	uint8_t*									fLinkEditBase;		
	const MachOLayoutAbstraction&				fLayout;
	macho_dysymtab_command<P>*					fDynamicSymbolTable;
	macho_symtab_command<P>*					fSymbolTableLoadCommand;
	const macho_nlist<P>*						fSymbolTable;
	const char*									fStrings;
	StringPool&									fNewStringPool;
	std::map<uint32_t,uint32_t>					fOldToNewSymbolIndexes;
	uint32_t									fLocalSymbolsStartIndexInNewLinkEdit;
	uint32_t									fLocalSymbolsCountInNewLinkEdit;
	uint32_t									fExportedSymbolsStartIndexInNewLinkEdit;
	uint32_t									fExportedSymbolsCountInNewLinkEdit;
	uint32_t									fImportSymbolsStartIndexInNewLinkEdit;
	uint32_t									fImportedSymbolsCountInNewLinkEdit;
	uint32_t									fExternalRelocationsOffsetIntoNewLinkEdit;
	uint32_t									fIndirectSymbolTableOffsetInfoNewLinkEdit;
	static int32_t								fgLocalSymbolsStartIndexInNewLinkEdit;
};

template <typename A> int32_t LinkEditOptimizer<A>::fgLocalSymbolsStartIndexInNewLinkEdit = 0;


template <typename A>
LinkEditOptimizer<A>::LinkEditOptimizer(const MachOLayoutAbstraction& layout, uint8_t* newLinkEdit, StringPool& stringPool)
 : 	fLayout(layout), fLinkEditBase(NULL), fNewLinkEditStart(newLinkEdit), 
	fDynamicSymbolTable(NULL), fSymbolTableLoadCommand(NULL), fSymbolTable(NULL), fStrings(NULL), fNewStringPool(stringPool),
	fLocalSymbolsStartIndexInNewLinkEdit(0), fLocalSymbolsCountInNewLinkEdit(0),
	fExportedSymbolsStartIndexInNewLinkEdit(0), fExportedSymbolsCountInNewLinkEdit(0),
	fImportSymbolsStartIndexInNewLinkEdit(0), fImportedSymbolsCountInNewLinkEdit(0),
	fExternalRelocationsOffsetIntoNewLinkEdit(0), fIndirectSymbolTableOffsetInfoNewLinkEdit(0)
	
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
void LinkEditOptimizer<A>::makeDummyLocalSymbol(uint32_t& symbolIndex, uint8_t* storage, StringPool& pool)
{
	fgLocalSymbolsStartIndexInNewLinkEdit = symbolIndex;
	macho_nlist<P>* newSymbolEntry = (macho_nlist<P>*)storage;
	newSymbolEntry->set_n_strx(pool.add("__no_local_symbols_in_dyld_shared_cache"));
	newSymbolEntry->set_n_type(N_SECT);
	newSymbolEntry->set_n_sect(1);
	newSymbolEntry->set_n_desc(0);
	newSymbolEntry->set_n_value(0);
	++symbolIndex;
}

template <typename A>
void LinkEditOptimizer<A>::copyLocalSymbols()
{
	if ( fDynamicSymbolTable->nlocalsym() > 0 ) {
		// if image has any local symbols, make cache look like it has one local symbol
		// which is actually shared by all images
		fLocalSymbolsCountInNewLinkEdit = 1; 
		fLocalSymbolsStartIndexInNewLinkEdit = fgLocalSymbolsStartIndexInNewLinkEdit;
	}
}


template <typename A>
void LinkEditOptimizer<A>::copyExportedSymbols(uint32_t& symbolIndex)
{
	fExportedSymbolsStartIndexInNewLinkEdit = symbolIndex;
	const macho_nlist<P>* const firstExport = &fSymbolTable[fDynamicSymbolTable->iextdefsym()];
	const macho_nlist<P>* const lastExport  = &fSymbolTable[fDynamicSymbolTable->iextdefsym()+fDynamicSymbolTable->nextdefsym()];
	uint32_t oldIndex = fDynamicSymbolTable->iextdefsym();
	for (const macho_nlist<P>* entry = firstExport; entry < lastExport; ++entry, ++oldIndex) {
		if ( ((entry->n_type() & N_TYPE) == N_SECT) && (strncmp(&fStrings[entry->n_strx()], ".objc_", 6) != 0) ) {
			macho_nlist<P>* newSymbolEntry = &((macho_nlist<P>*)fNewLinkEditStart)[symbolIndex];
			*newSymbolEntry = *entry;
			newSymbolEntry->set_n_strx(fNewStringPool.add(&fStrings[entry->n_strx()]));
			fOldToNewSymbolIndexes[oldIndex] = symbolIndex;
			++symbolIndex;
		}
	}
	fExportedSymbolsCountInNewLinkEdit = symbolIndex - fExportedSymbolsStartIndexInNewLinkEdit;
	//fprintf(stderr, "%u exports starting at %u for %s\n", fExportedSymbolsCountInNewLinkEdit, fExportedSymbolsStartIndexInNewLinkEdit, fLayout.getFilePath());
	// sort by name, so that dyld does not need a toc
	macho_nlist<P>* newSymbolsStart = &((macho_nlist<P>*)fNewLinkEditStart)[fExportedSymbolsStartIndexInNewLinkEdit];
	macho_nlist<P>* newSymbolsEnd = &((macho_nlist<P>*)fNewLinkEditStart)[fExportedSymbolsStartIndexInNewLinkEdit+fExportedSymbolsCountInNewLinkEdit];
	std::sort(newSymbolsStart, newSymbolsEnd, SymbolSorter<A>(fNewStringPool));
	//for (macho_nlist<P>* entry = newSymbolsStart; entry < newSymbolsEnd; ++entry)
	//	fprintf(stderr, "\t%u\t %s\n", (entry-newSymbolsStart)+fExportedSymbolsStartIndexInNewLinkEdit, fNewStringPool.stringAtIndex(entry->n_strx()));
}


template <typename A>
void LinkEditOptimizer<A>::copyImportedSymbols(uint32_t& symbolIndex)
{
	fImportSymbolsStartIndexInNewLinkEdit = symbolIndex;
	const macho_nlist<P>* const firstImport = &fSymbolTable[fDynamicSymbolTable->iundefsym()];
	const macho_nlist<P>* const lastImport  = &fSymbolTable[fDynamicSymbolTable->iundefsym()+fDynamicSymbolTable->nundefsym()];
	uint32_t oldIndex = fDynamicSymbolTable->iundefsym();
	for (const macho_nlist<P>* entry = firstImport; entry < lastImport; ++entry, ++oldIndex) {
		if ( ((entry->n_type() & N_TYPE) == N_UNDF) && (strncmp(&fStrings[entry->n_strx()], ".objc_", 6) != 0) ) {
			macho_nlist<P>* newSymbolEntry = &((macho_nlist<P>*)fNewLinkEditStart)[symbolIndex];
			*newSymbolEntry = *entry;
			newSymbolEntry->set_n_strx(fNewStringPool.addUnique(&fStrings[entry->n_strx()]));
			fOldToNewSymbolIndexes[oldIndex] = symbolIndex;
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
void LinkEditOptimizer<A>::updateLoadCommands(uint64_t newVMAddress, uint64_t size, uint32_t stringPoolOffset)
{
	// set LINKEDIT segment commmand to new merged LINKEDIT
	const macho_load_command<P>* const cmds = (macho_load_command<P>*)((uint8_t*)fHeader + sizeof(macho_header<P>));
	const uint32_t cmd_count = fHeader->ncmds();
	const macho_load_command<P>* cmd = cmds;
	uint32_t linkEditStartFileOffset = 0;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		if ( cmd->cmd() == macho_segment_command<P>::CMD ) {
			macho_segment_command<P>* seg = (macho_segment_command<P>*)cmd;
			if ( strcmp(seg->segname(), "__LINKEDIT") == 0 ) {
				seg->set_vmaddr(newVMAddress);
				seg->set_vmsize(size);
				seg->set_filesize(size);
				linkEditStartFileOffset = seg->fileoff();
			}
		}
		cmd = (const macho_load_command<P>*)(((uint8_t*)cmd)+cmd->cmdsize());
	}	
		
	// update symbol table and dynamic symbol table with new offsets
	fSymbolTableLoadCommand->set_symoff(linkEditStartFileOffset);
	fSymbolTableLoadCommand->set_nsyms(fExportedSymbolsCountInNewLinkEdit+fImportedSymbolsCountInNewLinkEdit);
	fSymbolTableLoadCommand->set_stroff(linkEditStartFileOffset+stringPoolOffset);
	fSymbolTableLoadCommand->set_strsize(fNewStringPool.size());
	fDynamicSymbolTable->set_ilocalsym(fLocalSymbolsStartIndexInNewLinkEdit);
	fDynamicSymbolTable->set_nlocalsym(fLocalSymbolsCountInNewLinkEdit);
	fDynamicSymbolTable->set_iextdefsym(fExportedSymbolsStartIndexInNewLinkEdit);
	fDynamicSymbolTable->set_nextdefsym(fExportedSymbolsCountInNewLinkEdit);
	fDynamicSymbolTable->set_iundefsym(fImportSymbolsStartIndexInNewLinkEdit);
	fDynamicSymbolTable->set_nundefsym(fImportedSymbolsCountInNewLinkEdit);
	fDynamicSymbolTable->set_tocoff(0);
	fDynamicSymbolTable->set_ntoc(0);
	fDynamicSymbolTable->set_modtaboff(0);
	fDynamicSymbolTable->set_nmodtab(0);
	fDynamicSymbolTable->set_indirectsymoff(linkEditStartFileOffset+fIndirectSymbolTableOffsetInfoNewLinkEdit);
	fDynamicSymbolTable->set_extreloff(linkEditStartFileOffset+fExternalRelocationsOffsetIntoNewLinkEdit);
	fDynamicSymbolTable->set_locreloff(0);
	fDynamicSymbolTable->set_nlocrel(0);
}



template <typename A>
uint8_t* SharedCache<A>::optimizeLINKEDIT()
{
	// allocate space for optimized LINKEDIT area
	uint8_t* newLinkEdit = new uint8_t[fLinkEditsTotalUnoptimizedSize];
	bzero(newLinkEdit, fLinkEditsTotalUnoptimizedSize);
	
	// make a string pool 
	StringPool stringPool;
	
	// create optimizer object for each LINKEDIT segment
	std::vector<LinkEditOptimizer<A>*> optimizers;
	for(typename std::vector<LayoutInfo>::const_iterator it = fDylibs.begin(); it != fDylibs.end(); ++it) {
		optimizers.push_back(new LinkEditOptimizer<A>(*it->layout, newLinkEdit, stringPool));
	}

	// copy local symbol table entries
	uint32_t symbolTableIndex = 0;
	LinkEditOptimizer<A>::makeDummyLocalSymbol(symbolTableIndex, newLinkEdit, stringPool);
	for(typename std::vector<LinkEditOptimizer<A>*>::iterator it = optimizers.begin(); it != optimizers.end(); ++it) {
		(*it)->copyLocalSymbols();
	}

	// copy exported symbol table entries
	for(typename std::vector<LinkEditOptimizer<A>*>::iterator it = optimizers.begin(); it != optimizers.end(); ++it) {
		(*it)->copyExportedSymbols(symbolTableIndex);
	}
	//fprintf(stderr, "%u exported symbols, with %d bytes of strings\n", symbolTableIndex, stringPool.size());
	//uint32_t importStart = symbolTableIndex;
	//uint32_t importPoolStart =  stringPool.size();
	
	// copy imported symbol table entries
	for(typename std::vector<LinkEditOptimizer<A>*>::iterator it = optimizers.begin(); it != optimizers.end(); ++it) {
		(*it)->copyImportedSymbols(symbolTableIndex);
	}
	//fprintf(stderr, "%u imported symbols, with %d bytes of strings\n", symbolTableIndex-importStart, stringPool.size()-importPoolStart);
	
	// copy external relocations, 8-byte aligned after end of symbol table
	uint32_t externalRelocsOffset = (symbolTableIndex * sizeof(macho_nlist<typename A::P>) + 7) & (-8);
	//uint32_t externalRelocsStartOffset = externalRelocsOffset;
	for(typename std::vector<LinkEditOptimizer<A>*>::iterator it = optimizers.begin(); it != optimizers.end(); ++it) {
		(*it)->copyExternalRelocations(externalRelocsOffset);
	}
	//fprintf(stderr, "%u bytes of external relocs\n", externalRelocsOffset-externalRelocsStartOffset);
	
	// copy indirect symbol tables
	uint32_t indirectSymbolTableOffset = externalRelocsOffset;
	for(typename std::vector<LinkEditOptimizer<A>*>::iterator it = optimizers.begin(); it != optimizers.end(); ++it) {
		(*it)->copyIndirectSymbolTable(indirectSymbolTableOffset);
	}
	
	// copy string pool
	uint32_t stringPoolOffset = indirectSymbolTableOffset;
	memcpy(&newLinkEdit[stringPoolOffset], stringPool.getBuffer(), stringPool.size());
	
	// find new size
	uint32_t linkEditsTotalOptimizedSize = (stringPoolOffset + stringPool.size() + 4095) & (-4096);
	
	// update load commands so that all dylibs shared different areas of the same LINKEDIT segment
	for(typename std::vector<LinkEditOptimizer<A>*>::iterator it = optimizers.begin(); it != optimizers.end(); ++it) {
		(*it)->updateLoadCommands(fLinkEditsStartAddress, fLinkEditsTotalUnoptimizedSize, stringPoolOffset);
	}

	//fprintf(stderr, "fLinkEditsTotalUnoptimizedSize=%llu, linkEditsTotalOptimizedSize=%u\n", fLinkEditsTotalUnoptimizedSize, linkEditsTotalOptimizedSize);
	//fprintf(stderr, "mega link edit mapped starting at: %p\n", fFirstLinkEditSegment->mappedAddress());

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
				seg.setSize(linkEditsTotalOptimizedSize);
				seg.setFileSize(linkEditsTotalOptimizedSize);
				//seg.setFileOffset(0);
			}
		}
	}
	
	// return new end of cache
	return (uint8_t*)fFirstLinkEditSegment->mappedAddress() + linkEditsTotalOptimizedSize;
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


template <typename A>
bool SharedCache<A>::update(const char* rootPath, const char* cacheDir, bool force, bool optimize, bool deleteExistingFirst, int archIndex, int archCount)
{
	bool didUpdate = false;
	char cachePath[1024];
	strcpy(cachePath, rootPath);
	strcat(cachePath, cacheDir);
	strcat(cachePath, DYLD_SHARED_CACHE_BASE_NAME);
	strcat(cachePath, filename(optimize));
	
	// already up to date?
	if ( force || this->notUpToDate(cachePath) ) {
		if ( verbose )
			fprintf(stderr, "update_dyld_shared_cache: regenerating %s\n", cachePath);
		if ( fDylibs.size() == 0 ) {
			fprintf(stderr, "update_dyld_shared_cache: warning, empty cache not generated for arch %s\n", archName());
			return false;
		}
		// delete existing cache while building the new one
		// this is a flag to dyld to stop pinging update_dyld_shared_cache
		if ( deleteExistingFirst )
			::unlink(cachePath);
		uint8_t* inMemoryCache = NULL;
		uint32_t allocatedCacheSize = 0;
		char tempCachePath[strlen(cachePath)+16];
		sprintf(tempCachePath, "%s.tmp%u", cachePath, getpid());
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
			
			// fill in header
			dyldCacheHeader<E>* header = (dyldCacheHeader<E>*)inMemoryCache;
			char temp[16];
			strcpy(temp, "dyld_v1        ");
			strcpy(&temp[15-strlen(archName())], archName());
			header->set_magic(temp);
			//header->set_architecture(arch());
			header->set_mappingOffset(sizeof(dyldCacheHeader<E>)); 
			header->set_mappingCount(fMappings.size());
			header->set_imagesOffset(header->mappingOffset() + fMappings.size()*sizeof(dyldCacheFileMapping<E>));	
			header->set_imagesCount(fDylibs.size());
			header->set_dyldBaseAddress(fDyldBaseAddress);
			//header->set_dependenciesOffset(sizeof(dyldCacheHeader<E>) + fMappings.size()*sizeof(dyldCacheFileMapping<E>) + fDylibs.size()*sizeof(dyldCacheImageInfo<E>));	
			//header->set_dependenciesCount(fDependencyPool.size());
			
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
				image->set_pathFileOffset(cacheFileOffsetForAddress(it->info.address+it->info.pathFileOffset));
				//image->set_dependenciesStartOffset(it->info.dependenciesStartOffset);
				++image;
			}
						
			// copy each segment to cache buffer
			int dylibIndex = 0;
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
					throwf("aborting because OS dylib modified during cache creation: %s", path);

				if ( verbose )
					fprintf(stderr, "update_dyld_shared_cache: copying %s to cache\n", it->layout->getID().name);
				try {
					const std::vector<MachOLayoutAbstraction::Segment>& segs = it->layout->getSegments();
					for (int i=0; i < segs.size(); ++i) {
						const MachOLayoutAbstraction::Segment& seg = segs[i];
						if ( verbose )
							fprintf(stderr, "\t\tsegment %s, size=0x%0llX, cache address=0x%0llX\n", seg.name(), seg.fileSize(), seg.newAddress());
						if ( seg.size() > 0 ) {
							const uint64_t segmentSrcStartOffset = it->layout->getOffsetInUniversalFile()+seg.fileOffset();
							const uint64_t segmentSize = seg.fileSize();
							const uint64_t segmentDstStartOffset = cacheFileOffsetForAddress(seg.newAddress());
							ssize_t readResult = ::pread(src, &inMemoryCache[segmentDstStartOffset], segmentSize, segmentSrcStartOffset);
							if ( readResult != segmentSize ) 
								if ( readResult == -1 )
									throwf("read failure copying dylib errno=%d for %s", errno, it->layout->getID().name);
								else
									throwf("read failure copying dylib. Read of %lld bytes at file offset %lld returned %ld for %s", 
											segmentSize, segmentSrcStartOffset, readResult, it->layout->getID().name);
							// verify __TEXT segment has no zeroed out pages
							if ( strcmp(seg.name(), "__TEXT") == 0 ) {
								// only scan first 128KB.  Some OS dylibs have zero filled TEXT pages later in __const...
								int scanEnd = segmentSize;
								if ( scanEnd > 0x20000 )
									scanEnd = 0x20000;
								for (int pageOffset = 0; pageOffset < scanEnd; pageOffset += 4096) {
									const uint32_t* page = (uint32_t*)(&inMemoryCache[segmentDstStartOffset+pageOffset]);
									bool foundNonZero = false;
									for(int p=0; p < 1024; ++p) {
										if ( page[p] != 0 ) {
											//fprintf(stderr, "found non-zero at pageOffset=0x%08X, p=0x%08X in memory=%p for %s\n", pageOffset, p, page, it->layout->getID().name);
											foundNonZero = true;
											break;
										}
									}
									if ( !foundNonZero )
										throwf("suspected bad read. Found __TEXT segment page at offset 0x%08X that is all zeros for %s in %s", pageOffset, archName(), it->layout->getID().name);
								}
							}
						}
					}
				}
				catch (const char* msg) {
					throwf("%s while copying %s to shared cache", msg, it->layout->getID().name);
				}
				::close(src);
			}
						
			// set mapped address for each segment
			for(typename std::vector<LayoutInfo>::const_iterator it = fDylibs.begin(); it != fDylibs.end(); ++it) {
				std::vector<MachOLayoutAbstraction::Segment>& segs = ((MachOLayoutAbstraction*)(it->layout))->getSegments();
				for (int i=0; i < segs.size(); ++i) {
					MachOLayoutAbstraction::Segment& seg = segs[i];
					if ( seg.size() > 0 )
						seg.setMappedAddress(inMemoryCache + cacheFileOffsetForAddress(seg.newAddress()));
					//fprintf(stderr, "%s at %p to %p for %s\n", seg.name(), seg.mappedAddress(), (char*)seg.mappedAddress()+ seg.size(), it->layout->getID().name);
				}
			}

			// rebase each dylib in shared cache
			for(typename std::vector<LayoutInfo>::const_iterator it = fDylibs.begin(); it != fDylibs.end(); ++it) {
				try {
					Rebaser<A> r(*it->layout);
					r.rebase();
					//if ( verbose )
					//	fprintf(stderr, "update_dyld_shared_cache: for %s, rebasing dylib into cache for %s\n", archName(), it->layout->getID().name);
				}
				catch (const char* msg) {
					throwf("%s in %s", msg, it->layout->getID().name);
				}
			}
			
			// merge/optimize all LINKEDIT segments
			if ( optimize ) {
				//fprintf(stderr, "update_dyld_shared_cache: original cache file size %uMB\n", cacheFileSize/(1024*1024));
				cacheFileSize = (this->optimizeLINKEDIT() - inMemoryCache);
				//fprintf(stderr, "update_dyld_shared_cache: optimized cache file size %uMB\n", cacheFileSize/(1024*1024));
				// update header to reduce mapping size
				dyldCacheHeader<E>* cacheHeader = (dyldCacheHeader<E>*)inMemoryCache;
				dyldCacheFileMapping<E>* mappings = (dyldCacheFileMapping<E>*)&inMemoryCache[sizeof(dyldCacheHeader<E>)];
				dyldCacheFileMapping<E>* lastMapping = &mappings[cacheHeader->mappingCount()-1];
				lastMapping->set_size(cacheFileSize-lastMapping->file_offset());
				// update fMappings so .map file will print correctly
				fMappings.back().sfm_size = cacheFileSize-fMappings.back().sfm_file_offset;
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
					(*it)->bind();
				}
				catch (const char* msg) {
					throwf("%s in %s", msg, (*it)->getDylibID());
				}
			}
			// delete binders
			for(typename std::vector<Binder<A>*>::iterator it = binders.begin(); it != binders.end(); ++it) {
				delete *it;
			}
	
			// install signal handlers to delete temp file if program is killed 
			sCleanupFile = tempCachePath;
			::signal(SIGINT, cleanup);
			::signal(SIGBUS, cleanup);
			::signal(SIGSEGV, cleanup);
			
			// create temp file for cache
			int fd = ::open(tempCachePath, O_CREAT | O_RDWR | O_TRUNC, 0644);	
			if ( fd == -1 )
				throwf("can't create temp file %s, errnor=%d", tempCachePath, errno);
				
			// try to allocate whole cache file contiguously
			fstore_t fcntlSpec = { F_ALLOCATECONTIG|F_ALLOCATEALL, F_PEOFPOSMODE, 0, cacheFileSize, 0 };
			::fcntl(fd, F_PREALLOCATE, &fcntlSpec);

			// write out cache file
			if ( verbose )
				fprintf(stderr, "update_dyld_shared_cache: writing cache to disk\n");
			if ( ::pwrite(fd, inMemoryCache, cacheFileSize, 0) != cacheFileSize )
				throwf("write() failure creating cache file, errno=%d", errno);
			
			// flush to disk and close
			int result = ::fcntl(fd, F_FULLFSYNC, NULL);
			if ( result == -1 ) 
				fprintf(stderr, "update_dyld_shared_cache: warning, fcntl(F_FULLFSYNC) failed with errno=%d for %s\n", errno, tempCachePath);
			result = ::close(fd);
			if ( result != 0 ) 
				fprintf(stderr, "update_dyld_shared_cache: warning, close() failed with errno=%d for %s\n", errno, tempCachePath);
			
			// atomically swap in new cache file, do this after F_FULLFSYNC
			result = ::rename(tempCachePath, cachePath);
			if ( result != 0 ) 
				throwf("can't swap newly create dyld shared cache file: rename(%s,%s) returned errno=%d", tempCachePath, cachePath, errno);
				
			// flush everything to disk to assure rename() gets recorded
			::sync();
			didUpdate = true;
			
			// restore default signal handlers
			::signal(SIGINT, SIG_DFL);
			::signal(SIGBUS, SIG_DFL);
			::signal(SIGSEGV, SIG_DFL);

			// generate human readable "map" file that shows the layout of the cache file
			if ( verbose )
				fprintf(stderr, "update_dyld_shared_cache: writing .map file to disk\n");
			sprintf(tempCachePath, "%s.map", cachePath);// re-use path buffer
			FILE* fmap = ::fopen(tempCachePath, "w");	
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
						fprintf(fmap, "mapping %s %4lluMB 0x%0llX -> 0x%0llX\n", prot, it->sfm_size/(1024*1024),
															it->sfm_address, it->sfm_address+it->sfm_size);
					else
						fprintf(fmap, "mapping %s %4lluKB 0x%0llX -> 0x%0llX\n", prot, it->sfm_size/1024,
															it->sfm_address, it->sfm_address+it->sfm_size);
				}
				for(typename std::vector<LayoutInfo>::const_iterator it = fDylibs.begin(); it != fDylibs.end(); ++it) {
					fprintf(fmap, "%s\n", it->layout->getID().name);
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
			}
			
			// free in memory cache
			vm_deallocate(mach_task_self(), (vm_address_t)inMemoryCache, allocatedCacheSize);
			inMemoryCache = NULL;
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
					paths.push_back(symbolStart);
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


static void scanForSharedDylibs(const char* rootPath, const char* dirOfPathFiles, const std::set<cpu_type_t>& onlyArchs)
{
	char rootDirOfPathFiles[strlen(rootPath)+strlen(dirOfPathFiles)+2];
	if ( strlen(rootPath) != 0 ) {
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
		if ( entry->d_type == DT_REG ) {
			// only look at files ending in .paths
			if ( strcmp(&entry->d_name[entry->d_namlen-6], ".paths") == 0 ) {
				char fullPath[strlen(dirOfPathFiles)+entry->d_namlen+2];
				strcpy(fullPath, dirOfPathFiles);
				strcat(fullPath, "/");
				strcat(fullPath, entry->d_name);
				parsePathsFile(fullPath, rootsPaths);
			}
			else {
				fprintf(stderr, "update_dyld_shared_cache: warning, ignore file with wrong extension: %s\n", entry->d_name);
			}
		}
	}
	::closedir(dir);

	// set file system root
	ArchGraph::setFileSystemRoot(rootPath);

	// initialize all architectures requested
	for(std::set<cpu_type_t>::iterator a = onlyArchs.begin(); a != onlyArchs.end(); ++a)
		ArchGraph::addArch(*a);

	// add roots to graph
	for(std::vector<const char*>::iterator it = rootsPaths.begin(); it != rootsPaths.end(); ++it) 
		ArchGraph::addRoot(*it, onlyArchs);

	// determine shared dylibs
	for(std::set<cpu_type_t>::iterator a = onlyArchs.begin(); a != onlyArchs.end(); ++a)
		ArchGraph::findSharedDylibs(*a);
	
	if ( rootsPaths.size() == 0 )
		fprintf(stderr, "update_dyld_shared_cache: warning, no entries found in shared_region_roots\n");
}


// If the 10.5.0 version of update_dyld_shared_cache was killed or crashed, it 
// could leave large half written cache files laying around.  The function deletes
// those files.  To prevent the deletion of tmp files being created by another
// copy of update_dyld_shared_cache, it only deletes the temp cache file if its 
// creation time was before the last restart of this machine.
static void deleteOrphanTempCacheFiles()
{
	DIR* dir = ::opendir(DYLD_SHARED_CACHE_DIR);
	if ( dir != NULL ) {
		std::vector<const char*> filesToDelete;
		for (dirent* entry = ::readdir(dir); entry != NULL; entry = ::readdir(dir)) {
			if ( entry->d_type == DT_REG ) {
				// only look at files with .tmp in name
				if ( strstr(entry->d_name, ".tmp") != NULL ) {
					char fullPath[strlen(DYLD_SHARED_CACHE_DIR)+entry->d_namlen+2];
					strcpy(fullPath, DYLD_SHARED_CACHE_DIR);
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



static bool updateSharedeCacheFile(const char* rootPath, const char* cacheDir, const std::set<cpu_type_t>& onlyArchs, 
									bool force, bool alphaSort, bool optimize, bool deleteExistingFirst)
{
	bool didUpdate = false;
	// get dyld load address info
	UniversalMachOLayout* dyldLayout = new UniversalMachOLayout("/usr/lib/dyld", &onlyArchs);

	const int archCount = onlyArchs.size();
	int index = 0;
	for(std::set<cpu_type_t>::iterator a = onlyArchs.begin(); a != onlyArchs.end(); ++a, ++index) {
		const MachOLayoutAbstraction* dyldLayoutForArch = dyldLayout->getArch(*a);
		if ( dyldLayoutForArch == NULL )
			throw "dyld not avaiable for specified architecture";
		uint64_t dyldBaseAddress = dyldLayoutForArch->getBaseAddress();
		switch ( *a ) {
			case CPU_TYPE_POWERPC:
				{
					SharedCache<ppc> cache(ArchGraph::getArch(*a), alphaSort, dyldBaseAddress);
		#if __i386__
					// <rdar://problem/5217377> Rosetta does not work with optimized dyld shared cache
					didUpdate |= cache.update(rootPath, cacheDir, force, false, deleteExistingFirst, index, archCount);
		#else
					didUpdate |= cache.update(rootPath, cacheDir, force, optimize, deleteExistingFirst, index, archCount);
		#endif
				}
				break;
			case CPU_TYPE_POWERPC64:
				{
					SharedCache<ppc64> cache(ArchGraph::getArch(*a), alphaSort, dyldBaseAddress);
					didUpdate |= cache.update(rootPath, cacheDir, force, optimize, deleteExistingFirst, index, archCount);
				}
				break;
			case CPU_TYPE_I386:
				{
					SharedCache<x86> cache(ArchGraph::getArch(*a), alphaSort, dyldBaseAddress);
					didUpdate |= cache.update(rootPath, cacheDir, force, optimize, deleteExistingFirst, index, archCount);
				}
				break;
			case CPU_TYPE_X86_64:
				{
					SharedCache<x86_64> cache(ArchGraph::getArch(*a), alphaSort, dyldBaseAddress);
					didUpdate |= cache.update(rootPath, cacheDir, force, optimize, deleteExistingFirst, index, archCount);
				}
				break;
		}
	}
	
	deleteOrphanTempCacheFiles();
	
	return didUpdate;
}


static void usage()
{
	fprintf(stderr, "update_dyld_shared_cache [-force] [-root dir] [-arch arch] [-debug]\n");
}

// flag so that we only update cache once per invocation
static bool doNothingAndDrainQueue = false;

static kern_return_t do_update_cache(cpu_type_t arch, bool deleteExistingCacheFileFirst)
{
	if ( !doNothingAndDrainQueue ) {
		std::set<cpu_type_t> onlyArchs;
		onlyArchs.insert(arch);
		try {
			scanForSharedDylibs("", "/var/db/dyld/shared_region_roots/", onlyArchs);
			if ( updateSharedeCacheFile("", DYLD_SHARED_CACHE_DIR, onlyArchs, false, false, true, deleteExistingCacheFileFirst) )
				fprintf(stderr, "update_dyld_shared_cache[%u] regenerated cache for arch=%s\n", getpid(), ArchGraph::archName(arch));
		}
		catch (const char* msg) {
			fprintf(stderr, "update_dyld_shared_cache[%u] for arch=%s failed: %s\n", getpid(), ArchGraph::archName(arch), msg);
			return KERN_FAILURE;
		}
		// <rdar://problem/6378354> only build one cache file per life of process
		doNothingAndDrainQueue = true;
	}
	return KERN_SUCCESS;
}



kern_return_t do_dyld_shared_cache_missing(mach_port_t dyld_port, cpu_type_t arch)
{
	return do_update_cache(arch, false);
}


kern_return_t do_dyld_shared_cache_out_of_date(mach_port_t dyld_port, cpu_type_t arch)
{
	// If cache exists but is out of date, delete the file while building the new one.
	// This will stop dyld from pinging update_dyld_share_cache while the cache is being built.
	return do_update_cache(arch, true);
}


int main(int argc, const char* argv[])
{
	mach_port_t mp;
	if ( bootstrap_check_in(bootstrap_port, "com.apple.dyld", &mp) == KERN_SUCCESS ) {
		// started by launchd
		mach_msg_size_t mxmsgsz = sizeof(union __RequestUnion__do_dyld_server_subsystem) + MAX_TRAILER_SIZE;
		doNothingAndDrainQueue = false;
		while ( mach_msg_server(dyld_server_server, mxmsgsz, mp, MACH_RCV_TIMEOUT) == KERN_SUCCESS ) {
			// keep processing messages
			doNothingAndDrainQueue = true;
			// but set flag so work is no longer done.
			// This is because the rest of the tool leaks and processing more than once
			// can hog system resources: <rdar://problem/5392427> 9A516 - Keep getting disk full errors
			// We drain the queue of messages because there is usually are a couple of duplicate messages.
			// It is ok to miss some messages.  If the cache is out of date or missing, some new process
			// will discover it and send another message.  
		}
		return 0;
	}
	else {
		// started as command line tool
		std::set<cpu_type_t> onlyArchs;
		const char* rootPath = "";
		bool force = false;
		bool alphaSort = false;
		bool optimize = true;
		bool makeSymLink = false;
	
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
					else if ( strcmp(arg, "-sort_by_name") == 0 ) {
						alphaSort = true;
					}
					else if ( strcmp(arg, "-opt") == 0 ) {
						optimize = true;
					}
					else if ( strcmp(arg, "-no_opt") == 0 ) {
						optimize = false;
					}
					else if ( (strcmp(arg, "-root") == 0) || (strcmp(arg, "--root") == 0) ) {
						rootPath = argv[++i];
						if ( rootPath == NULL )
							throw "-root missing path argument";
						// strip tailing slashes
						int len = strlen(rootPath)-1;
						if (  rootPath[len] == '/' ) {
							char* newRootPath = strdup(rootPath);
							while ( newRootPath[len] == '/' )	
								newRootPath[len--] = '\0';
							rootPath = newRootPath;
						}
					}
					else if ( strcmp(arg, "-arch") == 0 ) {
						const char* arch = argv[++i];
						if ( strcmp(arch, "ppc") == 0 ) 
							onlyArchs.insert(CPU_TYPE_POWERPC);
						else if ( strcmp(arch, "ppc64") == 0 )
							onlyArchs.insert(CPU_TYPE_POWERPC64);
						else if ( strcmp(arch, "i386") == 0 )
							onlyArchs.insert(CPU_TYPE_I386);
						else if ( strcmp(arch, "x86_64") == 0 )
							onlyArchs.insert(CPU_TYPE_X86_64);
						else 
							throwf("unknown architecture %s", arch);
					}
					else if ( strcmp(arg, "-universal_boot") == 0 ) {
				#if __ppc__
						throwf("universal_boot option can only be used on Intel machines");
				#endif
						onlyArchs.insert(CPU_TYPE_POWERPC);
						onlyArchs.insert(CPU_TYPE_I386);
						makeSymLink = true;
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
					
			// if no restrictions specified, use architectures that work on this machine
			if ( onlyArchs.size() == 0 ) {
				int available;
				size_t len = sizeof(int);
			#if __ppc__	
				onlyArchs.insert(CPU_TYPE_POWERPC);
				if ( (sysctlbyname("hw.optional.64bitops", &available, &len, NULL, 0) == 0) && available )
					onlyArchs.insert(CPU_TYPE_POWERPC64);
			#elif __i386__
				onlyArchs.insert(CPU_TYPE_I386);
				onlyArchs.insert(CPU_TYPE_POWERPC);	// assume rosetta always available
				if ( (sysctlbyname("hw.optional.x86_64", &available, &len, NULL, 0) == 0) && available )
					onlyArchs.insert(CPU_TYPE_X86_64);
			#else
				#error unknown architecture
			#endif
			}
			
			if ( geteuid() != 0 )
				throw "you must be root to run this tool";
			
			// build list of shared dylibs
			scanForSharedDylibs(rootPath, "/var/db/dyld/shared_region_roots/", onlyArchs);
			updateSharedeCacheFile(rootPath, DYLD_SHARED_CACHE_DIR, onlyArchs, force, alphaSort, optimize, false);
			
			// To make a universal bootable image with dyld caches,
			// build the rosetta cache and symlink ppc to point to it.
			// A rosetta cache is just an unoptimized ppc cache, so ppc machine can use it too.
			// rdar://problem/5498469
			if ( makeSymLink ) {
				char symLinkLocation[1024];
				strcpy(symLinkLocation, rootPath);
				strcat(symLinkLocation, DYLD_SHARED_CACHE_DIR);
				strcat(symLinkLocation, DYLD_SHARED_CACHE_BASE_NAME);
				strcat(symLinkLocation, SharedCache<ppc>::filename(true));
				char symLinkTarget[1024];
				strcpy(symLinkTarget, DYLD_SHARED_CACHE_BASE_NAME);
				strcat(symLinkTarget, SharedCache<ppc>::filename(false));
				if ( symlink(symLinkTarget, symLinkLocation) == -1 ) {
					if ( errno != EEXIST )
						throwf("symlink() returned errno=%d", errno);
				}
			}
		}
		catch (const char* msg) {
			fprintf(stderr, "update_dyld_shared_cache failed: %s\n", msg);
			return 1;
		}
		
		return 0;
	}
}



