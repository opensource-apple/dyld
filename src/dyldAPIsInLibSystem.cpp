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
#include <string.h>
#include <crt_externs.h>

#include "mach-o/dyld.h"
#include "mach-o/dyld_priv.h"

#include "dyldLock.h"

/*
 * names_match() takes an install_name from an LC_LOAD_DYLIB command and a
 * libraryName (which is -lx or -framework Foo argument passed to the static
 * link editor for the same library) and determines if they match.  This depends
 * on conventional use of names including major versioning.
 */
static
bool
names_match(
char *install_name,
const char* libraryName)
{
    char *basename;
    unsigned long n;

	/*
	 * Conventional install names have these forms:
	 *	/System/Library/Frameworks/AppKit.framework/Versions/A/Appkit
	 *	/Local/Library/Frameworks/AppKit.framework/Appkit
	 *	/lib/libsys_s.A.dylib
	 *	/usr/lib/libsys_s.dylib
	 */
	basename = strrchr(install_name, '/');
	if(basename == NULL)
	    basename = install_name;
	else
	    basename++;

	/*
	 * By checking the base name matching the library name we take care
	 * of the -framework cases.
	 */
	if(strcmp(basename, libraryName) == 0)
	    return(TRUE);

	/*
	 * Now check the base name for "lib" if so proceed to check for the
	 * -lx case dealing with a possible .X.dylib and a .dylib extension.
	 */
	if(strncmp(basename, "lib", 3) ==0){
	    n = strlen(libraryName);
	    if(strncmp(basename+3, libraryName, n) == 0){
		if(strncmp(basename+3+n, ".dylib", 6) == 0)
		    return(TRUE);
		if(basename[3+n] == '.' &&
		   basename[3+n+1] != '\0' &&
		   strncmp(basename+3+n+2, ".dylib", 6) == 0)
		    return(TRUE);
	    }
	}
	return(FALSE);
}

void NSInstallLinkEditErrorHandlers(
const NSLinkEditErrorHandlers* handlers)
{
	DYLD_WRITER_LOCK_THIS_BLOCK;
    static void (*p)(
	void     (*undefined)(const char* symbol_name),
	NSModule (*multiple)(NSSymbol s, NSModule old, NSModule newhandler),
	void     (*linkEdit)(NSLinkEditErrors c, int errorNumber,
		     const char* fileName, const char* errorString)) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_install_handlers", (void**)&p);
	p(handlers->undefined, handlers->multiple, handlers->linkEdit);
}

const char* 
NSNameOfModule(
NSModule module)
{
	DYLD_READER_LOCK_THIS_BLOCK;
    static const char*  (*p)(NSModule module) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSNameOfModule", (void**)&p);
	return(p(module));
} 

const char* 
NSLibraryNameForModule(
NSModule module)
{
	DYLD_READER_LOCK_THIS_BLOCK;
    static const char*  (*p)(NSModule module) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSLibraryNameForModule", (void**)&p);
	return(p(module));
}

bool
NSIsSymbolNameDefined(
const char* symbolName)
{
	DYLD_READER_LOCK_THIS_BLOCK;
    static bool (*p)(const char* symbolName) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSIsSymbolNameDefined", (void**)&p);
	return(p(symbolName));
}

bool
NSIsSymbolNameDefinedWithHint(
const char* symbolName,
const char* libraryNameHint)
{
	DYLD_READER_LOCK_THIS_BLOCK;
    static bool (*p)(const char* symbolName,
			  const char* libraryNameHint) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSIsSymbolNameDefinedWithHint", (void**)&p);
	return(p(symbolName, libraryNameHint));
}

bool
NSIsSymbolNameDefinedInImage(
const struct mach_header *image,
const char* symbolName)
{
	DYLD_READER_LOCK_THIS_BLOCK;
    static bool (*p)(const struct mach_header *image,
			  const char* symbolName) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSIsSymbolNameDefinedInImage", (void**)&p);
	return(p(image, symbolName));
}

