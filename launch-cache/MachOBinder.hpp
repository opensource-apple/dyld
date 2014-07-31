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

#ifndef __MACHO_BINDER__
#define __MACHO_BINDER__

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <mach-o/loader.h>
#include <mach-o/fat.h>

#include <vector>
#include <set>
#include <ext/hash_map>
#include <ext/hash_set>

#include "MachOFileAbstraction.hpp"
#include "Architectures.hpp"
#include "MachOLayout.hpp"
#include "MachORebaser.hpp"
#include "MachOTrie.hpp"

#ifndef EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER
	#define EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER 0x10
#endif



template <typename A>
class Binder : public Rebaser<A>
{
public:
	struct CStringEquals {
		bool operator()(const char* left, const char* right) const { return (strcmp(left, right) == 0); }
	};
	typedef __gnu_cxx::hash_map<const char*, class Binder<A>*, __gnu_cxx::hash<const char*>, CStringEquals> Map;


												Binder(const MachOLayoutAbstraction&, uint64_t dyldBaseAddress);
	virtual										~Binder() {}
	
	const char*									getDylibID() const;
	void										setDependentBinders(const Map& map);
	void										bind(std::vector<void*>&);
	void										optimize();
    void                                        addResolverClient(Binder<A>* clientDylib, const char* symbolName);
    void                                        addResolverLazyPointerMappedAddress(const char* symbolName, 
                                                                                    typename A::P::uint_t  lpVMAddr);
private:
	typedef typename A::P					P;
	typedef typename A::P::E				E;
	typedef typename A::P::uint_t			pint_t;
	struct BinderAndReExportFlag { Binder<A>* binder; bool reExport; };
	struct SymbolReExport { const char* exportName; int dylibOrdinal; const char* importName; };
	typedef __gnu_cxx::hash_map<const char*, pint_t, __gnu_cxx::hash<const char*>, CStringEquals> NameToAddrMap;
	typedef __gnu_cxx::hash_set<const char*, __gnu_cxx::hash<const char*>, CStringEquals> NameSet;
    struct ClientAndSymbol { Binder<A>* client; const char* symbolName; };
    struct SymbolAndLazyPointer { const char* symbolName; pint_t lpVMAddr; };
	
	static bool									isPublicLocation(const char* pth);
	void										doBindExternalRelocations();
	void										doBindIndirectSymbols();
	void										doSetUpDyldSection();
	void										doSetPreboundUndefines();
	void										hoistPrivateRexports();
	int											ordinalOfDependentBinder(Binder<A>* dep);
	void										doBindDyldInfo(std::vector<void*>& pointersInData);
	void										doBindDyldLazyInfo(std::vector<void*>& pointersInData);
	void										bindDyldInfoAt(uint8_t segmentIndex, uint64_t segmentOffset, uint8_t type, 
																int libraryOrdinal, int64_t addend, 
																const char* symbolName, bool lazyPointer,
																std::vector<void*>& pointersInData);
	pint_t										resolveUndefined(const macho_nlist<P>* undefinedSymbol);
	bool										findExportedSymbolAddress(const char* name, pint_t* result, Binder<A>** foundIn, bool* isResolverSymbol);
	void										bindStub(uint8_t elementSize, uint8_t* location, pint_t vmlocation, pint_t value);
	const char*									parentUmbrella();
	pint_t										runtimeAddressFromNList(const macho_nlist<P>* sym);
    void                                        optimizeStub(const char* symbolName, pint_t lpVMAddr);
    void                                        optimizeStub(uint8_t* stubMappedAddress, pint_t stubVMAddress, uint32_t stubSize, pint_t lpVMAddr);
    pint_t                                      findLazyPointerFor(const char* symbolName);


	static uint8_t								pointerRelocSize();
	static uint8_t								pointerRelocType();
	
	std::vector<BinderAndReExportFlag>			fDependentDylibs;
	NameToAddrMap								fHashTable;
	NameSet										fSymbolResolvers;
	std::vector<SymbolReExport>					fReExportedSymbols;
	uint64_t									fDyldBaseAddress;
	const macho_nlist<P>*						fSymbolTable;
	const char*									fStrings;
	const macho_dysymtab_command<P>*			fDynamicInfo;
	const macho_segment_command<P>*				fFristWritableSegment;
	const macho_dylib_command<P>*				fDylibID;
	const macho_dylib_command<P>*				fParentUmbrella;
	const macho_dyld_info_command<P>*			fDyldInfo;
	bool										fOriginallyPrebound;
	bool										fReExportedSymbolsResolved;
    std::vector<ClientAndSymbol>                fClientAndSymbols;
    std::vector<SymbolAndLazyPointer>           fSymbolAndLazyPointers;
};

template <> 
uint32_t Binder<arm>::runtimeAddressFromNList(const macho_nlist<Pointer32<LittleEndian> >* sym) 
{ 
	if (sym->n_desc() & N_ARM_THUMB_DEF)
		return sym->n_value() + 1; 
	else
		return sym->n_value(); 
}

template <typename A> 
typename A::P::uint_t	Binder<A>::runtimeAddressFromNList(const macho_nlist<P>* sym) 
{ 
	return sym->n_value(); 
}


