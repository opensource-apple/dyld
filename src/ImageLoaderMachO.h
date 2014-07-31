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


#ifndef __IMAGELOADERMACHO__
#define __IMAGELOADERMACHO__

#include <stdint.h> 

#include "ImageLoader.h"
#include "mach-o/dyld_images.h"

struct sf_mapping;
struct _shared_region_mapping_np;


//
// ImageLoaderMachO is the concrete subclass of ImageLoader which loads mach-o format files.
// The class is written to be 64-bit clean and support both 32-bit and 64-bit executables in
// mach-o.
//
//
class ImageLoaderMachO : public ImageLoader {
public:
										ImageLoaderMachO(const char* path, int fd, const uint8_t firstPage[4096], uint64_t offset, uint64_t len, const struct stat& info, const LinkContext& context);
										ImageLoaderMachO(const char* moduleName, const struct mach_header* mh, uint64_t len, const LinkContext& context);
										ImageLoaderMachO(const struct mach_header* mh, const char* path, const struct stat& info, const LinkContext& context);
										ImageLoaderMachO(const struct mach_header* executableHeader, uintptr_t executableSlide, const char* path, const LinkContext& context);
	virtual								~ImageLoaderMachO();

	const char*							getInstallPath() const;
	virtual void*						getMain() const;
	virtual const struct mach_header*   machHeader() const;
	virtual uintptr_t					getSlide() const;
	virtual const void*					getEnd() const;
	virtual bool						hasCoalescedExports() const;
	virtual const Symbol*				findExportedSymbol(const char* name, const void* hint, bool searchReExports, const ImageLoader** foundIn) const;
	virtual uintptr_t					getExportedSymbolAddress(const Symbol* sym, const LinkContext& context, const ImageLoader* requestor) const;
	virtual DefinitionFlags				getExportedSymbolInfo(const Symbol* sym) const;
	virtual const char*					getExportedSymbolName(const Symbol* sym) const;
	virtual uint32_t					getExportedSymbolCount() const;
	virtual const Symbol*				getIndexedExportedSymbol(uint32_t index) const;
	virtual uint32_t					getImportedSymbolCount() const;
	virtual const Symbol*				getIndexedImportedSymbol(uint32_t index) const;
	virtual ReferenceFlags				geImportedSymbolInfo(const Symbol* sym) const;
	virtual const char*					getImportedSymbolName(const Symbol* sym) const;
	virtual bool						isBundle() const;
	virtual bool						isDylib() const;
	virtual bool						forceFlat() const;
	virtual uintptr_t					doBindLazySymbol(uintptr_t* lazyPointer, const LinkContext& context);
	virtual void						doTermination(const LinkContext& context);
#if IMAGE_NOTIFY_SUPPORT
	virtual void						doNotification(enum dyld_image_mode mode, uint32_t infoCount, const struct dyld_image_info info[]);
#endif
	virtual bool						needsInitialization();
#if IMAGE_NOTIFY_SUPPORT
	virtual bool						hasImageNotification();
#endif
	virtual bool						getSectionContent(const char* segmentName, const char* sectionName, void** start, size_t* length);
	virtual bool						findSection(const void* imageInterior, const char** segmentName, const char** sectionName, size_t* sectionOffset);
	virtual bool						usablePrebinding(const LinkContext& context) const;
			
	
	static void							printStatistics(unsigned int imageCount);
	
protected:
						ImageLoaderMachO(const ImageLoaderMachO&);
	void				operator=(const ImageLoaderMachO&);

	virtual uint32_t	doGetDependentLibraryCount();
	virtual void		doGetDependentLibraries(DependentLibraryInfo libs[]);
	virtual LibraryInfo doGetLibraryInfo();
	virtual	void		getRPaths(const LinkContext& context, std::vector<const char*>&) const;
	virtual void		doRebase(const LinkContext& context);
	virtual void		doBind(const LinkContext& context, bool forceLazysBound);
	virtual void		doBindJustLazies(const LinkContext& context);
	virtual void		doUpdateMappingPermissions(const LinkContext& context);
	virtual void		doInitialization(const LinkContext& context);
	virtual void		doGetDOFSections(const LinkContext& context, std::vector<ImageLoader::DOFInfo>& dofs);
	virtual bool		needsTermination();
	virtual void		instantiateSegments(const uint8_t* fileData);
	virtual bool		segmentsMustSlideTogether() const;	
	virtual bool		segmentsCanSlide() const;			
	virtual void		setSlide(intptr_t slide);		
	virtual bool		usesTwoLevelNameSpace() const;
	virtual	bool		isSubframeworkOf(const LinkContext& context, const ImageLoader* image) const;
	virtual	bool		hasSubLibrary(const LinkContext& context, const ImageLoader* child) const;
	virtual bool		isPrebindable() const;
	virtual SegmentIterator	beginSegments() const;
	virtual SegmentIterator	endSegments() const;

#if SPLIT_SEG_DYLIB_SUPPORT
	virtual void		mapSegments(int fd, uint64_t offsetInFat, uint64_t lenInFat, uint64_t fileLen, const LinkContext& context);
#endif 
		
private:
	friend class SegmentMachO;