NSSymbol
NSLookupAndBindSymbol(
const char* symbolName)
{
	DYLD_WRITER_LOCK_THIS_BLOCK;
    static NSSymbol (*p)(const char* symbolName) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSLookupAndBindSymbol", (void**)&p);
	return(p(symbolName));
}

NSSymbol
NSLookupAndBindSymbolWithHint(
const char* symbolName,
const char* libraryNameHint)
{
	DYLD_WRITER_LOCK_THIS_BLOCK;
    static NSSymbol (*p)(const char* symbolName,
			 const char* libraryNameHint) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSLookupAndBindSymbolWithHint", (void**)&p);
	return(p(symbolName, libraryNameHint));
}

NSSymbol
NSLookupSymbolInModule(
NSModule module,
const char* symbolName)
{
	DYLD_READER_LOCK_THIS_BLOCK;
    static NSSymbol (*p)(NSModule module, const char* symbolName) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSLookupSymbolInModule", (void**)&p);
	return(p(module, symbolName));
}

NSSymbol
NSLookupSymbolInImage(
const struct mach_header *image,
const char* symbolName,
uint32_t options)
{
 	DYLD_READER_LOCK_THIS_BLOCK;
   static NSSymbol (*p)(const struct mach_header *image,
			 const char* symbolName,
			 uint32_t options) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSLookupSymbolInImage", (void**)&p);
	return(p(image, symbolName, options));
}

const char* 
NSNameOfSymbol(
NSSymbol symbol)
{
	DYLD_READER_LOCK_THIS_BLOCK;
    static char * (*p)(NSSymbol symbol) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSNameOfSymbol",(void**)&p);
	return(p(symbol));
}

void *
NSAddressOfSymbol(
NSSymbol symbol)
{
	DYLD_READER_LOCK_THIS_BLOCK;
    static void * (*p)(NSSymbol symbol) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSAddressOfSymbol", (void**)&p);
	return(p(symbol));
}

NSModule
NSModuleForSymbol(
NSSymbol symbol)
{
	DYLD_READER_LOCK_THIS_BLOCK;
    static NSModule (*p)(NSSymbol symbol) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSModuleForSymbol", (void**)&p);
	return(p(symbol));
}

bool
NSAddLibrary(
const char* pathName)
{
	DYLD_WRITER_LOCK_THIS_BLOCK;
    static bool (*p)(const char* pathName) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSAddLibrary", (void**)&p);
	return(p(pathName));
}

bool
NSAddLibraryWithSearching(
const char* pathName)
{
	DYLD_WRITER_LOCK_THIS_BLOCK;
    static bool (*p)(const char* pathName) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSAddLibraryWithSearching", (void**)&p);
	return(p(pathName));
}

const struct mach_header *
NSAddImage(
const char* image_name,
uint32_t options)
{
	DYLD_WRITER_LOCK_THIS_BLOCK;
    static const struct mach_header * (*p)(const char* image_name,
					   uint32_t options) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSAddImage", (void**)&p);
	return(p(image_name, options));
}

/*
 * This routine returns the current version of the named shared library the
 * executable it was built with.  The libraryName parameter is the same as the
 * -lx or -framework Foo argument passed to the static link editor when building
 * the executable (with -lx it would be "x" and with -framework Foo it would be
 * "Foo").  If this the executable was not built against the specified library
 * it returns -1.  It should be noted that if this only returns the value the
 * current version of the named shared library the executable was built with
 * and not a list of current versions that dependent libraries and bundles the
 * program is using were built with.
 */