template <typename A>
Binder<A>::Binder(const MachOLayoutAbstraction& layout, uint64_t dyldBaseAddress)
	: Rebaser<A>(layout), fDyldBaseAddress(dyldBaseAddress),
	  fSymbolTable(NULL), fStrings(NULL), fDynamicInfo(NULL),
	  fFristWritableSegment(NULL), fDylibID(NULL), fDyldInfo(NULL),
	  fParentUmbrella(NULL), fReExportedSymbolsResolved(false)
{
	fOriginallyPrebound = ((this->fHeader->flags() & MH_PREBOUND) != 0);
	// update header flags so the cache looks prebound split-seg (0x80000000 is in-shared-cache bit)
	((macho_header<P>*)this->fHeader)->set_flags(this->fHeader->flags() | MH_PREBOUND | MH_SPLIT_SEGS | 0x80000000);

	// calculate fDynamicInfo, fStrings, fSymbolTable
	const macho_symtab_command<P>* symtab;
	const macho_load_command<P>* const cmds = (macho_load_command<P>*)((uint8_t*)this->fHeader + sizeof(macho_header<P>));
	const uint32_t cmd_count = this->fHeader->ncmds();
	const macho_load_command<P>* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd()) {
			case LC_SYMTAB:
				symtab = (macho_symtab_command<P>*)cmd;
				fSymbolTable = (macho_nlist<P>*)(&this->fLinkEditBase[symtab->symoff()]);
				fStrings = (const char*)&this->fLinkEditBase[symtab->stroff()];
				break;
			case LC_DYSYMTAB:
				fDynamicInfo = (macho_dysymtab_command<P>*)cmd;
				break;
			case LC_ID_DYLIB:
				((macho_dylib_command<P>*)cmd)->set_timestamp(0);
				fDylibID = (macho_dylib_command<P>*)cmd;
				break;
			case LC_LOAD_DYLIB:
			case LC_LOAD_WEAK_DYLIB:
			case LC_REEXPORT_DYLIB:
			case LC_LOAD_UPWARD_DYLIB:
				((macho_dylib_command<P>*)cmd)->set_timestamp(0);
				break;
			case LC_SUB_FRAMEWORK:
				fParentUmbrella = (macho_dylib_command<P>*)cmd;
				break;
			case LC_DYLD_INFO:
			case LC_DYLD_INFO_ONLY:
				fDyldInfo = (macho_dyld_info_command<P>*)cmd;
				break;
			case LC_RPATH:
				throwf("LC_RPATH not supported in dylibs in dyld shared cache");
				break;
			default:
				if ( cmd->cmd() & LC_REQ_DYLD )
					throwf("unknown required load command 0x%08X", cmd->cmd());
		}
		cmd = (const macho_load_command<P>*)(((uint8_t*)cmd)+cmd->cmdsize());
	}	
	if ( fDynamicInfo == NULL )	
		throw "no LC_DYSYMTAB";
	if ( fSymbolTable == NULL )	
		throw "no LC_SYMTAB";
	// build hash table
//	fprintf(stderr, "exports for %s\n", layout.getFilePath());
	if ( fDyldInfo != NULL ) {
		std::vector<mach_o::trie::Entry> exports;
		const uint8_t* exportsStart = layout.getDyldInfoExports(); 
		const uint8_t* exportsEnd = &exportsStart[fDyldInfo->export_size()];
		mach_o::trie::parseTrie(exportsStart, exportsEnd, exports);
		pint_t baseAddress = layout.getSegments()[0].newAddress();
		for(std::vector<mach_o::trie::Entry>::iterator it = exports.begin(); it != exports.end(); ++it) {
			if ( (it->flags & EXPORT_SYMBOL_FLAGS_KIND_MASK) == EXPORT_SYMBOL_FLAGS_KIND_REGULAR ) {
				if ( (it->flags & EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER) ) {
					fSymbolResolvers.insert(it->name);
				}
				if ( it->flags & EXPORT_SYMBOL_FLAGS_REEXPORT ) {
					//fprintf(stderr, "found re-export %s in %s\n", sym.exportName, this->getDylibID());
					SymbolReExport sym;
					sym.exportName = it->name;
					sym.dylibOrdinal = it->other;
					sym.importName = it->importName;
					if ( (sym.importName == NULL) || (sym.importName[0] == '\0') ) 
						sym.importName = sym.exportName;
					fReExportedSymbols.push_back(sym);
					// fHashTable entry will be added in first call to findExportedSymbolAddress() 
				}
				else {
					fHashTable[it->name] = it->address + baseAddress;
				}
			}
			else {
				throwf("non-regular symbol binding not supported for %s in %s", it->name, layout.getFilePath());
			}
			//fprintf(stderr, "0x%08llX %s\n", it->address + baseAddress, it->name);
		}
	}
	else {
		if ( fDynamicInfo->tocoff() == 0 ) {
			const macho_nlist<P>* start = &fSymbolTable[fDynamicInfo->iextdefsym()];
			const macho_nlist<P>* end = &start[fDynamicInfo->nextdefsym()];
			fHashTable.resize(fDynamicInfo->nextdefsym()); // set initial bucket count
			for (const macho_nlist<P>* sym=start; sym < end; ++sym) {
				const char* name = &fStrings[sym->n_strx()];
				fHashTable[name] = runtimeAddressFromNList(sym);
				//fprintf(stderr, " 0x%08llX %s\n", sym->n_value(), name);
			}
		}
		else {
			int32_t count = fDynamicInfo->ntoc();
			fHashTable.resize(count); // set initial bucket count
			const struct dylib_table_of_contents* toc = (dylib_table_of_contents*)&this->fLinkEditBase[fDynamicInfo->tocoff()];
			for (int32_t i = 0; i < count; ++i) {
				const uint32_t index = E::get32(toc[i].symbol_index);
				const macho_nlist<P>* sym = &fSymbolTable[index];
				const char* name = &fStrings[sym->n_strx()];
				fHashTable[name] = runtimeAddressFromNList(sym);
				//fprintf(stderr, "- 0x%08llX %s\n", sym->n_value(), name);
			}
		}
	}
}

template <> uint8_t	Binder<ppc>::pointerRelocSize()    { return 2; }
template <> uint8_t	Binder<x86>::pointerRelocSize()   { return 2; }
template <> uint8_t	Binder<x86_64>::pointerRelocSize() { return 3; }
template <> uint8_t	Binder<arm>::pointerRelocSize() { return 2; }

template <> uint8_t	Binder<ppc>::pointerRelocType()    { return GENERIC_RELOC_VANILLA; }
template <> uint8_t	Binder<x86>::pointerRelocType()   { return GENERIC_RELOC_VANILLA; }
template <> uint8_t	Binder<x86_64>::pointerRelocType() { return X86_64_RELOC_UNSIGNED; }
template <> uint8_t	Binder<arm>::pointerRelocType() { return ARM_RELOC_VANILLA; }


template <typename A>
const char* Binder<A>::getDylibID() const
{
	if ( fDylibID != NULL )
		return fDylibID->name();
	else
		return NULL;
}

template <typename A>
const char* Binder<A>::parentUmbrella()
{
	if ( fParentUmbrella != NULL )
		return fParentUmbrella->name();
	else
		return NULL;
}


template <typename A>
bool Binder<A>::isPublicLocation(const char* pth)
{
	// /usr/lib is a public location
	if ( (strncmp(pth, "/usr/lib/", 9) == 0) && (strchr(&pth[9], '/') == NULL) )
		return true;

	// /System/Library/Frameworks/ is a public location
	if ( strncmp(pth, "/System/Library/Frameworks/", 27) == 0 ) {
		const char* frameworkDot = strchr(&pth[27], '.');
		// but only top level framework
		// /System/Library/Frameworks/Foo.framework/Versions/A/Foo                 ==> true
		// /System/Library/Frameworks/Foo.framework/Resources/libBar.dylib         ==> false
		// /System/Library/Frameworks/Foo.framework/Frameworks/Bar.framework/Bar   ==> false
		// /System/Library/Frameworks/Foo.framework/Frameworks/Xfoo.framework/XFoo ==> false
		if ( frameworkDot != NULL ) {
			int frameworkNameLen = frameworkDot - &pth[27];
			if ( strncmp(&pth[strlen(pth)-frameworkNameLen-1], &pth[26], frameworkNameLen+1) == 0 )
				return true;
		}
	}
		
	return false;
}