			void		init();
			void		parseLoadCmds();
			uintptr_t	bindIndirectSymbol(uintptr_t* ptrToBind, const struct macho_section* sect, const char* symbolName, uintptr_t targetAddr, const ImageLoader* targetImage, const LinkContext& context);
			void		doBindIndirectSymbolPointers(const LinkContext& context, bool bindNonLazys, bool bindLazys, bool onlyCoalescedSymbols);
			void		doBindExternalRelocations(const LinkContext& context, bool onlyCoalescedSymbols);
			uintptr_t	resolveUndefined(const LinkContext& context, const struct macho_nlist* symbol, bool twoLevel, const ImageLoader **foundIn);
			uintptr_t	getRelocBase();
			uintptr_t	getFirstWritableSegmentAddress();
			void		resetPreboundLazyPointers(const LinkContext& context, uintptr_t relocBase);
			void		doImageInit(const LinkContext& context);
			void		doModInitFunctions(const LinkContext& context);
			void		setupLazyPointerHandler(const LinkContext& context);
			void		lookupProgramVars(const LinkContext& context) const;
#if SPLIT_SEG_DYLIB_SUPPORT	
			unsigned int getExtraZeroFillEntriesCount();
			void		initMappingTable(uint64_t offsetInFat, _shared_region_mapping_np *mappingTable);
			int			sharedRegionMapFilePrivateOutside(int fd, uint64_t offsetInFat, uint64_t lenInFat, uint64_t fileLen, const LinkContext& context);
#endif
#if SPLIT_SEG_SHARED_REGION_SUPPORT
			int			sharedRegionLoadFile(int fd, uint64_t offsetInFat, uint64_t lenInFat, uint64_t fileLen, const LinkContext& context);
			int			sharedRegionMapFile(int fd, uint64_t offsetInFat, uint64_t lenInFat, uint64_t fileLen, const LinkContext& context);
			int			sharedRegionMakePrivate(const LinkContext& context);
			int			sharedRegionMapFilePrivate(int fd, uint64_t offsetInFat, uint64_t lenInFat, uint64_t fileLen, const LinkContext& context, bool usemmap);
			void		initMappingTable(uint64_t offsetInFat, sf_mapping *mappingTable, uintptr_t baseAddress);
#endif
			bool		needsCoalescing() const;
			bool		isAddrInSection(uintptr_t addr, uint8_t sectionIndex);
			void		adjustSegments();
			uintptr_t	getSymbolAddress(const struct macho_nlist* sym, const ImageLoader* requestor, const LinkContext& context) const;
			void		preFetch(int fd, uint64_t offsetInFat, const LinkContext& context);
#if __i386__
			void		makeImportSegmentWritable(const LinkContext& context);
			void		makeImportSegmentReadOnly(const LinkContext& context);
#endif
			
	static	bool				symbolRequiresCoalescing(const struct macho_nlist* symbol); 
	static uintptr_t			bindLazySymbol(const mach_header*, uintptr_t* lazyPointer);
	const struct macho_nlist*  binarySearch(const char* key, const char stringPool[], const struct macho_nlist symbols[], uint32_t symbolCount) const;
	const struct macho_nlist*  binarySearchWithToc(const char* key, const char stringPool[], const struct macho_nlist symbols[], 
												const struct dylib_table_of_contents toc[], uint32_t symbolCount, uint32_t hintIndex) const;
			
	const uint8_t*							fMachOData;
	const uint8_t*							fLinkEditBase; // add any internal "offset" to this to get actual address
	const struct macho_nlist*				fSymbolTable;
	const char*								fStrings;
	const struct dysymtab_command*			fDynamicInfo;
	uintptr_t								fSlide;
	const struct twolevel_hints_command*	fTwoLevelHints;
	const struct dylib_command*				fDylibID;
	class SegmentMachO*						fSegmentsArray;
#if TEXT_RELOC_SUPPORT
	class SegmentMachO*						fTextSegmentWithFixups; // NULL unless __TEXT segment has fixups
#endif
#if __i386__
	class SegmentMachO*						fReadOnlyImportSegment; // NULL unless __IMPORT segment built with -read_only_stubs
	static uint32_t							fgReadOnlyImportSpinLock;
#endif
	uint32_t								fSegmentsArrayCount : 8,
											fIsSplitSeg : 1,
											fInSharedCache : 1,
#if __ppc64__
											f4GBWritable : 1,
#endif
											fHasSubLibraries : 1,
											fHasSubUmbrella : 1,
											fInUmbrella : 1,
											fHasDOFSections : 1,
											fHasDashInit : 1,
											fHasInitializers : 1,
#if IMAGE_NOTIFY_SUPPORT
											fHasImageNotifySection : 1,
#endif
											fHasTerminators : 1;
	static uint32_t					fgHintedBinaryTreeSearchs;
	static uint32_t					fgUnhintedBinaryTreeSearchs;
	static uint32_t					fgCountOfImagesWithWeakExports;
};


class SegmentMachO : public Segment
{
public:	
								SegmentMachO(const struct macho_segment_command* cmd);
	virtual						~SegmentMachO();
								
	virtual const char*			getName();
	virtual uintptr_t			getSize();
	virtual uintptr_t			getFileSize();
	virtual uintptr_t			getFileOffset();
	virtual bool				readable();
	virtual bool				writeable();
	virtual bool				executable();
	virtual bool				unaccessible();
	virtual uintptr_t			getActualLoadAddress(const ImageLoader*);
	virtual uintptr_t			getPreferredLoadAddress();
	virtual void				unmap(const ImageLoader*);
	virtual Segment*			next(Segment*);
#if TEXT_RELOC_SUPPORT
	virtual bool				hasFixUps();
#endif
#if __i386__
	virtual bool				readOnlyImportStubs();
#endif
protected:
	virtual bool				hasPreferredLoadAddress();
	
private:
						SegmentMachO(const SegmentMachO&);
	void				operator=(const SegmentMachO&);
	void				adjust(const struct macho_segment_command* cmd);

	friend class ImageLoaderMachO;
	
	const struct macho_segment_command*		fSegmentLoadCommand;
};


#endif // __IMAGELOADERMACHO__