int32_t
NSVersionOfLinkTimeLibrary(
const char* libraryName)
{
    unsigned long i;
    struct load_command *load_commands, *lc;
    struct dylib_command *dl;
    char *install_name;
#ifndef __OPENSTEP__
    static struct mach_header *mh = NULL;
	if(mh == NULL)
	    mh = _NSGetMachExecuteHeader();
#else /* defined(__OPENSTEP__) */
#ifdef __DYNAMIC__
    static struct mach_header *mh = NULL;
	if(mh == NULL)
	    _dyld_lookup_and_bind("__mh_execute_header", &mh, NULL);
#else
    struct mach_header *mh;
	mh = (struct mach_header *)&_mh_execute_header;
#endif
#endif /* __OPENSTEP__ */
	load_commands = (struct load_command *)
			((char *)mh + sizeof(struct mach_header));
	lc = load_commands;
	for(i = 0; i < mh->ncmds; i++){
	    if(lc->cmd == LC_LOAD_DYLIB){
		dl = (struct dylib_command *)lc;
		install_name = (char *)dl + dl->dylib.name.offset;
		if(names_match(install_name, libraryName) == TRUE)
		    return(dl->dylib.current_version);
	    }
	    lc = (struct load_command *)((char *)lc + lc->cmdsize);
	}
	return(-1);
}

/*
 * This routine returns the current version of the named shared library the
 * program it is running against.  The libraryName parameter is the same as
 * would be static link editor using the -lx or -framework Foo flags (with -lx
 * it would be "x" and with -framework Foo it would be "Foo").  If the program
 * is not using the specified library it returns -1.
 */
int32_t
NSVersionOfRunTimeLibrary(
const char* libraryName)
{
    unsigned long i, j, n;
    char *install_name;
    struct load_command *load_commands, *lc;
    struct dylib_command *dl;
    const struct mach_header *mh;

	n = _dyld_image_count();
	for(i = 0; i < n; i++){
	    mh = _dyld_get_image_header(i);
	    if(mh->filetype != MH_DYLIB)
		continue;
	    load_commands = (struct load_command *)
			    ((char *)mh + sizeof(struct mach_header));
	    lc = load_commands;
	    for(j = 0; j < mh->ncmds; j++){
		if(lc->cmd == LC_ID_DYLIB){
		    dl = (struct dylib_command *)lc;
		    install_name = (char *)dl + dl->dylib.name.offset;
		    if(names_match(install_name, libraryName) == TRUE)
			return(dl->dylib.current_version);
		}
		lc = (struct load_command *)((char *)lc + lc->cmdsize);
	    }
	}
	return(-1);
}

/*
 * NSCreateObjectFileImageFromFile() creates an NSObjectFileImage for the
 * specified file name if the file is a correct Mach-O file that can be loaded
 * with NSloadModule().  For return codes of NSObjectFileImageFailure and
 * NSObjectFileImageFormat an error message is printed to stderr.  All
 * other codes cause no printing. 
 */
NSObjectFileImageReturnCode
NSCreateObjectFileImageFromFile(
const char* pathName,
NSObjectFileImage *objectFileImage)
{
	DYLD_WRITER_LOCK_THIS_BLOCK;
    static NSObjectFileImageReturnCode (*p)(const char*, NSObjectFileImage*) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSCreateObjectFileImageFromFile", (void**)&p);
	return p(pathName, objectFileImage);
}


/*
 * NSCreateObjectFileImageFromMemory() creates an NSObjectFileImage for the
 * object file mapped into memory at address of size length if the object file
 * is a correct Mach-O file that can be loaded with NSloadModule().  For return
 * codes of NSObjectFileImageFailure and NSObjectFileImageFormat an error
 * message is printed to stderr.  All other codes cause no printing. 
 */
NSObjectFileImageReturnCode
NSCreateObjectFileImageFromMemory(
const void* address,
size_t size, 
NSObjectFileImage *objectFileImage)
{
	DYLD_WRITER_LOCK_THIS_BLOCK;
    static NSObjectFileImageReturnCode (*p)(const void*, size_t, NSObjectFileImage*) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSCreateObjectFileImageFromMemory", (void**)&p);
	return p(address, size, objectFileImage);
}