template <typename A>
void Binder<A>::setDependentBinders(const Map& map)
{
	// first pass to build vector of dylibs
	const macho_load_command<P>* const cmds = (macho_load_command<P>*)((uint8_t*)this->fHeader + sizeof(macho_header<P>));
	const uint32_t cmd_count = this->fHeader->ncmds();
	const macho_load_command<P>* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd()) {
			case LC_LOAD_DYLIB:
			case LC_LOAD_WEAK_DYLIB:
			case LC_REEXPORT_DYLIB:
			case LC_LOAD_UPWARD_DYLIB:
				const char* path = ((struct macho_dylib_command<P>*)cmd)->name();
				typename Map::const_iterator pos = map.find(path);
				if ( pos != map.end() ) {
					BinderAndReExportFlag entry;
					entry.binder = pos->second;
					entry.reExport = ( cmd->cmd() == LC_REEXPORT_DYLIB );
					fDependentDylibs.push_back(entry);
				}
				else {
					// the load command string does not match the install name of any loaded dylib
					// this could happen if there was not a world build and some dylib changed its
					// install path to be some symlinked path
					
					// use realpath() and walk map looking for a realpath match
					bool found = false;
					char targetPath[PATH_MAX];
					if ( realpath(path, targetPath) != NULL ) {
						for(typename Map::const_iterator it=map.begin(); it != map.end(); ++it) {
							char aPath[PATH_MAX];
							if ( realpath(it->first, aPath) != NULL ) {
								if ( strcmp(targetPath, aPath) == 0 ) {
									BinderAndReExportFlag entry;
									entry.binder = it->second;
									entry.reExport = ( cmd->cmd() == LC_REEXPORT_DYLIB );
									fDependentDylibs.push_back(entry);
									found = true;
									fprintf(stderr, "update_dyld_shared_cache: warning mismatched install path in %s for %s\n", 
										this->getDylibID(), path);
									break;
								}
							}
						}
					}
					if ( ! found )
						throwf("in %s can't find dylib %s", this->getDylibID(), path);
				}
				break;
		}
		cmd = (const macho_load_command<P>*)(((uint8_t*)cmd)+cmd->cmdsize());
	}
	// handle pre-10.5 re-exports 
	if ( (this->fHeader->flags() & MH_NO_REEXPORTED_DYLIBS) == 0 ) {
		cmd = cmds;
		// LC_SUB_LIBRARY means re-export one with matching leaf name
		const char* dylibBaseName;
		const char* frameworkLeafName;
		for (uint32_t i = 0; i < cmd_count; ++i) {
			switch ( cmd->cmd() ) {
				case LC_SUB_LIBRARY:
					dylibBaseName = ((macho_sub_library_command<P>*)cmd)->sub_library();
					for (typename std::vector<BinderAndReExportFlag>::iterator it = fDependentDylibs.begin(); it != fDependentDylibs.end(); ++it) {
						const char* dylibName = it->binder->getDylibID();
						const char* lastSlash = strrchr(dylibName, '/');
						const char* leafStart = &lastSlash[1];
						if ( lastSlash == NULL )
							leafStart = dylibName;
						const char* firstDot = strchr(leafStart, '.');
						int len = strlen(leafStart);
						if ( firstDot != NULL )
							len = firstDot - leafStart;
						if ( strncmp(leafStart, dylibBaseName, len) == 0 )
							it->reExport = true;
					}
					break;
				case LC_SUB_UMBRELLA:
					frameworkLeafName = ((macho_sub_umbrella_command<P>*)cmd)->sub_umbrella();
					for (typename std::vector<BinderAndReExportFlag>::iterator it = fDependentDylibs.begin(); it != fDependentDylibs.end(); ++it) {
						const char* dylibName = it->binder->getDylibID();
						const char* lastSlash = strrchr(dylibName, '/');
						if ( (lastSlash != NULL) && (strcmp(&lastSlash[1], frameworkLeafName) == 0) )
							it->reExport = true;
					}
					break;
			}
			cmd = (const macho_load_command<P>*)(((uint8_t*)cmd)+cmd->cmdsize());
		}
		// ask dependents if they re-export through me
		const char* thisName = this->getDylibID();
		if ( thisName != NULL ) {
			const char* thisLeafName = strrchr(thisName, '/');
			if ( thisLeafName != NULL )
				++thisLeafName;
			for (typename std::vector<BinderAndReExportFlag>::iterator it = fDependentDylibs.begin(); it != fDependentDylibs.end(); ++it) {
				if ( ! it->reExport ) {
					const char* parentUmbrellaName = it->binder->parentUmbrella();
					if ( parentUmbrellaName != NULL ) {
						if ( strcmp(parentUmbrellaName, thisLeafName) == 0 )
							it->reExport = true;
					}
				}
			}	
		}
	}
	
}

template <typename A>
int Binder<A>::ordinalOfDependentBinder(Binder<A>* dep)
{
	for (int i=0; i < fDependentDylibs.size(); ++i) {
		if ( fDependentDylibs[i].binder == dep ) 
			return i+1;
	}
	throw "dependend dylib not found";
}

