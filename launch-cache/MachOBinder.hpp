/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*- 
 *
 * Copyright (c) 2006 Apple Computer, Inc. All rights reserved.
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

#include "MachOFileAbstraction.hpp"
#include "Architectures.hpp"
#include "MachOLayout.hpp"
#include "MachORebaser.hpp"




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
	void										bind();

private:
	typedef typename A::P					P;
	typedef typename A::P::E				E;
	typedef typename A::P::uint_t			pint_t;
	struct BinderAndReExportFlag { Binder<A>* binder; bool reExport; };
	typedef __gnu_cxx::hash_map<const char*, const macho_nlist<P>*, __gnu_cxx::hash<const char*>, CStringEquals> NameToSymbolMap;
	
	void										doBindExternalRelocations();
	void										doBindIndirectSymbols();
	void										doSetUpDyldSection();
	void										doSetPreboundUndefines();
	pint_t										resolveUndefined(const macho_nlist<P>* undefinedSymbol);
	const macho_nlist<P>*						findExportedSymbol(const char* name);
	void										bindStub(uint8_t elementSize, uint8_t* location, pint_t vmlocation, pint_t value);
	const char*									parentUmbrella();

	static uint8_t								pointerRelocSize();
	static uint8_t								pointerRelocType();
	
	std::vector<BinderAndReExportFlag>			fDependentDylibs;
	NameToSymbolMap								fHashTable;
	uint64_t									fDyldBaseAddress;
	const macho_nlist<P>*						fSymbolTable;
	const char*									fStrings;
	const macho_dysymtab_command<P>*			fDynamicInfo;
	const macho_segment_command<P>*				fFristWritableSegment;
	const macho_dylib_command<P>*				fDylibID;
	const macho_dylib_command<P>*				fParentUmbrella;
	bool										fOriginallyPrebound;
};


template <typename A>
Binder<A>::Binder(const MachOLayoutAbstraction& layout, uint64_t dyldBaseAddress)
	: Rebaser<A>(layout), fDyldBaseAddress(dyldBaseAddress),
	  fSymbolTable(NULL), fStrings(NULL), fDynamicInfo(NULL),
	  fFristWritableSegment(NULL), fDylibID(NULL),
	  fParentUmbrella(NULL)
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
				((macho_dylib_command<P>*)cmd)->set_timestamp(0);
				break;
			case LC_SUB_FRAMEWORK:
				fParentUmbrella = (macho_dylib_command<P>*)cmd;
				break;
			default:
				if ( cmd->cmd() & LC_REQ_DYLD )
					throwf("unknown required load command %d", cmd->cmd());
		}
		cmd = (const macho_load_command<P>*)(((uint8_t*)cmd)+cmd->cmdsize());
	}	
	if ( fDynamicInfo == NULL )	
		throw "no LC_DYSYMTAB";
	if ( fSymbolTable == NULL )	
		throw "no LC_SYMTAB";
	// build hash table
	if ( fDynamicInfo->tocoff() == 0 ) {
		const macho_nlist<P>* start = &fSymbolTable[fDynamicInfo->iextdefsym()];
		const macho_nlist<P>* end = &start[fDynamicInfo->nextdefsym()];
		fHashTable.resize(fDynamicInfo->nextdefsym()); // set initial bucket count
		for (const macho_nlist<P>* sym=start; sym < end; ++sym) {
			const char* name = &fStrings[sym->n_strx()];
			fHashTable[name] = sym;
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
			fHashTable[name] = sym;
		}
	}

}

template <> uint8_t	Binder<ppc>::pointerRelocSize()    { return 2; }
template <> uint8_t	Binder<ppc64>::pointerRelocSize()  { return 3; }
template <> uint8_t	Binder<x86>::pointerRelocSize()   { return 2; }
template <> uint8_t	Binder<x86_64>::pointerRelocSize() { return 3; }

template <> uint8_t	Binder<ppc>::pointerRelocType()    { return GENERIC_RELOC_VANILLA; }
template <> uint8_t	Binder<ppc64>::pointerRelocType()  { return GENERIC_RELOC_VANILLA; }
template <> uint8_t	Binder<x86>::pointerRelocType()   { return GENERIC_RELOC_VANILLA; }
template <> uint8_t	Binder<x86_64>::pointerRelocType() { return X86_64_RELOC_UNSIGNED; }


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
void Binder<A>::bind()
{
	this->doSetUpDyldSection();
	this->doBindExternalRelocations();
	this->doBindIndirectSymbols();
	this->doSetPreboundUndefines();
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
			pint_t pbaddr = this->resolveUndefined(entry);
			//fprintf(stderr, "doSetPreboundUndefines: r_sym=%s, pbaddr=0x%08X, in %s\n", 
			//	&fStrings[entry->n_strx()], pbaddr, this->getDylibID());
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
			location = mappedAddressForNewAddress(reloc->r_address() + firstWritableSegmentBaseAddress);
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
			return undefinedSymbol->n_value();
		}
		if ( (undefinedSymbol->n_desc() & N_WEAK_DEF) != 0 ) {
			// is a weak definition, we should prebind to this one in the same linkage unit
			return undefinedSymbol->n_value();
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
		const macho_nlist<P>* sym = binder->findExportedSymbol(symbolName);
		if ( sym == NULL ) 
			throwf("could not resolve %s from %s", symbolName, this->getDylibID());
		return sym->n_value();
	}
}

template <typename A>
const macho_nlist<typename A::P>* Binder<A>::findExportedSymbol(const char* name)
{
	//fprintf(stderr, "findExportedSymbol(%s) in %s\n", name, this->getDylibID());
	const macho_nlist<P>* sym = NULL;
	typename NameToSymbolMap::iterator pos = fHashTable.find(name);
	if ( pos != fHashTable.end() ) 
		return pos->second;

	// search re-exports
	for (typename std::vector<BinderAndReExportFlag>::iterator it = fDependentDylibs.begin(); it != fDependentDylibs.end(); ++it) {
		if ( it->reExport ) {
			sym = it->binder->findExportedSymbol(name);
			if ( sym != NULL )
				return sym;
		}
	}
	return NULL;
}



#endif // __MACHO_BINDER__