/*
 * NSCreateCoreFileImageFromFile() creates an NSObjectFileImage for the 
 * specified core file name if the file is a correct Mach-O core file.
 * For return codes of NSObjectFileImageFailure and NSObjectFileImageFormat
 * an error message is printed to stderr.  All other codes cause no printing. 
 */
NSObjectFileImageReturnCode
NSCreateCoreFileImageFromFile(
const char* pathName,
NSObjectFileImage *objectFileImage)
{
	DYLD_WRITER_LOCK_THIS_BLOCK;
    static NSObjectFileImageReturnCode (*p)(const char*, NSObjectFileImage*) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSCreateCoreFileImageFromFile", (void**)&p);
	return p(pathName, objectFileImage);
}

bool
NSDestroyObjectFileImage(
NSObjectFileImage objectFileImage)
{
	DYLD_WRITER_LOCK_THIS_BLOCK;
    static bool (*p)(NSObjectFileImage) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSDestroyObjectFileImage", (void**)&p);
	return p(objectFileImage);
}


NSModule
NSLinkModule(
NSObjectFileImage objectFileImage, 
const char* moduleName,
uint32_t options)
{
	DYLD_WRITER_LOCK_THIS_BLOCK;
    static NSModule (*p)(NSObjectFileImage, const char*, unsigned long) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSLinkModule", (void**)&p);
		
	return p(objectFileImage, moduleName, options);
}


/*
 * NSFindSectionAndOffsetInObjectFileImage() takes the specified imageOffset
 * into the specified ObjectFileImage and returns the segment/section name and
 * offset into that section of that imageOffset.  Returns FALSE if the
 * imageOffset is not in any section.  You can used the resulting sectionOffset
 * to index into the data returned by NSGetSectionDataInObjectFileImage.
 * 
 * SPI: currently only used by ZeroLink to detect +load methods
 */
bool 
NSFindSectionAndOffsetInObjectFileImage(
NSObjectFileImage objectFileImage, 
unsigned long imageOffset,
const char** segmentName, 	/* can be NULL */
const char** sectionName, 	/* can be NULL */
unsigned long* sectionOffset)	/* can be NULL */
{
	DYLD_READER_LOCK_THIS_BLOCK;
    static bool (*p)(NSObjectFileImage, unsigned long, const char**, const char**, unsigned long*) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSFindSectionAndOffsetInObjectFileImage", (void**)&p);
		
	return p(objectFileImage, imageOffset, segmentName, sectionName, sectionOffset);
}


/*
 * NSSymbolDefinitionCountInObjectFileImage() returns the number of symbol
 * definitions in the NSObjectFileImage.
 */
uint32_t
NSSymbolDefinitionCountInObjectFileImage(
NSObjectFileImage objectFileImage)
{
	DYLD_READER_LOCK_THIS_BLOCK;
    static unsigned long (*p)(NSObjectFileImage) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSSymbolDefinitionCountInObjectFileImage", (void**)&p);
		
	return p(objectFileImage);
}

/*
 * NSSymbolDefinitionNameInObjectFileImage() returns the name of the i'th
 * symbol definitions in the NSObjectFileImage.  If the ordinal specified is
 * outside the range [0..NSSymbolDefinitionCountInObjectFileImage], NULL will
 * be returned.
 */
const char* 
NSSymbolDefinitionNameInObjectFileImage(
NSObjectFileImage objectFileImage,
uint32_t ordinal)
{
	DYLD_READER_LOCK_THIS_BLOCK;
    static const char*  (*p)(NSObjectFileImage, uint32_t) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSSymbolDefinitionNameInObjectFileImage", (void**)&p);
		
	return p(objectFileImage, ordinal);
}

/*
 * NSSymbolReferenceCountInObjectFileImage() returns the number of references
 * to undefined symbols the NSObjectFileImage.
 */
uint32_t
NSSymbolReferenceCountInObjectFileImage(
NSObjectFileImage objectFileImage)
{
	DYLD_READER_LOCK_THIS_BLOCK;
    static unsigned long (*p)(NSObjectFileImage) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSSymbolReferenceCountInObjectFileImage", (void**)&p);
		
	return p(objectFileImage);
}