template <typename A>
void Binder<A>::hoistPrivateRexports()
{
	std::vector<Binder<A>*> privateReExportedDylibs;
	for (typename std::vector<BinderAndReExportFlag>::iterator it = fDependentDylibs.begin(); it != fDependentDylibs.end(); ++it) {
		if ( it->reExport && ! isPublicLocation(it->binder->getDylibID()) )
			privateReExportedDylibs.push_back(it->binder);
	}
	if ( privateReExportedDylibs.size() != 0 ) {
		// parse export info into vector of exports
		const uint8_t* exportsStart = this->fLayout.getDyldInfoExports(); 
		const uint8_t* exportsEnd = &exportsStart[fDyldInfo->export_size()];
		std::vector<mach_o::trie::Entry> exports;
		mach_o::trie::parseTrie(exportsStart, exportsEnd, exports);
		//fprintf(stderr, "%s exports %lu symbols from trie of size %u \n", this->fLayout.getFilePath(), exports.size(), fDyldInfo->export_size());

		// add re-exports for each export from an re-exported dylib
		for(typename std::vector<Binder<A>*>::iterator it = privateReExportedDylibs.begin(); it != privateReExportedDylibs.end(); ++it) {
			Binder<A>* binder = *it;
			int ordinal = ordinalOfDependentBinder(binder);
			const uint8_t* aDylibsExportsStart = binder->fLayout.getDyldInfoExports(); 
			const uint8_t* aDylibsExportsEnd = &aDylibsExportsStart[binder->fDyldInfo->export_size()];
			std::vector<mach_o::trie::Entry> aDylibsExports;
			mach_o::trie::parseTrie(aDylibsExportsStart, aDylibsExportsEnd, aDylibsExports);
			//fprintf(stderr, "%s re-exports %lu symbols from %s\n", this->fLayout.getFilePath(), aDylibsExports.size(), binder->getDylibID());
			for(std::vector<mach_o::trie::Entry>::iterator eit = aDylibsExports.begin(); eit != aDylibsExports.end(); ++eit) {
				mach_o::trie::Entry entry = *eit;
				entry.flags |= EXPORT_SYMBOL_FLAGS_REEXPORT;
				entry.other = ordinal;
				entry.importName = NULL;
				exports.push_back(entry);
			}
		}
		// rebuild new combined trie
		std::vector<uint8_t> newExportTrieBytes;
		newExportTrieBytes.reserve(fDyldInfo->export_size());
		mach_o::trie::makeTrie(exports, newExportTrieBytes);
		//fprintf(stderr, "%s now exports %lu symbols from trie of size %lu\n", this->fLayout.getFilePath(), exports.size(), newExportTrieBytes.size());

		// allocate new buffer and set export_off to use new buffer instead
		uint32_t newExportsSize = newExportTrieBytes.size();
		uint8_t* sideTrie = new uint8_t[newExportsSize];
		memcpy(sideTrie, &newExportTrieBytes[0], newExportsSize);
		this->fLayout.setDyldInfoExports(sideTrie);
		((macho_dyld_info_command<P>*)fDyldInfo)->set_export_off(0); // invalidate old trie
		((macho_dyld_info_command<P>*)fDyldInfo)->set_export_size(newExportsSize);
	}
}


template <typename A>
void Binder<A>::bind(std::vector<void*>& pointersInData)
{
	this->doSetUpDyldSection();
	if ( fDyldInfo != NULL ) {
		this->doBindDyldInfo(pointersInData);
		this->doBindDyldLazyInfo(pointersInData);
		this->hoistPrivateRexports();
		// weak bind info is processed at launch time
	}
	else {
		this->doBindExternalRelocations();
		this->doBindIndirectSymbols();
		this->doSetPreboundUndefines();
	}
}


template <typename A>
void Binder<A>::doSetUpDyldSection()
{
	// find __DATA __dyld section 
	const macho_load_command<P>* const cmds = (macho_load_command<P>*)((uint8_t*)this->fHeader + sizeof(macho_header<P>));
	const uint32_t cmd_count = this->fHeader->ncmds();
	const macho_load_command<P>* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		if ( cmd->cmd() == macho_segment_command<P>::CMD ) {
			const macho_segment_command<P>* seg = (macho_segment_command<P>*)cmd;
			if ( strcmp(seg->segname(), "__DATA") == 0 ) {
				const macho_section<P>* const sectionsStart = (macho_section<P>*)((uint8_t*)seg + sizeof(macho_segment_command<P>));
				const macho_section<P>* const sectionsEnd = &sectionsStart[seg->nsects()];
				for (const macho_section<P>* sect=sectionsStart; sect < sectionsEnd; ++sect) {
					if ( (strcmp(sect->sectname(), "__dyld") == 0) && (sect->size() >= 2*sizeof(pint_t)) ) {
						// set two values in __dyld section to point into dyld
						pint_t* lazyBinder = this->mappedAddressForNewAddress(sect->addr());
						pint_t* dyldFuncLookup = this->mappedAddressForNewAddress(sect->addr()+sizeof(pint_t));
						A::P::setP(*lazyBinder, fDyldBaseAddress + 0x1000);
						A::P::setP(*dyldFuncLookup, fDyldBaseAddress + 0x1008);
					}
				}
			}
		}
		cmd = (const macho_load_command<P>*)(((uint8_t*)cmd)+cmd->cmdsize());
	}
}

template <typename A>
void Binder<A>::bindDyldInfoAt(uint8_t segmentIndex, uint64_t segmentOffset, uint8_t type, int libraryOrdinal, 
							int64_t addend, const char* symbolName, bool lazyPointer, std::vector<void*>& pointersInData)
{
	//printf("%d 0x%08llX type=%d, lib=%d, addend=%lld, symbol=%s\n", segmentIndex, segmentOffset, type, libraryOrdinal, addend, symbolName);
	const std::vector<MachOLayoutAbstraction::Segment>& segments = this->fLayout.getSegments();
	if ( segmentIndex > segments.size() )
		throw "bad segment index in rebase info";
		
	if ( libraryOrdinal == BIND_SPECIAL_DYLIB_FLAT_LOOKUP ) 
		throw "dynamic lookup linkage not allowed in dyld shared cache";
	
	if ( libraryOrdinal == BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE ) 
		throw "linkage to main executable not allowed in dyld shared cache";
	
	if ( libraryOrdinal < 0 ) 
		throw "bad mach-o binary, special library ordinal not allowd in dyld shared cache";
	
	if ( (unsigned)libraryOrdinal > fDependentDylibs.size() ) 
		throw "bad mach-o binary, library ordinal too big";
	
	Binder<A>* binder;
	if ( libraryOrdinal == BIND_SPECIAL_DYLIB_SELF ) 
		binder = this;
	else
		binder = fDependentDylibs[libraryOrdinal-1].binder;
	pint_t targetSymbolAddress;
	bool isResolverSymbol;
    Binder<A>* foundIn;
	if ( ! binder->findExportedSymbolAddress(symbolName, &targetSymbolAddress, &foundIn, &isResolverSymbol) ) 
		throwf("could not bind symbol %s in %s expected in %s", symbolName, this->getDylibID(), binder->getDylibID());

	// don't bind lazy pointers to resolver stubs in shared cache
	if ( lazyPointer && isResolverSymbol ) {
        if ( foundIn == this ) {
            // record location of lazy pointer for other dylibs to re-use
            pint_t  lpVMAddr = segments[segmentIndex].newAddress() + segmentOffset;
            foundIn->addResolverLazyPointerMappedAddress(symbolName, lpVMAddr);
            //fprintf(stderr, "resolver %s in %s has lazy pointer with segmentOffset=0x%08llX\n", symbolName, this->getDylibID(), segmentOffset);
       }
        else {
            // record that this dylib has a lazy pointer to a resolver function
            foundIn->addResolverClient(this, symbolName);
           // fprintf(stderr, "have lazy pointer to resolver %s in %s\n", symbolName, this->getDylibID());
        }
		return;
    }

	// do actual update
	const MachOLayoutAbstraction::Segment& seg = segments[segmentIndex];
	uint8_t*  mappedAddr = (uint8_t*)seg.mappedAddress() + segmentOffset;
	pint_t*   mappedAddrP = (pint_t*)mappedAddr;
	uint32_t* mappedAddr32 = (uint32_t*)mappedAddr;
	int32_t svalue32new;
	switch ( type ) {
		case BIND_TYPE_POINTER:
			P::setP(*mappedAddrP, targetSymbolAddress + addend);
			break;
		
		case BIND_TYPE_TEXT_ABSOLUTE32:
			E::set32(*mappedAddr32, targetSymbolAddress + addend);
			break;
			
		case BIND_TYPE_TEXT_PCREL32:
			svalue32new = seg.address() + segmentOffset + 4 - (targetSymbolAddress + addend);
			E::set32(*mappedAddr32, svalue32new);
			break;
		
		default:
			throw "bad bind type";
	}
	pointersInData.push_back(mappedAddr);
}