/*
 * NSSymbolReferenceNameInObjectFileImage() returns the name of the i'th
 * undefined symbol in the NSObjectFileImage. If the ordinal specified is
 * outside the range [0..NSSymbolReferenceCountInObjectFileImage], NULL will be
 * returned.
 */
const char* 
NSSymbolReferenceNameInObjectFileImage(
NSObjectFileImage objectFileImage,
uint32_t ordinal,
bool *tentative_definition) /* can be NULL */
{
	DYLD_READER_LOCK_THIS_BLOCK;
    static const char*  (*p)(NSObjectFileImage, uint32_t, bool*) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSSymbolReferenceNameInObjectFileImage", (void**)&p);
		
	return p(objectFileImage, ordinal, tentative_definition);
}

/*
 * NSIsSymbolDefinedInObjectFileImage() returns TRUE if the specified symbol
 * name has a definition in the NSObjectFileImage and FALSE otherwise.
 */
bool
NSIsSymbolDefinedInObjectFileImage(
NSObjectFileImage objectFileImage,
const char* symbolName)
{
	DYLD_READER_LOCK_THIS_BLOCK;
    static bool (*p)(NSObjectFileImage, const char*) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSIsSymbolDefinedInObjectFileImage", (void**)&p);
		
	return p(objectFileImage, symbolName);
}

/*
 * NSGetSectionDataInObjectFileImage() returns a pointer to the section contents
 * in the NSObjectFileImage for the specified segmentName and sectionName if
 * it exists and it is not a zerofill section.  If not it returns NULL.  If
 * the parameter size is not NULL the size of the section is also returned
 * indirectly through that pointer.
 */
void *
NSGetSectionDataInObjectFileImage(
NSObjectFileImage objectFileImage,
const char* segmentName,
const char* sectionName,
unsigned long *size) /* can be NULL */
{
	DYLD_READER_LOCK_THIS_BLOCK;
    static void* (*p)(NSObjectFileImage, const char*, const char*, unsigned long*) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSGetSectionDataInObjectFileImage", (void**)&p);
		
	return p(objectFileImage, segmentName, sectionName, size);
}

/*
 * NSHasModInitObjectFileImage() returns TRUE if the NSObjectFileImage has any
 * module initialization sections and FALSE it it does not.
 *
 * SPI: currently only used by ZeroLink to detect C++ initializers
 */
bool
NSHasModInitObjectFileImage(
NSObjectFileImage objectFileImage)
{
	DYLD_READER_LOCK_THIS_BLOCK;
    static bool (*p)(NSObjectFileImage) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_NSHasModInitObjectFileImage", (void**)&p);
		
	return p(objectFileImage);
}

void
NSLinkEditError(
NSLinkEditErrors *c,
int *errorNumber, 
const char* *fileName,
const char* *errorString)
{
	DYLD_WRITER_LOCK_THIS_BLOCK;
    static void (*p)(NSLinkEditErrors *c,
		     int *errorNumber, 
		     const char* *fileName,
		     const char* *errorString) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_link_edit_error", (void**)&p);
	if(p != NULL)
	    p(c, errorNumber, fileName, errorString);
}

bool
NSUnLinkModule(
NSModule module, 
uint32_t options)
{
	DYLD_WRITER_LOCK_THIS_BLOCK;
    static bool (*p)(NSModule module, uint32_t options) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_unlink_module", (void**)&p);

	return p(module, options);
}

NSModule
NSReplaceModule(
NSModule moduleToReplace,
NSObjectFileImage newObjectFileImage, 
uint32_t options)
{
	return(NULL);
}

/*
 *_NSGetExecutablePath copies the path of the executable into the buffer and
 * returns 0 if the path was successfully copied in the provided buffer. If the
 * buffer is not large enough, -1 is returned and the expected buffer size is
 * copied in *bufsize. Note that _NSGetExecutablePath will return "a path" to
 * the executable not a "real path" to the executable. That is the path may be
 * a symbolic link and not the real file. And with deep directories the total
 * bufsize needed could be more than MAXPATHLEN.
 */
int
_NSGetExecutablePath(
char *buf,
uint32_t *bufsize)
{
	DYLD_READER_LOCK_THIS_BLOCK;
    static int (*p)(char *buf, uint32_t *bufsize) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld__NSGetExecutablePath", (void**)&p);
	return(p(buf, bufsize));
}

void
_dyld_lookup_and_bind(
const char* symbol_name,
void** address,
NSModule* module)
{
	DYLD_WRITER_LOCK_THIS_BLOCK;
    static void (*p)(const char*, void** , NSModule*) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_lookup_and_bind", (void**)&p);
	p(symbol_name, address, module);
}

void
_dyld_lookup_and_bind_with_hint(
const char* symbol_name,
const char* library_name_hint,
void** address,
NSModule* module)
{
	DYLD_WRITER_LOCK_THIS_BLOCK;
    static void (*p)(const char*, const char*, void**, NSModule*) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_lookup_and_bind_with_hint", (void**)&p);
	p(symbol_name, library_name_hint, address, module);
}

void
_dyld_lookup_and_bind_objc(
const char* symbol_name,
void** address,
NSModule* module)
{
	DYLD_WRITER_LOCK_THIS_BLOCK;
    static void (*p)(const char* , void**, NSModule*) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_lookup_and_bind_objc", (void**)&p);
	p(symbol_name, address, module);
}

void
_dyld_lookup_and_bind_fully(
const char* symbol_name,
void** address,
NSModule* module)
{
	DYLD_WRITER_LOCK_THIS_BLOCK;
    static void (*p)(const char*, void**, NSModule*) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_lookup_and_bind_fully", (void**)&p);
	p(symbol_name, address, module);
}

bool
_dyld_bind_fully_image_containing_address(
const void* address)
{
	DYLD_WRITER_LOCK_THIS_BLOCK;
    static bool (*p)(const void*) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_bind_fully_image_containing_address", (void**)&p);
	return p(address);
}


/*
 * _dyld_register_func_for_add_image registers the specified function to be
 * called when a new image is added (a bundle or a dynamic shared library) to
 * the program.  When this function is first registered it is called for once
 * for each image that is currently part of the program.
 */
void
_dyld_register_func_for_add_image(
void (*func)(const struct mach_header *mh, intptr_t vmaddr_slide))
{
	DYLD_WRITER_LOCK_THIS_BLOCK;
    static void (*p)(void (*func)(const struct mach_header *mh, intptr_t vmaddr_slide)) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_register_func_for_add_image", (void**)&p);
	p(func);
}

/*
 * _dyld_register_func_for_remove_image registers the specified function to be
 * called when an image is removed (a bundle or a dynamic shared library) from
 * the program.
 */
void
_dyld_register_func_for_remove_image(
void (*func)(const struct mach_header *mh, intptr_t vmaddr_slide))
{
	DYLD_WRITER_LOCK_THIS_BLOCK;
    static void (*p)(void (*func)(const struct mach_header *mh, intptr_t vmaddr_slide)) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_register_func_for_remove_image", (void**)&p);
	p(func);
}

/*
 * _dyld_register_func_for_link_module registers the specified function to be
 * called when a module is bound into the program.  When this function is first
 * registered it is called for once for each module that is currently bound into
 * the program.
 */
void
_dyld_register_func_for_link_module(
void (*func)(NSModule module))
{
	DYLD_WRITER_LOCK_THIS_BLOCK;
    static void (*p)(void (*func)(NSModule module)) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_register_func_for_link_module", (void**)&p);
	p(func);
}

/*
 * _dyld_register_func_for_unlink_module registers the specified function to be
 * called when a module is unbound from the program.
 */