template <typename A>
void Binder<A>::doBindDyldLazyInfo(std::vector<void*>& pointersInData)
{
	const uint8_t* p = &this->fLinkEditBase[fDyldInfo->lazy_bind_off()];
	const uint8_t* end = &p[fDyldInfo->lazy_bind_size()];
	
	uint8_t type = BIND_TYPE_POINTER;
	uint64_t segmentOffset = 0;
	uint8_t segmentIndex = 0;
	const char* symbolName = NULL;
	int libraryOrdinal = 0;
	int64_t addend = 0;
	while ( p < end ) {
		uint8_t immediate = *p & BIND_IMMEDIATE_MASK;
		uint8_t opcode = *p & BIND_OPCODE_MASK;
		++p;
		switch (opcode) {
			case BIND_OPCODE_DONE:
				// this opcode marks the end of each lazy pointer binding
				break;
			case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
				libraryOrdinal = immediate;
				break;
			case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
				libraryOrdinal = read_uleb128(p, end);
				break;
			case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
				// the special ordinals are negative numbers
				if ( immediate == 0 )
					libraryOrdinal = 0;
				else {
					int8_t signExtended = BIND_OPCODE_MASK | immediate;
					libraryOrdinal = signExtended;
				}
				break;
			case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
				symbolName = (char*)p;
				while (*p != '\0')
					++p;
				++p;
				break;
			case BIND_OPCODE_SET_ADDEND_SLEB:
				addend = read_sleb128(p, end);
				break;
			case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
				segmentIndex = immediate;
				segmentOffset = read_uleb128(p, end);
				break;
			case BIND_OPCODE_DO_BIND:
				bindDyldInfoAt(segmentIndex, segmentOffset, type, libraryOrdinal, addend, symbolName, true, pointersInData);
				segmentOffset += sizeof(pint_t);
				break;
			case BIND_OPCODE_SET_TYPE_IMM:
			case BIND_OPCODE_ADD_ADDR_ULEB:
			case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
			case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
			case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
			default:
				throwf("bad lazy bind opcode %d", *p);
		}
	}	
	

}

template <typename A>
void Binder<A>::doBindDyldInfo(std::vector<void*>& pointersInData)
{
	const uint8_t* p = &this->fLinkEditBase[fDyldInfo->bind_off()];
	const uint8_t* end = &p[fDyldInfo->bind_size()];
	
	uint8_t type = 0;
	uint64_t segmentOffset = 0;
	uint8_t segmentIndex = 0;
	const char* symbolName = NULL;
	int libraryOrdinal = 0;
	int64_t addend = 0;
	uint32_t count;
	uint32_t skip;
	bool done = false;
	while ( !done && (p < end) ) {
		uint8_t immediate = *p & BIND_IMMEDIATE_MASK;
		uint8_t opcode = *p & BIND_OPCODE_MASK;
		++p;
		switch (opcode) {
			case BIND_OPCODE_DONE:
				done = true;
				break;
			case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
				libraryOrdinal = immediate;
				break;
			case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
				libraryOrdinal = read_uleb128(p, end);
				break;
			case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
				// the special ordinals are negative numbers
				if ( immediate == 0 )
					libraryOrdinal = 0;
				else {
					int8_t signExtended = BIND_OPCODE_MASK | immediate;
					libraryOrdinal = signExtended;
				}
				break;
			case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
				symbolName = (char*)p;
				while (*p != '\0')
					++p;
				++p;
				break;
			case BIND_OPCODE_SET_TYPE_IMM:
				type = immediate;
				break;
			case BIND_OPCODE_SET_ADDEND_SLEB:
				addend = read_sleb128(p, end);
				break;
			case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
				segmentIndex = immediate;
				segmentOffset = read_uleb128(p, end);
				break;
			case BIND_OPCODE_ADD_ADDR_ULEB:
				segmentOffset += read_uleb128(p, end);
				break;
			case BIND_OPCODE_DO_BIND:
				bindDyldInfoAt(segmentIndex, segmentOffset, type, libraryOrdinal, addend, symbolName, false, pointersInData);
				segmentOffset += sizeof(pint_t);
				break;
			case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
				bindDyldInfoAt(segmentIndex, segmentOffset, type, libraryOrdinal, addend, symbolName, false, pointersInData);
				segmentOffset += read_uleb128(p, end) + sizeof(pint_t);
				break;
			case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
				bindDyldInfoAt(segmentIndex, segmentOffset, type, libraryOrdinal, addend, symbolName, false, pointersInData);
				segmentOffset += immediate*sizeof(pint_t) + sizeof(pint_t);
				break;
			case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
				count = read_uleb128(p, end);
				skip = read_uleb128(p, end);
				for (uint32_t i=0; i < count; ++i) {
					bindDyldInfoAt(segmentIndex, segmentOffset, type, libraryOrdinal, addend, symbolName, false, pointersInData);
					segmentOffset += skip + sizeof(pint_t);
				}
				break;
			default:
				throwf("bad bind opcode %d", *p);
		}
	}	

	

}