void
_dyld_register_func_for_unlink_module(
void (*func)(NSModule module))
{
	DYLD_WRITER_LOCK_THIS_BLOCK;
    static void (*p)(void (*func)(NSModule module)) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_register_func_for_unlink_module", (void**)&p);
	p(func);
}

/*
 * _dyld_register_func_for_replace_module registers the specified function to be
 * called when a module is to be replace with another module in the program.
 */
void
_dyld_register_func_for_replace_module(
void (*func)(NSModule oldmodule, NSModule newmodule))
{
	DYLD_WRITER_LOCK_THIS_BLOCK;
    static void (*p)(void (*func)(NSModule oldmodule,
				  NSModule newmodule)) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_register_func_for_replace_module", (void**)&p);
	p(func);
}


/*
 * _dyld_get_objc_module_sect_for_module is passed a module and sets a 
 * pointer to the (__OBJC,__module) section and its size for the specified
 * module.
 */
void
_dyld_get_objc_module_sect_for_module(
NSModule module,
void **objc_module,
unsigned long *size)
{
	DYLD_READER_LOCK_THIS_BLOCK;
    static void (*p)(NSModule module,
		     void **objc_module,
		     unsigned long *size) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_get_objc_module_sect_for_module", (void**)&p);
	p(module, objc_module, size);
}

/*
 * _dyld_bind_objc_module() is passed a pointer to something in an (__OBJC,
 * __module) section and causes the module that is associated with that address
 * to be bound.
 */
void
_dyld_bind_objc_module(
const void* objc_module)
{
	DYLD_READER_LOCK_THIS_BLOCK;
    static void (*p)(const void *objc_module) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_bind_objc_module", (void**)&p);
	p(objc_module);
}


#if __DYNAMIC__
bool
_dyld_present(
void)
{
	// hmmm, this code is in libSystem.dylib, which is loaded by dyld...
	return true;
}
#endif

uint32_t
_dyld_image_count(
void)
{
	DYLD_NO_LOCK_THIS_BLOCK;
    static unsigned long (*p)(void) = NULL;

	if(_dyld_present() == 0)
	    return(0);
	if(p == NULL)
	    _dyld_func_lookup("__dyld_image_count", (void**)&p);
	return(p());
}

const struct mach_header *
_dyld_get_image_header(
uint32_t image_index)
{
	DYLD_NO_LOCK_THIS_BLOCK;
    static struct mach_header * (*p)(uint32_t image_index) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_get_image_header", (void**)&p);
	return(p(image_index));
}

intptr_t
_dyld_get_image_vmaddr_slide(
uint32_t image_index)
{
	DYLD_NO_LOCK_THIS_BLOCK;
    static unsigned long (*p)(uint32_t image_index) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_get_image_vmaddr_slide", (void**)&p);
	return(p(image_index));
}

const char* 
_dyld_get_image_name(
uint32_t image_index)
{
	DYLD_NO_LOCK_THIS_BLOCK;
    static const char*  (*p)(uint32_t image_index) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_get_image_name", (void**)&p);
	return(p(image_index));
}

bool
_dyld_image_containing_address(
const void* address)
{
	DYLD_READER_LOCK_THIS_BLOCK;
    static bool (*p)(const void*) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_image_containing_address", (void**)&p);
	return(p(address));
}

const struct mach_header *
_dyld_get_image_header_containing_address(
const void* address)
{
	DYLD_READER_LOCK_THIS_BLOCK;
    static const struct mach_header * (*p)(const void*) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_get_image_header_containing_address", (void**)&p);
	return p(address);
}

void _dyld_moninit(
void (*monaddition)(char *lowpc, char *highpc))
{
	DYLD_READER_LOCK_THIS_BLOCK;
    static void (*p)(void (*monaddition)(char *lowpc, char *highpc)) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_moninit", (void**)&p);
	p(monaddition);
}

bool _dyld_launched_prebound(
void)
{
	DYLD_READER_LOCK_THIS_BLOCK;
    static bool (*p)(void) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_launched_prebound", (void**)&p);
	return(p());
}