template <typename A>
void Binder<A>::doSetPreboundUndefines()
{
	const macho_dysymtab_command<P>* dysymtab = NULL;
	macho_nlist<P>* symbolTable = NULL;

	// get symbol table info
	const macho_load_command<P>* const cmds = (macho_load_command<P>*)((uint8_t*)this->fHeader + sizeof(macho_header<P>));
	const uint32_t cmd_count = this->fHeader->ncmds();
	const macho_load_command<P>* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd()) {
			case LC_SYMTAB:
				{
					const macho_symtab_command<P>* symtab = (macho_symtab_command<P>*)cmd;
					symbolTable = (macho_nlist<P>*)(&this->fLinkEditBase[symtab->symoff()]);
				}
				break;
			case LC_DYSYMTAB:
				dysymtab = (macho_dysymtab_command<P>*)cmd;
				break;
		}
		cmd = (const macho_load_command<P>*)(((uint8_t*)cmd)+cmd->cmdsize());
	}	
	
	// walk all undefines and set their prebound n_value
	macho_nlist<P>* const lastUndefine = &symbolTable[dysymtab->iundefsym()+dysymtab->nundefsym()];
	for (macho_nlist<P>* entry = &symbolTable[dysymtab->iundefsym()]; entry < lastUndefine; ++entry) {
		if ( entry->n_type() & N_EXT ) {
			//fprintf(stderr, "doSetPreboundUndefines: r_sym=%s, pbaddr=0x%08X, in %s\n", 
			//	&fStrings[entry->n_strx()], pbaddr, this->getDylibID());
			pint_t pbaddr = this->resolveUndefined(entry);
			entry->set_n_value(pbaddr);
		}
	}
}


template <typename A>
void Binder<A>::doBindExternalRelocations()
{
	// get where reloc addresses start
	// these address are always relative to first writable segment because they are in cache which always
	// has writable segments far from read-only segments
	pint_t firstWritableSegmentBaseAddress = 0;
	const std::vector<MachOLayoutAbstraction::Segment>& segments = this->fLayout.getSegments();
	for(std::vector<MachOLayoutAbstraction::Segment>::const_iterator it = segments.begin(); it != segments.end(); ++it) {
		const MachOLayoutAbstraction::Segment& seg = *it;
		if ( seg.writable() ) {
			firstWritableSegmentBaseAddress = seg.newAddress();
			break;
		}
	}	
	
	// loop through all external relocation records and bind each
	const macho_relocation_info<P>* const relocsStart = (macho_relocation_info<P>*)(&this->fLinkEditBase[fDynamicInfo->extreloff()]);
	const macho_relocation_info<P>* const relocsEnd = &relocsStart[fDynamicInfo->nextrel()];
	for (const macho_relocation_info<P>* reloc=relocsStart; reloc < relocsEnd; ++reloc) {
		if ( reloc->r_length() != pointerRelocSize() ) 
			throw "bad external relocation length";
		if ( reloc->r_type() != pointerRelocType() ) 
			throw "unknown external relocation type";
		if ( reloc->r_pcrel() ) 
			throw "r_pcrel external relocaiton not supported";

		const macho_nlist<P>* undefinedSymbol = &fSymbolTable[reloc->r_symbolnum()];
		pint_t* location;
		try {
			location = this->mappedAddressForNewAddress(reloc->r_address() + firstWritableSegmentBaseAddress);
		}
		catch (const char* msg) {
			throwf("%s processesing external relocation r_address 0x%08X", msg, reloc->r_address());
		}
		pint_t addend =  P::getP(*location);
		if ( fOriginallyPrebound ) {
			// in a prebound binary, the n_value field of an undefined symbol is set to the address where the symbol was found when prebound
			// so, subtracting that gives the initial displacement which we need to add to the newly found symbol address
			// if mach-o relocation structs had an "addend" field this complication would not be necessary.
			addend -= undefinedSymbol->n_value();
			// To further complicate things, if this is defined symbol, then its n_value has already been adjust to the
			// new base address, so we need to back off the slide too..
			if ( (undefinedSymbol->n_type() & N_TYPE) == N_SECT ) {
				addend += this->getSlideForNewAddress(undefinedSymbol->n_value());
			}
		}
		pint_t symbolAddr = this->resolveUndefined(undefinedSymbol);
		//fprintf(stderr, "external reloc: r_address=0x%08X, r_sym=%s, symAddr=0x%08llX, addend=0x%08llX in %s\n", 
		//		reloc->r_address(), &fStrings[undefinedSymbol->n_strx()], (uint64_t)symbolAddr, (uint64_t)addend, this->getDylibID());
		P::setP(*location, symbolAddr + addend); 
	}
}


// most architectures use pure code, unmodifiable stubs
template <typename A>
void Binder<A>::bindStub(uint8_t elementSize, uint8_t* location, pint_t vmlocation, pint_t value)
{
	// do nothing
}

// x86 supports fast stubs
template <>
void Binder<x86>::bindStub(uint8_t elementSize, uint8_t* location, pint_t vmlocation, pint_t value)
{
	// if the stub is not 5-bytes, it is an old slow stub
	if ( elementSize == 5 ) {
		uint32_t rel32 = value - (vmlocation + 5);
		location[0] = 0xE9; // JMP rel32
		location[1] = rel32 & 0xFF;
		location[2] = (rel32 >> 8) & 0xFF;
		location[3] = (rel32 >> 16) & 0xFF;
		location[4] = (rel32 >> 24) & 0xFF;
	}
}

template <typename A>
void Binder<A>::doBindIndirectSymbols()
{
	const uint32_t* const indirectTable = (uint32_t*)&this->fLinkEditBase[fDynamicInfo->indirectsymoff()];
	const macho_load_command<P>* const cmds = (macho_load_command<P>*)((uint8_t*)this->fHeader + sizeof(macho_header<P>));
	const uint32_t cmd_count = this->fHeader->ncmds();
	const macho_load_command<P>* cmd = cmds;
	//fprintf(stderr, "doBindIndirectSymbols() %s\n", this->fLayout.getFilePath());
	for (uint32_t i = 0; i < cmd_count; ++i) {
		if ( cmd->cmd() == macho_segment_command<P>::CMD ) {
			const macho_segment_command<P>* seg = (macho_segment_command<P>*)cmd;
			const macho_section<P>* const sectionsStart = (macho_section<P>*)((uint8_t*)seg + sizeof(macho_segment_command<P>));
			const macho_section<P>* const sectionsEnd = &sectionsStart[seg->nsects()];
			for (const macho_section<P>* sect=sectionsStart; sect < sectionsEnd; ++sect) {
				uint8_t elementSize = 0;
				uint8_t sectionType = sect->flags() & SECTION_TYPE;
				switch ( sectionType ) {
					case S_SYMBOL_STUBS:
						elementSize = sect->reserved2();
						break;
					case S_NON_LAZY_SYMBOL_POINTERS:
					case S_LAZY_SYMBOL_POINTERS:
						elementSize = sizeof(pint_t);
						break;
				}
				if ( elementSize != 0 ) {
					uint32_t elementCount = sect->size() / elementSize;
					const uint32_t indirectTableOffset = sect->reserved1();
					uint8_t* location = NULL; 
					if ( sect->size() != 0 )
						location = (uint8_t*)this->mappedAddressForNewAddress(sect->addr());
					pint_t vmlocation = sect->addr();
					for (uint32_t j=0; j < elementCount; ++j, location += elementSize, vmlocation += elementSize) {
						uint32_t symbolIndex = E::get32(indirectTable[indirectTableOffset + j]); 
						switch ( symbolIndex ) {
							case INDIRECT_SYMBOL_ABS:
							case INDIRECT_SYMBOL_LOCAL:
								break;
							default:
								const macho_nlist<P>* undefinedSymbol = &fSymbolTable[symbolIndex];
								//fprintf(stderr, " sect=%s, index=%d, symbolIndex=%d, sym=%s\n", sect->sectname(), j, symbolIndex, &fStrings[undefinedSymbol->n_strx()]);
								pint_t symbolAddr = this->resolveUndefined(undefinedSymbol);
								switch ( sectionType ) {
									case S_NON_LAZY_SYMBOL_POINTERS:
									case S_LAZY_SYMBOL_POINTERS:
										P::setP(*((pint_t*)location), symbolAddr); 
										break;
									case S_SYMBOL_STUBS:
										this->bindStub(elementSize, location, vmlocation, symbolAddr);
											break;
								}
								break;
						}
					}
				}
			}
		}
		cmd = (const macho_load_command<P>*)(((uint8_t*)cmd)+cmd->cmdsize());
	}
}




template <typename A>
typename A::P::uint_t Binder<A>::resolveUndefined(const macho_nlist<P>* undefinedSymbol)
{
	if ( (undefinedSymbol->n_type() & N_TYPE) == N_SECT ) {
		if ( (undefinedSymbol->n_type() & N_PEXT) != 0 ) {
			// is a multi-module private_extern internal reference that the linker did not optimize away
			return runtimeAddressFromNList(undefinedSymbol);
		}
		if ( (undefinedSymbol->n_desc() & N_WEAK_DEF) != 0 ) {
			// is a weak definition, we should prebind to this one in the same linkage unit
			return runtimeAddressFromNList(undefinedSymbol);
		}
	}
	const char* symbolName = &fStrings[undefinedSymbol->n_strx()];
	if ( (this->fHeader->flags() & MH_TWOLEVEL) == 0 ) {
		// flat namespace binding
		throw "flat namespace not supported";
	}
	else {
		uint8_t ordinal = GET_LIBRARY_ORDINAL(undefinedSymbol->n_desc());
		Binder<A>* binder = NULL;
		switch ( ordinal ) {
			case EXECUTABLE_ORDINAL:
			case DYNAMIC_LOOKUP_ORDINAL:
				throw "magic ordineal not supported";
			case SELF_LIBRARY_ORDINAL:
				binder = this;
				break;
			default:
				if ( ordinal > fDependentDylibs.size() )
					throw "two-level ordinal out of range";
				binder = fDependentDylibs[ordinal-1].binder;
		}	
		pint_t addr;
		bool isResolver;
        Binder<A>* foundIn;
		if ( ! binder->findExportedSymbolAddress(symbolName, &addr, &foundIn, &isResolver) )
			throwf("could not resolve undefined symbol %s in %s expected in %s", symbolName, this->getDylibID(), binder->getDylibID());
		return addr;
	}
}

template <typename A>
bool Binder<A>::findExportedSymbolAddress(const char* name, pint_t* result, Binder<A>** foundIn, bool* isResolverSymbol)
{
    *foundIn = NULL;
	// since re-export chains can be any length, re-exports cannot be resolved in setDependencies()
	// instead we lazily, recursively update 
	if ( !fReExportedSymbolsResolved ) {
		
		// update fHashTable with any individual symbol re-exports
		for (typename std::vector<SymbolReExport>::iterator it=fReExportedSymbols.begin(); it != fReExportedSymbols.end(); ++it) {
			pint_t targetSymbolAddress;
			bool isResolver;

			if ( it->dylibOrdinal <= 0 ) 
				throw "bad mach-o binary, special library ordinal not allowed in re-exported symbols in dyld shared cache";
			
			Binder<A>* binder = fDependentDylibs[it->dylibOrdinal-1].binder;
			
			if ( ! binder->findExportedSymbolAddress(it->importName, &targetSymbolAddress, foundIn, &isResolver) ) 
				throwf("could not bind symbol %s in %s expected in %s", it->importName, this->getDylibID(), binder->getDylibID());

			if ( isResolver ) 
				throw "bad mach-o binary, re-export of resolvers symbols not supported in dyld shared cache";

			fHashTable[it->exportName] = targetSymbolAddress;
		}
		// mark as done
		fReExportedSymbolsResolved = true;
	}

	*isResolverSymbol = false;
	if ( !fSymbolResolvers.empty() && fSymbolResolvers.count(name) ) {
		// lazy pointers should be left unbound, rather than bind to resolver stub
		*isResolverSymbol = true;
	}
	
	typename NameToAddrMap::iterator pos = fHashTable.find(name);
	if ( pos != fHashTable.end() ) {
		*result = pos->second;
		//fprintf(stderr, "findExportedSymbolAddress(%s) => 0x%08llX in %s\n", name, (uint64_t)*result, this->getDylibID());
        *foundIn = this;
		return true;
	}

	// search re-exports
	for (typename std::vector<BinderAndReExportFlag>::iterator it = fDependentDylibs.begin(); it != fDependentDylibs.end(); ++it) {
		if ( it->reExport ) {
			if ( it->binder->findExportedSymbolAddress(name, result, foundIn, isResolverSymbol) )
				return true;
		}
	}
	//fprintf(stderr, "findExportedSymbolAddress(%s) => not found in %s\n", name, this->getDylibID());
	return false;
}