bool _dyld_all_twolevel_modules_prebound(
void)
{
	DYLD_READER_LOCK_THIS_BLOCK;
    static bool (*p)(void) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_all_twolevel_modules_prebound", (void**)&p);
	return(p());
}


#include <dlfcn.h>
#include <stddef.h>
#include <pthread.h>
#include <stdlib.h>
#include <mach-o/dyld.h>
#include "dyldLock.h"
#include "dyldLibSystemThreadHelpers.h"

// pthread key used to access per-thread dlerror message
static pthread_key_t dlerrorPerThreadKey;

// data kept per-thread
struct dlerrorPerThreadData
{
	uint32_t	sizeAllocated;
	char		message[1];
};

// function called by dyld to get buffer to store dlerror message
static char* getPerThreadBufferFor_dlerror(uint32_t sizeRequired)
{
	const int size = (sizeRequired < 256) ? 256 : sizeRequired;
	dlerrorPerThreadData* data = (dlerrorPerThreadData*)pthread_getspecific(dlerrorPerThreadKey);
	if ( data == NULL ) {
		//int mallocSize = offsetof(dlerrorPerThreadData, message[size]);
		const int mallocSize = sizeof(dlerrorPerThreadData)+size;
		data = (dlerrorPerThreadData*)malloc(mallocSize);
		data->sizeAllocated = size;
		pthread_setspecific(dlerrorPerThreadKey, data);
	}
	else if ( data->sizeAllocated < sizeRequired ) {
		free(data);
		//int mallocSize = offsetof(dlerrorPerThreadData, message[size]);
		const int mallocSize = sizeof(dlerrorPerThreadData)+size;
		data = (dlerrorPerThreadData*)malloc(mallocSize);
		data->sizeAllocated = size;
		pthread_setspecific(dlerrorPerThreadKey, data);
	}
	return data->message;
}

// that table passed to dyld containing thread helpers
static dyld::ThreadingHelpers sThreadHelpers = { 1, &lockForLazyBinding, &unlockForLazyBinding, &getPerThreadBufferFor_dlerror };

//
// during initialization of libSystem this routine will run
// and call dyld, registering the threading helpers.
//
//
static int registerWithDyld()
{
    static void (*p)(dyld::ThreadingHelpers*) = NULL;

	// create key and tell pthread package to call free() on any data associated with key if thread dies
	pthread_key_create(&dlerrorPerThreadKey, &free);
	
	if(p == NULL) 
	    _dyld_func_lookup("__dyld_register_thread_helpers", (void**)&p);
	if(p != NULL)
		p(&sThreadHelpers);
	
	return 0;
}

// should be able to use __attribute__((constructor)) on registerWithDyld, but compiler has bug (3679135)
// instead use initialization of global to force it to run.
static int hack = registerWithDyld();


char* dlerror()
{
	DYLD_READER_LOCK_THIS_BLOCK;
    static char* (*p)() = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_dlerror", (void**)&p);
	return(p());
}

int dladdr(const void* addr, Dl_info* info)
{
	DYLD_READER_LOCK_THIS_BLOCK;
    static int (*p)(const void* , Dl_info*) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_dladdr", (void**)&p);
	return(p(addr, info));
}

int dlclose(void* handle)
{
	DYLD_WRITER_LOCK_THIS_BLOCK;
    static int (*p)(void* handle) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_dlclose", (void**)&p);
	return(p(handle));
}

void* dlopen(const char* path, int mode)
{
	DYLD_WRITER_LOCK_THIS_BLOCK;
    static void* (*p)(const char* path, int) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_dlopen", (void**)&p);
	return(p(path, mode));
}

void* dlsym(void* handle, const char* symbol)
{
	DYLD_READER_LOCK_THIS_BLOCK;
    static void* (*p)(void* handle, const char* symbol) = NULL;

	if(p == NULL)
	    _dyld_func_lookup("__dyld_dlsym", (void**)&p);
	return(p(handle, symbol));
}