// record which dylibs will be using this dylibs lazy pointer
template <typename A>
void Binder<A>::addResolverClient(Binder<A>* clientDylib, const char* symbolName)
{
    ClientAndSymbol x;
    x.client = clientDylib;
    x.symbolName = symbolName;
    fClientAndSymbols.push_back(x);
}                                        

// Record that this dylib has an lazy pointer that points within itself for use
// with a resolver function.
template <typename A>
void Binder<A>::addResolverLazyPointerMappedAddress(const char* symbolName, pint_t lpVMAddr)
{
    SymbolAndLazyPointer x;
    x.symbolName = symbolName;
    x.lpVMAddr = lpVMAddr;
    fSymbolAndLazyPointers.push_back(x);
}

template <typename A>
typename A::P::uint_t Binder<A>::findLazyPointerFor(const char* symbolName)
{
    for (typename std::vector<SymbolAndLazyPointer>::iterator it = fSymbolAndLazyPointers.begin(); it != fSymbolAndLazyPointers.end(); ++it) {
        if ( strcmp(it->symbolName, symbolName) == 0 )
            return it->lpVMAddr;
    }
    return 0;
}

// called after all binding is done to optimize lazy pointers
template <typename A>
void Binder<A>::optimize()
{
    for (typename std::vector<ClientAndSymbol>::iterator it = fClientAndSymbols.begin(); it != fClientAndSymbols.end(); ++it) {
        pint_t lpVMAddr = findLazyPointerFor(it->symbolName);
        if ( lpVMAddr != 0 ) {
            it->client->optimizeStub(it->symbolName, lpVMAddr);
        }
        else {
            fprintf(stderr, "not able to optimize lazy pointer for %s in %s\n", it->symbolName, it->client->getDylibID());
        }
        
    }
}

template <>
void Binder<arm>::optimizeStub(uint8_t* stubMappedAddress, pint_t stubVMAddress, uint32_t stubSize, pint_t lpVMAddr)
{
    if ( stubSize != 16 ) {
        fprintf(stderr, "could not optimize ARM stub to resolver function in %s because it is wrong size\n", this->getDylibID());
        return;
    }
    uint32_t* instructions = (uint32_t*)stubMappedAddress;
    if (   (E::get32(instructions[0]) != 0xe59fc004)
        || (E::get32(instructions[1]) != 0xe08fc00c)
        || (E::get32(instructions[2]) != 0xe59cf000)
        ) {
        fprintf(stderr, "could not optimize ARM stub to resolver function in %s because instructions are not as expected\n", this->getDylibID());
        return;
    }
    // last .long in stub is:  lazyPtr - (stub+8)
    // alter to point to more optimal lazy pointer
    uint32_t betterOffset = lpVMAddr  - (stubVMAddress + 12);
    E::set32(instructions[3], betterOffset);
}


template <>
void Binder<x86_64>::optimizeStub(uint8_t* stubMappedAddress, pint_t stubVMAddress, uint32_t stubSize, pint_t lpVMAddr)
{
    if ( stubSize != 6 ) {
        fprintf(stderr, "could not optimize x86_64 stub to resolver function in %s because it is wrong size\n", this->getDylibID());
        return;
    }
    if ( (stubMappedAddress[0] != 0xFF) || (stubMappedAddress[1] != 0x25) ) {
        fprintf(stderr, "could not optimize stub to resolver function in %s because instructions are not as expected\n", this->getDylibID());
        return;
    }
    // last four bytes in stub is RIP relative offset to lazy pointer
    // alter to point to more optimal lazy pointer
    uint32_t betterOffset = lpVMAddr  - (stubVMAddress + 6);
    E::set32(*((uint32_t*)(&stubMappedAddress[2])), betterOffset);
}

template <typename A>
void Binder<A>::optimizeStub(uint8_t* stubMappedAddress, pint_t stubVMAddress, uint32_t stubSize, pint_t lpVMAddress)
{
    // Remaining architectures are not optimized
    //fprintf(stderr, "optimize stub at %p in %s to use lazyPointer at 0x%llX\n", stubMappedAddress, this->getDylibID(), (uint64_t)lpVMAddress);
}

// search for stub in this image that call target symbol name and then optimize its lazy pointer
template <typename A>
void Binder<A>::optimizeStub(const char* stubName, pint_t lpVMAddr)
{
	// find named stub
	const uint32_t* const indirectTable = (uint32_t*)&this->fLinkEditBase[fDynamicInfo->indirectsymoff()];
	const macho_load_command<P>* const cmds = (macho_load_command<P>*)((uint8_t*)this->fHeader + sizeof(macho_header<P>));
	const uint32_t cmd_count = this->fHeader->ncmds();
	const macho_load_command<P>* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		if ( cmd->cmd() == macho_segment_command<P>::CMD ) {
			macho_segment_command<P>* seg = (macho_segment_command<P>*)cmd;
			macho_section<P>* const sectionsStart = (macho_section<P>*)((char*)seg + sizeof(macho_segment_command<P>));
			macho_section<P>* const sectionsEnd = &sectionsStart[seg->nsects()];
			for(macho_section<P>* sect = sectionsStart; sect < sectionsEnd; ++sect) {
				if ( ((sect->flags() & SECTION_TYPE) == S_SYMBOL_STUBS) && (sect->size() != 0) ) {
					pint_t stubsVMStart = sect->addr();
					uint8_t* stubsMappingStart = (uint8_t*)this->mappedAddressForNewAddress(stubsVMStart);
					const uint32_t indirectTableOffset = sect->reserved1();
					const uint32_t stubSize = sect->reserved2();
					uint32_t elementCount = sect->size() / stubSize;
                    pint_t stubVMAddr = stubsVMStart;
                    uint8_t* stubMappedAddr = stubsMappingStart;
					for (uint32_t j=0; j < elementCount; ++j, stubMappedAddr += stubSize, stubVMAddr += stubSize) {
						uint32_t symbolIndex = E::get32(indirectTable[indirectTableOffset + j]); 
						switch ( symbolIndex ) {
							case INDIRECT_SYMBOL_ABS:
							case INDIRECT_SYMBOL_LOCAL:
								break;
							default:
                                {
                                   const macho_nlist<P>* sym = &this->fSymbolTable[symbolIndex];
                                   const char* symName = &fStrings[sym->n_strx()];
                                    if ( strcmp(symName, stubName) == 0 ) 
                                        this->optimizeStub(stubMappedAddr, stubVMAddr, stubSize, lpVMAddr);
                                }
 								break;
						}
					}
				}
			}
		}
		cmd = (const macho_load_command<P>*)(((uint8_t*)cmd)+cmd->cmdsize());
	}
}


#endif // __MACHO_BINDER__




