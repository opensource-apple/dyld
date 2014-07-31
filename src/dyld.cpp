/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2004-2006 Apple Computer, Inc. All rights reserved.
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

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/param.h>
#include <mach/mach_time.h> // mach_absolute_time()
#include <sys/types.h>
#include <sys/stat.h> 
#include <mach-o/fat.h> 
#include <mach-o/loader.h> 
#include <libkern/OSByteOrder.h> 
#include <mach/mach.h>
#include <sys/sysctl.h>

#include <vector>

#include "mach-o/dyld_gdb.h"

#include "dyld.h"
#include "ImageLoader.h"
#include "ImageLoaderMachO.h"
#include "dyldLibSystemThreadHelpers.h"


#define CPU_TYPE_MASK 0x00FFFFFF	/* complement of CPU_ARCH_MASK */


/* implemented in dyld_gdb.cpp */
void addImagesToAllImages(uint32_t infoCount, const dyld_image_info info[]);
void removeImageFromAllImages(const mach_header* mh);
#if OLD_GDB_DYLD_INTERFACE
void addImageForgdb(const mach_header* mh, uintptr_t slide, const char* physicalPath, const char* logicalPath);
void removeImageForgdb(const struct mach_header* mh);
#endif

// magic so CrashReporter logs message
extern "C" {
	char error_string[1024];
}


//
// The file contains the core of dyld used to get a process to main().  
// The API's that dyld supports are implemented in dyldAPIs.cpp.
//
//
//
//
//


namespace dyld {


// 
// state of all environment variables dyld uses
//
struct EnvironmentVariables {
	const char* const *			DYLD_FRAMEWORK_PATH;
	const char* const *			DYLD_FALLBACK_FRAMEWORK_PATH;
	const char* const *			DYLD_LIBRARY_PATH;
	const char* const *			DYLD_FALLBACK_LIBRARY_PATH;
	const char*	const *			DYLD_ROOT_PATH;
	const char* const *			DYLD_INSERT_LIBRARIES;
	const char* const *			LD_LIBRARY_PATH;			// for unix conformance
	bool						DYLD_PRINT_LIBRARIES;
	bool						DYLD_PRINT_LIBRARIES_POST_LAUNCH;
	bool						DYLD_BIND_AT_LAUNCH;
	bool						DYLD_PRINT_STATISTICS;
	bool						DYLD_PRINT_OPTS;
	bool						DYLD_PRINT_ENV;
							//	DYLD_IMAGE_SUFFIX				==> gLinkContext.imageSuffix
							//	DYLD_PRINT_OPTS					==> gLinkContext.verboseOpts
							//	DYLD_PRINT_ENV					==> gLinkContext.verboseEnv
							//	DYLD_FORCE_FLAT_NAMESPACE		==> gLinkContext.bindFlat
							//	DYLD_PRINT_INITIALIZERS			==> gLinkContext.verboseInit
							//	DYLD_PRINT_SEGMENTS				==> gLinkContext.verboseMapping
							//	DYLD_PRINT_BINDINGS				==> gLinkContext.verboseBind
							//	DYLD_PRINT_REBASINGS			==> gLinkContext.verboseRebase
							//	DYLD_PRINT_APIS					==> gLogAPIs
							//	DYLD_IGNORE_PREBINDING			==> gLinkContext.prebindUsage
							//	DYLD_PREBIND_DEBUG				==> gLinkContext.verbosePrebinding
							//	DYLD_NEW_LOCAL_SHARED_REGIONS	==> gLinkContext.sharedRegionMode
							//	DYLD_SHARED_REGION				==> gLinkContext.sharedRegionMode
							//	DYLD_SLIDE_AND_PACK_DYLIBS		==> gLinkContext.slideAndPackDylibs
							//	DYLD_PRINT_WARNINGS				==> gLinkContext.verboseWarnings
};

// all global state
static const char*					sExecPath = NULL;
static const struct mach_header*	sMainExecutableMachHeader = NULL;
static cpu_type_t					sHostCPU;
static cpu_subtype_t				sHostCPUsubtype;
static ImageLoader*					sMainExecutable = NULL;
static bool							sAllImagesMightContainUnlinkedImages;	 // necessary until will support dylib unloading
static std::vector<ImageLoader*>	sAllImages;
static std::vector<ImageLoader*>	sImageRoots;
static std::vector<ImageLoader*>	sImageFilesNeedingTermination;
static std::vector<ImageLoader*>	sImagesToNotifyAboutOtherImages;
static std::vector<ImageCallback>   sAddImageCallbacks;
static std::vector<ImageCallback>   sRemoveImageCallbacks;
static ImageLoader*					sLastImageByAddressCache;
static EnvironmentVariables			sEnv;
static const char*					sFrameworkFallbackPaths[] = { "$HOME/Library/Frameworks", "/Library/Frameworks", "/Network/Library/Frameworks", "/System/Library/Frameworks", NULL };
static const char*					sLibraryFallbackPaths[] = { "$HOME/lib", "/usr/local/lib", "/usr/lib", NULL };
static BundleNotificationCallBack   sBundleNotifier = NULL;
static BundleLocatorCallBack		sBundleLocation = NULL;
static UndefinedHandler				sUndefinedHandler = NULL;
ImageLoader::LinkContext			gLinkContext;
bool								gLogAPIs = false;
const struct ThreadingHelpers*		gThreadHelpers = NULL;



// utility class to assure files are closed when an exception is thrown
class FileOpener {
public:
	FileOpener(const char* path);
	~FileOpener();
	int getFileDescriptor() { return fd; }
private:
	int fd;
};

FileOpener::FileOpener(const char* path)
{
	fd = open(path, O_RDONLY, 0);
}

FileOpener::~FileOpener()
{
	close(fd);
}



// Objective-C installs an addImage hook to dyld to get notified about new images
// The callback needs to be run after the image is rebased and bound, but before its initializers are called
static uint32_t imageNotification(ImageLoader* image, uint32_t startIndex)
{
	// tell all register add image handlers about this
	const uint32_t callbackCount = sAddImageCallbacks.size();
	for (uint32_t i=startIndex; i < callbackCount; ++i) {
		ImageCallback cb = sAddImageCallbacks[i];
		//fprintf(stderr, "dyld: calling add-image-callback[%d]=%p for %s\n", i, cb, image->getPath());
		(cb)(image->machHeader(), image->getSlide());
	}
	return callbackCount;
}



// notify gdb et al about these new images
static void notifyAdding(std::vector<ImageLoader*>& images)
{
	// build array
	unsigned int len = images.size();
	if ( len != 0 ) {
		dyld_image_info	infos[len];
		for (unsigned int i=0; i < len; ++i) {
			dyld_image_info* p = &infos[i];
			ImageLoader* image = images[i];
			p->imageLoadAddress = image->machHeader();
			p->imageFilePath = image->getPath();
			p->imageFileModDate = image->lastModified();
			//fprintf(stderr, "notifying objc about %s\n", image->getPath());
		}
		
		// tell gdb
		addImagesToAllImages(len, infos);
				
		// tell all interested images (after gdb, so you can debug anything the notification does)
		for (std::vector<ImageLoader*>::iterator it=sImagesToNotifyAboutOtherImages.begin(); it != sImagesToNotifyAboutOtherImages.end(); it++) {
			(*it)->doNotification(dyld_image_adding, len, infos);
		}
	}
}



// In order for register_func_for_add_image() callbacks to to be called bottom up,
// we need to maintain a list of root images. The main executable is usally the
// first root. Any images dynamically added are also roots (unless already loaded).
// If DYLD_INSERT_LIBRARIES is used, those libraries are first.
static void addRootImage(ImageLoader* image)
{
	//fprintf(stderr, "addRootImage(%p, %s)\n", image, image->getPath());
	// add to list of roots
	sImageRoots.push_back(image);
}

// Objective-C will contain a __DATA/__image_notify section which contains pointers to a function to call
// whenever any new image is loaded.
static void addImageNeedingNotification(ImageLoader* image)
{
	sImagesToNotifyAboutOtherImages.push_back(image);
}

static void addImage(ImageLoader* image)
{
	// add to master list
	sAllImages.push_back(image);
	
	if ( sEnv.DYLD_PRINT_LIBRARIES || (sEnv.DYLD_PRINT_LIBRARIES_POST_LAUNCH && (sMainExecutable!=NULL) && sMainExecutable->isLinked()) ) {
		uint64_t offset = image->getOffsetInFatFile();
		if ( offset == 0 )
			fprintf(stderr, "dyld: loaded: %s\n", image->getPath());
		else
			fprintf(stderr, "dyld: loaded: %s, cpu-sub-type: %d\n", image->getPath(), image->machHeader()->cpusubtype);
	}
	
#if OLD_GDB_DYLD_INTERFACE
	// let gdb find out about this
	addImageForgdb(image->machHeader(), image->getSlide(), image->getPath(), image->getLogicalPath());
#endif
}

void removeImage(ImageLoader* image)
{
	// if in termination list, pull it out and run terminator
	for (std::vector<ImageLoader*>::iterator it=sImageFilesNeedingTermination.begin(); it != sImageFilesNeedingTermination.end(); it++) {
		if ( *it == image ) {
			sImageFilesNeedingTermination.erase(it);
			image->doTermination(gLinkContext);
			break;
		}
	}
	
	// tell all register add image handlers about this
	// do this before removing image from internal data structures so that the callback can querey dyld about the image
	for (std::vector<ImageCallback>::iterator it=sRemoveImageCallbacks.begin(); it != sRemoveImageCallbacks.end(); it++) {
		(*it)(image->machHeader(), image->getSlide());
	}
	
	// tell all interested images
	for (std::vector<ImageLoader*>::iterator it=sImagesToNotifyAboutOtherImages.begin(); it != sImagesToNotifyAboutOtherImages.end(); it++) {
		dyld_image_info info;
		info.imageLoadAddress = image->machHeader();
		info.imageFilePath = image->getPath();
		info.imageFileModDate = image->lastModified();
		(*it)->doNotification(dyld_image_removing, 1, &info);
	}

	// remove from master list
	for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); it++) {
		if ( *it == image ) {
			sAllImages.erase(it);
			break;
		}
	}
	
	// flush find-by-address cache
	if ( sLastImageByAddressCache == image )
		sLastImageByAddressCache = NULL;

	// if in announcement list, pull it out 
	for (std::vector<ImageLoader*>::iterator it=sImagesToNotifyAboutOtherImages.begin(); it != sImagesToNotifyAboutOtherImages.end(); it++) {
		if ( *it == image ) {
			sImagesToNotifyAboutOtherImages.erase(it);
			break;
		}
	}

	// if in root list, pull it out 
	for (std::vector<ImageLoader*>::iterator it=sImageRoots.begin(); it != sImageRoots.end(); it++) {
		if ( *it == image ) {
			sImageRoots.erase(it);
			break;
		}
	}

	// tell gdb, new way
	removeImageFromAllImages(image->machHeader());

#if OLD_GDB_DYLD_INTERFACE
	// tell gdb, old way
	removeImageForgdb(image->machHeader());
	gdb_dyld_state_changed();
#endif
}


static void terminationRecorder(ImageLoader* image)
{
	sImageFilesNeedingTermination.push_back(image);
}

const char* getExecutablePath()
{
	return sExecPath;
}


void initializeMainExecutable()
{
	const int rootCount = sImageRoots.size();
	for(int i=0; i < rootCount; ++i) {
		ImageLoader* image = sImageRoots[i];
		//fprintf(stderr, "initializeMainExecutable: image = %p\n", image);
		image->runInitializers(gLinkContext);
	}
/*
	// this does not work???
	for (std::vector<ImageLoader*>::iterator it=sImageRoots.begin(); it != sImageRoots.end(); it++) {
		ImageLoader* image = *it;
		fprintf(stderr, "initializeMainExecutable: image = %p\n", image);
		// don't know why vector sometimes starts with NULL element???
		if ( image != NULL ) 
			image->runInitializers(gLinkContext);
	}
*/
	if ( sEnv.DYLD_PRINT_STATISTICS )
		ImageLoaderMachO::printStatistics(sAllImages.size());
}

bool mainExecutablePrebound()
{
	return sMainExecutable->usablePrebinding(gLinkContext);
}

ImageLoader* mainExecutable()
{
	return sMainExecutable;
}


void runTerminators()
{
	const unsigned int imageCount = sImageFilesNeedingTermination.size();
	for(unsigned int i=imageCount; i > 0; --i){
		ImageLoader* image = sImageFilesNeedingTermination[i-1];
		image->doTermination(gLinkContext);
	}
	sImageFilesNeedingTermination.clear();
}


//
// Turns a colon separated list of strings
// into a NULL terminated array of string 
// pointers.
//
static const char** parseColonList(const char* list)
{
	if ( list[0] == '\0' )
		return NULL;

	int colonCount = 0;
	for(const char* s=list; *s != '\0'; ++s) {
		if (*s == ':') 
			++colonCount;
	}
	
	int index = 0;
	const char* start = list;
	char** result = new char*[colonCount+2];
	for(const char* s=list; *s != '\0'; ++s) {
		if (*s == ':') {
			int len = s-start;
			char* str = new char[len+1];
			strncpy(str, start, len);
			str[len] = '\0';
			start = &s[1];
			result[index++] = str;
		}
	}
	int len = strlen(start);
	char* str = new char[len+1];
	strcpy(str, start);
	result[index++] = str;
	result[index] = NULL;
	
	return (const char**)result;
}

static void paths_expand_roots(const char **paths, const char *key, const char *val)
{
// 	assert(val != NULL);
// 	assert(paths != NULL);
	if(NULL != key) {
		size_t keyLen = strlen(key);
		for(int i=0; paths[i] != NULL; ++i) {
			if ( strncmp(paths[i], key, keyLen) == 0 ) {
				char* newPath = new char[strlen(val) + (strlen(paths[i]) - keyLen) + 1];
				strcpy(newPath, val);
				strcat(newPath, &paths[i][keyLen]);
				paths[i] = newPath;
			}
		}
	}
	return;
}

static void removePathWithPrefix(const char* paths[], const char* prefix)
{
	size_t prefixLen = strlen(prefix);
	for(int s=0,d=0; (paths[d] != NULL) && (paths[s] != NULL); ++s, ++d) {
		if ( strncmp(paths[s], prefix, prefixLen) == 0 )
			++s;
		paths[d] = paths[s];
	}
}

#if 0
static void paths_dump(const char **paths)
{
//   assert(paths != NULL);
  const char **strs = paths;
  while(*strs != NULL)
  {
    fprintf(stderr, "\"%s\"\n", *strs);
    strs++;
  }
  return;
}
#endif

static void printOptions(const char* argv[])
{
	uint32_t i = 0;
	while ( NULL != argv[i] ) {
		fprintf(stderr, "opt[%i] = \"%s\"\n", i, argv[i]);
		i++;
	}
}

static void printEnvironmentVariables(const char* envp[])
{
	while ( NULL != *envp ) {
		fprintf(stderr, "%s\n", *envp);
		envp++;
	}
}



void processDyldEnvironmentVarible(const char* key, const char* value)
{
	if ( strcmp(key, "DYLD_FRAMEWORK_PATH") == 0 ) {
		sEnv.DYLD_FRAMEWORK_PATH = parseColonList(value);
	}
	else if ( strcmp(key, "DYLD_FALLBACK_FRAMEWORK_PATH") == 0 ) {
		sEnv.DYLD_FALLBACK_FRAMEWORK_PATH = parseColonList(value);
	}
	else if ( strcmp(key, "DYLD_LIBRARY_PATH") == 0 ) {
		sEnv.DYLD_LIBRARY_PATH = parseColonList(value);
	}
	else if ( strcmp(key, "DYLD_FALLBACK_LIBRARY_PATH") == 0 ) {
		sEnv.DYLD_FALLBACK_LIBRARY_PATH = parseColonList(value);
	}
	else if ( (strcmp(key, "DYLD_ROOT_PATH") == 0) || (strcmp(key, "DYLD_PATHS_ROOT") == 0) ) {
		if ( strcmp(value, "/") != 0 ) {
			sEnv.DYLD_ROOT_PATH = parseColonList(value);
			for (int i=0; sEnv.DYLD_ROOT_PATH[i] != NULL; ++i) {
				if ( sEnv.DYLD_ROOT_PATH[i][0] != '/' ) {
					fprintf(stderr, "dyld: warning DYLD_ROOT_PATH not used because it contains a non-absolute path\n");
					sEnv.DYLD_ROOT_PATH = NULL;
					break;
				}
			}
		}
	} 
	else if ( strcmp(key, "DYLD_IMAGE_SUFFIX") == 0 ) {
		gLinkContext.imageSuffix = value;
	}
	else if ( strcmp(key, "DYLD_INSERT_LIBRARIES") == 0 ) {
		sEnv.DYLD_INSERT_LIBRARIES = parseColonList(value);
	}
	else if ( strcmp(key, "DYLD_DEBUG_TRACE") == 0 ) {
		fprintf(stderr, "dyld: warning DYLD_DEBUG_TRACE not supported\n");
	}
	else if ( strcmp(key, "DYLD_ERROR_PRINT") == 0 ) {
		fprintf(stderr, "dyld: warning DYLD_ERROR_PRINT not supported\n");
	}
	else if ( strcmp(key, "DYLD_PRINT_OPTS") == 0 ) {
		sEnv.DYLD_PRINT_OPTS = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_ENV") == 0 ) {
		sEnv.DYLD_PRINT_ENV = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_LIBRARIES") == 0 ) {
		sEnv.DYLD_PRINT_LIBRARIES = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_LIBRARIES_POST_LAUNCH") == 0 ) {
		sEnv.DYLD_PRINT_LIBRARIES_POST_LAUNCH = true;
	}
	else if ( strcmp(key, "DYLD_TRACE") == 0 ) {
		fprintf(stderr, "dyld: warning DYLD_TRACE not supported\n");
	}
	else if ( strcmp(key, "DYLD_EBADEXEC_ONLY") == 0 ) {
		fprintf(stderr, "dyld: warning DYLD_EBADEXEC_ONLY not supported\n");
	}
	else if ( strcmp(key, "DYLD_BIND_AT_LAUNCH") == 0 ) {
		sEnv.DYLD_BIND_AT_LAUNCH = true;
	}
	else if ( strcmp(key, "DYLD_FORCE_FLAT_NAMESPACE") == 0 ) {
		gLinkContext.bindFlat = true;
	}
	else if ( strcmp(key, "DYLD_DEAD_LOCK_HANG") == 0 ) {
		fprintf(stderr, "dyld: warning DYLD_DEAD_LOCK_HANG not supported\n");
	}
	else if ( strcmp(key, "DYLD_ABORT_MULTIPLE_INITS") == 0 ) {
		fprintf(stderr, "dyld: warning DYLD_ABORT_MULTIPLE_INITS not supported\n");
	}
	else if ( strcmp(key, "DYLD_NEW_LOCAL_SHARED_REGIONS") == 0 ) {
		gLinkContext.sharedRegionMode = ImageLoader::kUsePrivateSharedRegion;
	}
	else if ( strcmp(key, "DYLD_SLIDE_AND_PACK_DYLIBS") == 0 ) {
		gLinkContext.slideAndPackDylibs = true;
	}
	else if ( strcmp(key, "DYLD_NO_FIX_PREBINDING") == 0 ) {
		// since the new dyld never runs fix_prebinding, no need to warn if someone does not want it run
		//fprintf(stderr, "dyld: warning DYLD_NO_FIX_PREBINDING not supported\n");
	}
	else if ( strcmp(key, "DYLD_PREBIND_DEBUG") == 0 ) {
		gLinkContext.verbosePrebinding = true;
	}
	else if ( strcmp(key, "DYLD_HINTS_DEBUG") == 0 ) {
		fprintf(stderr, "dyld: warning DYLD_HINTS_DEBUG not supported\n");
	}
	else if ( strcmp(key, "DYLD_SAMPLE_DEBUG") == 0 ) {
		fprintf(stderr, "dyld: warning DYLD_SAMPLE_DEBUG not supported\n");
	}
	else if ( strcmp(key, "DYLD_EXECUTABLE_PATH_DEBUG") == 0 ) {
		fprintf(stderr, "dyld: warning DYLD_EXECUTABLE_PATH_DEBUG not supported\n");
	}
	else if ( strcmp(key, "DYLD_TWO_LEVEL_DEBUG") == 0 ) {
		fprintf(stderr, "dyld: warning DYLD_TWO_LEVEL_DEBUG not supported\n");
	}
	else if ( strcmp(key, "DYLD_LAZY_INITIALIZERS") == 0 ) {
		fprintf(stderr, "dyld: warning DYLD_LAZY_INITIALIZERS not supported\n");
	}
	else if ( strcmp(key, "DYLD_PRINT_INITIALIZERS") == 0 ) {
		gLinkContext.verboseInit = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_STATISTICS") == 0 ) {
		sEnv.DYLD_PRINT_STATISTICS = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_SEGMENTS") == 0 ) {
		gLinkContext.verboseMapping = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_BINDINGS") == 0 ) {
		gLinkContext.verboseBind = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_REBASINGS") == 0 ) {
		gLinkContext.verboseRebase = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_APIS") == 0 ) {
		gLogAPIs = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_WARNINGS") == 0 ) {
		gLinkContext.verboseWarnings = true;
	}
	else if ( strcmp(key, "DYLD_SHARED_REGION") == 0 ) {
		if ( strcmp(value, "private") == 0 ) {
			gLinkContext.sharedRegionMode = ImageLoader::kUsePrivateSharedRegion;
		}
		else if ( strcmp(value, "avoid") == 0 ) {
			gLinkContext.sharedRegionMode = ImageLoader::kDontUseSharedRegion;
		}
		else if ( strcmp(value, "use") == 0 ) {
			gLinkContext.sharedRegionMode = ImageLoader::kUseSharedRegion;
		}
		else if ( value[0] == '\0' ) {
			gLinkContext.sharedRegionMode = ImageLoader::kUseSharedRegion;
		}
		else {
			fprintf(stderr, "dyld: warning unknown option to DYLD_SHARED_REGION.  Valid options are: use, private, avoid\n");
		}
	}
	else if ( strcmp(key, "DYLD_IGNORE_PREBINDING") == 0 ) {
		if ( strcmp(value, "all") == 0 ) {
			gLinkContext.prebindUsage = ImageLoader::kUseNoPrebinding;
		}
		else if ( strcmp(value, "app") == 0 ) {
			gLinkContext.prebindUsage = ImageLoader::kUseAllButAppPredbinding;
		}
		else if ( strcmp(value, "nonsplit") == 0 ) {
			gLinkContext.prebindUsage = ImageLoader::kUseSplitSegPrebinding;
		}
		else if ( value[0] == '\0' ) {
			gLinkContext.prebindUsage = ImageLoader::kUseSplitSegPrebinding;
		}
		else {
			fprintf(stderr, "dyld: warning unknown option to DYLD_IGNORE_PREBINDING.  Valid options are: all, app, nonsplit\n");
		}
	}
	else {
		fprintf(stderr, "dyld: warning, unknown environment variable: %s\n", key);
	}
}

//
// For security, setuid programs ignore DYLD_* environment variables.
// Additionally, the DYLD_* enviroment variables are removed
// from the environment, so that any child processes don't see them.
//
static void pruneEnvironmentVariables(const char* envp[], const char*** applep)
{
	// delete all DYLD_* and LD_LIBRARY_PATH environment variables
	int removedCount = 0;
	const char** d = envp;
	for(const char** s = envp; *s != NULL; s++) {
	    if ( (strncmp(*s, "DYLD_", 5) != 0) && (strncmp(*s, "LD_LIBRARY_PATH=", 16) != 0) ) {
			*d++ = *s;
		}
		else {
			++removedCount;
		}
	}
	*d++ = NULL;
	
	// slide apple parameters
	if ( removedCount > 0 ) {
		*applep = d;
		do {
			*d = d[removedCount];
		} while ( *d++ != NULL );
	}
	
	// setup DYLD_FALLBACK_FRAMEWORK_PATH, if not set in environment
	if ( sEnv.DYLD_FALLBACK_FRAMEWORK_PATH == NULL ) {
		const char** paths = sFrameworkFallbackPaths;
		removePathWithPrefix(paths, "$HOME");
		sEnv.DYLD_FALLBACK_FRAMEWORK_PATH = paths;
	}

	// default value for DYLD_FALLBACK_LIBRARY_PATH, if not set in environment
	if ( sEnv.DYLD_FALLBACK_LIBRARY_PATH == NULL ) {
		const char** paths = sLibraryFallbackPaths;
		removePathWithPrefix(paths, "$HOME");
		sEnv.DYLD_FALLBACK_LIBRARY_PATH = paths;
	}
}

static void checkEnvironmentVariables(const char* envp[], bool ignoreEnviron)
{
	const char* home = NULL;
	const char** p;
	for(p = envp; *p != NULL; p++) {
		const char* keyEqualsValue = *p;
	    if ( strncmp(keyEqualsValue, "DYLD_", 5) == 0 ) {
			const char* equals = strchr(keyEqualsValue, '=');
			if ( (equals != NULL) && !ignoreEnviron ) {
				const char* value = &equals[1];
				const int keyLen = equals-keyEqualsValue;
				char key[keyLen+1];
				strncpy(key, keyEqualsValue, keyLen);
				key[keyLen] = '\0';
				processDyldEnvironmentVarible(key, value);
			}
		}
	    else if ( strncmp(keyEqualsValue, "HOME=", 5) == 0 ) {
			home = &keyEqualsValue[5];
		}
		else if ( strncmp(keyEqualsValue, "LD_LIBRARY_PATH=", 16) == 0 ) {
			const char* path = &keyEqualsValue[16];
			sEnv.LD_LIBRARY_PATH = parseColonList(path);
		}
	}
		
	// default value for DYLD_FALLBACK_FRAMEWORK_PATH, if not set in environment
	if ( sEnv.DYLD_FALLBACK_FRAMEWORK_PATH == NULL ) {
		const char** paths = sFrameworkFallbackPaths;
		if ( home == NULL )
			removePathWithPrefix(paths, "$HOME");
		else
			paths_expand_roots(paths, "$HOME", home);
		sEnv.DYLD_FALLBACK_FRAMEWORK_PATH = paths;
	}

	// default value for DYLD_FALLBACK_LIBRARY_PATH, if not set in environment
	if ( sEnv.DYLD_FALLBACK_LIBRARY_PATH == NULL ) {
		const char** paths = sLibraryFallbackPaths;
		if ( home == NULL ) 
			removePathWithPrefix(paths, "$HOME");
		else
			paths_expand_roots(paths, "$HOME", home);
		sEnv.DYLD_FALLBACK_LIBRARY_PATH = paths;
	}
}


static void getHostInfo()
{
#if 0
	struct host_basic_info info;
	mach_msg_type_number_t count = HOST_BASIC_INFO_COUNT;
	mach_port_t hostPort = mach_host_self();
	kern_return_t result = host_info(hostPort, HOST_BASIC_INFO, (host_info_t)&info, &count);
	mach_port_deallocate(mach_task_self(), hostPort);
	if ( result != KERN_SUCCESS )
		throw "host_info() failed";
	
	sHostCPU		= info.cpu_type;
	sHostCPUsubtype = info.cpu_subtype;
#endif

	size_t valSize = sizeof(sHostCPU);
	if (sysctlbyname ("hw.cputype", &sHostCPU, &valSize, NULL, 0) != 0) 
		throw "sysctlbyname(hw.cputype) failed";
	valSize = sizeof(sHostCPUsubtype);
	if (sysctlbyname ("hw.cpusubtype", &sHostCPUsubtype, &valSize, NULL, 0) != 0) 
		throw "sysctlbyname(hw.cpusubtype) failed";
}

bool validImage(ImageLoader* possibleImage)
{
	const unsigned int imageCount = sAllImages.size();
	for(unsigned int i=0; i < imageCount; ++i) {
		if ( possibleImage == sAllImages[i] ) {
			return true;
		}
	}
	return false;
}

uint32_t getImageCount()
{
	if ( sAllImagesMightContainUnlinkedImages ) {
		uint32_t count = 0;
		for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); it++) {
			if ( (*it)->isLinked() )
				++count;
		}
		return count;
	}
	else {
		return sAllImages.size();
	}
}

ImageLoader* getIndexedImage(unsigned int index)
{
	if ( sAllImagesMightContainUnlinkedImages ) {
		uint32_t count = 0;
		for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); it++) {
			if ( (*it)->isLinked() ) {
				if ( index == count )
					return *it;
				++count;
			}
		}
	}
	else {
		if ( index < sAllImages.size() )
			return sAllImages[index];
	}
	return NULL;
}

ImageLoader* findImageByMachHeader(const struct mach_header* target)
{
	const unsigned int imageCount = sAllImages.size();
	for(unsigned int i=0; i < imageCount; ++i) {
		ImageLoader* anImage = sAllImages[i];
		if ( anImage->machHeader() == target )
			return anImage;
	}
	return NULL;
}


ImageLoader* findImageContainingAddress(const void* addr)
{
#if FIND_STATS	
	static int cacheHit = 0; 
	static int cacheMiss = 0; 
	static int cacheNotMacho = 0; 
	if ( ((cacheHit+cacheMiss+cacheNotMacho) % 100) == 0 )
		fprintf(stderr, "findImageContainingAddress(): cache hit = %d, miss = %d, unknown = %d\n", cacheHit, cacheMiss, cacheNotMacho);
#endif
	// first look in image where last address was found rdar://problem/3685517
	if ( (sLastImageByAddressCache != NULL) && sLastImageByAddressCache->containsAddress(addr) ) {
#if FIND_STATS	
		++cacheHit;
#endif
		return sLastImageByAddressCache;
	}
	// do exhastive search 
	// todo: consider maintaining a list sorted by address ranges and do a binary search on that
	const unsigned int imageCount = sAllImages.size();
	for(unsigned int i=0; i < imageCount; ++i) {
		ImageLoader* anImage = sAllImages[i];
		if ( anImage->containsAddress(addr) ) {
			sLastImageByAddressCache = anImage;
#if FIND_STATS	
		++cacheMiss;
#endif
			return anImage;
		}
	}
#if FIND_STATS	
		++cacheNotMacho;
#endif
	return NULL;
}

ImageLoader* findImageContainingAddressThreadSafe(const void* addr)
{
	// do exhastive search 
	// todo: consider maintaining a list sorted by address ranges and do a binary search on that
	const unsigned int imageCount = sAllImages.size();
	for(unsigned int i=0; i < imageCount; ++i) {
		ImageLoader* anImage = sAllImages[i];
		if ( anImage->containsAddress(addr) ) {
			return anImage;
		}
	}
	return NULL;
}


void forEachImageDo( void (*callback)(ImageLoader*, void* userData), void* userData)
{
	const unsigned int imageCount = sAllImages.size();
	for(unsigned int i=0; i < imageCount; ++i) {
		ImageLoader* anImage = sAllImages[i];
		(*callback)(anImage, userData);
	}
}

ImageLoader* findLoadedImage(const struct stat& stat_buf)
{
	const unsigned int imageCount = sAllImages.size();
	for(unsigned int i=0; i < imageCount; ++i){
		ImageLoader* anImage = sAllImages[i];
		if ( anImage->statMatch(stat_buf) )
			return anImage;
	}
	return NULL;
}

// based on ANSI-C strstr()
static const char* strrstr(const char* str, const char* sub) 
{
	const int sublen = strlen(sub);
	for(const char* p = &str[strlen(str)]; p != str; --p) {
		if ( strncmp(p, sub, sublen) == 0 )
			return p;
	}
	return NULL;
}


//
// Find framework path
//
//  /path/foo.framework/foo								=>   foo.framework/foo	
//  /path/foo.framework/Versions/A/foo					=>   foo.framework/Versions/A/foo
//  /path/foo.framework/Frameworks/bar.framework/bar	=>   bar.framework/bar
//  /path/foo.framework/Libraries/bar.dylb				=>   NULL
//  /path/foo.framework/bar								=>   NULL
//
// Returns NULL if not a framework path
//
static const char* getFrameworkPartialPath(const char* path)
{
	const char* dirDot = strrstr(path, ".framework/");
	if ( dirDot != NULL ) {
		const char* dirStart = dirDot;
		for ( ; dirStart >= path; --dirStart) {
			if ( (*dirStart == '/') || (dirStart == path) ) {
				const char* frameworkStart = &dirStart[1];
				if ( dirStart == path )
					--frameworkStart;
				int len = dirDot - frameworkStart;
				char framework[len+1];
				strncpy(framework, frameworkStart, len);
				framework[len] = '\0';
				const char* leaf = strrchr(path, '/');
				if ( leaf != NULL ) {
					if ( strcmp(framework, &leaf[1]) == 0 ) {
						return frameworkStart;
					}
					if (  gLinkContext.imageSuffix != NULL ) {
						// some debug frameworks have install names that end in _debug
						if ( strncmp(framework, &leaf[1], len) == 0 ) {
							if ( strcmp( gLinkContext.imageSuffix, &leaf[len+1]) == 0 )
								return frameworkStart;
						}
					}
				}
			}
		}
	}
	return NULL;
}


static const char* getLibraryLeafName(const char* path)
{
	const char* start = strrchr(path, '/');
	if ( start != NULL )
		return &start[1];
	else
		return path;
}



const cpu_subtype_t CPU_SUBTYPE_END_OF_LIST = -1;


//
//	A fat file may contain multiple sub-images for the same CPU type.
//	In that case, dyld picks which sub-image to use by scanning a table
//	of preferred cpu-sub-types for the running cpu.  
//	
//	There is one row in the table for each cpu-sub-type on which dyld might run.
//  The first entry in a row is that cpu-sub-type.  It is followed by all
//	cpu-sub-types that can run on that cpu, if preferred order.  Each row ends with 
//	a "SUBTYPE_ALL" (to denote that images written to run on any cpu-sub-type are usable), 
//  followed by one or more CPU_SUBTYPE_END_OF_LIST to pad out this row.
//


//	 
//	32-bit PowerPC sub-type lists
//
const int kPPC_RowCount = 4;
static const cpu_subtype_t kPPC32[kPPC_RowCount][6] = { 
	// G5 can run any code
	{  CPU_SUBTYPE_POWERPC_970, CPU_SUBTYPE_POWERPC_7450,  CPU_SUBTYPE_POWERPC_7400, CPU_SUBTYPE_POWERPC_750, CPU_SUBTYPE_POWERPC_ALL, CPU_SUBTYPE_END_OF_LIST },
	
	// G4 can run all but G5 code
	{  CPU_SUBTYPE_POWERPC_7450,  CPU_SUBTYPE_POWERPC_7400,  CPU_SUBTYPE_POWERPC_750, CPU_SUBTYPE_POWERPC_ALL, CPU_SUBTYPE_END_OF_LIST, CPU_SUBTYPE_END_OF_LIST },
	{  CPU_SUBTYPE_POWERPC_7400,  CPU_SUBTYPE_POWERPC_7450,  CPU_SUBTYPE_POWERPC_750, CPU_SUBTYPE_POWERPC_ALL, CPU_SUBTYPE_END_OF_LIST, CPU_SUBTYPE_END_OF_LIST },

	// G3 cannot run G4 or G5 code
	{ CPU_SUBTYPE_POWERPC_750,  CPU_SUBTYPE_POWERPC_ALL, CPU_SUBTYPE_END_OF_LIST,  CPU_SUBTYPE_END_OF_LIST, CPU_SUBTYPE_END_OF_LIST, CPU_SUBTYPE_END_OF_LIST }
};


//	 
//	64-bit PowerPC sub-type lists
//
const int kPPC64_RowCount = 1;
static const cpu_subtype_t kPPC64[kPPC64_RowCount][3] = { 
	// G5 can run any 64-bit code
	{  CPU_SUBTYPE_POWERPC_970, CPU_SUBTYPE_POWERPC_ALL, CPU_SUBTYPE_END_OF_LIST },
};



//	 
//	32-bit x86 sub-type lists
//
//	TO-DO



// scan the tables above to find the cpu-sub-type-list for this machine
static const cpu_subtype_t* findCPUSubtypeList(cpu_type_t cpu, cpu_subtype_t subtype)
{
	switch (cpu) {
		case CPU_TYPE_POWERPC:
			for (int i=0; i < kPPC_RowCount ; ++i) {
				if ( kPPC32[i][0] == subtype )
					return kPPC32[i];
			}
			break;
		case CPU_TYPE_POWERPC64:
			for (int i=0; i < kPPC64_RowCount ; ++i) {
				if ( kPPC64[i][0] == subtype )
					return kPPC64[i];
			}
			break;
		case CPU_TYPE_I386:
			// To do 
			break;
	}
	return NULL;
}




// scan fat table-of-contents for best most preferred subtype
static bool fatFindBestFromOrderedList(cpu_type_t cpu, const cpu_subtype_t list[], const fat_header* fh, uint64_t* offset, uint64_t* len)
{
	const fat_arch* const archs = (fat_arch*)(((char*)fh)+sizeof(fat_header));
	for (uint32_t subTypeIndex=0; list[subTypeIndex] != CPU_SUBTYPE_END_OF_LIST; ++subTypeIndex) {
		for(uint32_t fatIndex=0; fatIndex < OSSwapBigToHostInt32(fh->nfat_arch); ++fatIndex) {
			if ( ((cpu_type_t)OSSwapBigToHostInt32(archs[fatIndex].cputype) == cpu) 
				&& (list[subTypeIndex] == archs[fatIndex].cpusubtype) ) {
				*offset = OSSwapBigToHostInt32(archs[fatIndex].offset);
				*len = OSSwapBigToHostInt32(archs[fatIndex].size);
				return true;
			}
		}
	}
	return false;
}

// scan fat table-of-contents for exact match of cpu and cpu-sub-type
static bool fatFindExactMatch(cpu_type_t cpu, cpu_subtype_t subtype, const fat_header* fh, uint64_t* offset, uint64_t* len)
{
	const fat_arch* archs = (fat_arch*)(((char*)fh)+sizeof(fat_header));
	for(uint32_t i=0; i < OSSwapBigToHostInt32(fh->nfat_arch); ++i) {
		if ( ((cpu_type_t)OSSwapBigToHostInt32(archs[i].cputype) == cpu)
			&& ((cpu_subtype_t)OSSwapBigToHostInt32(archs[i].cpusubtype) == subtype) ) {
			*offset = OSSwapBigToHostInt32(archs[i].offset);
			*len = OSSwapBigToHostInt32(archs[i].size);
			return true;
		}
	}
	return false;
}

// scan fat table-of-contents for image with matching cpu-type and runs-on-all-sub-types
static bool fatFindRunsOnAllCPUs(cpu_type_t cpu, const fat_header* fh, uint64_t* offset, uint64_t* len)
{
	const fat_arch* archs = (fat_arch*)(((char*)fh)+sizeof(fat_header));
	for(uint32_t i=0; i < OSSwapBigToHostInt32(fh->nfat_arch); ++i) {
		if ( (cpu_type_t)OSSwapBigToHostInt32(archs[i].cputype) == cpu) {
			switch (cpu) {
				case CPU_TYPE_POWERPC:
				case CPU_TYPE_POWERPC64:
					if ( (cpu_subtype_t)OSSwapBigToHostInt32(archs[i].cpusubtype) == CPU_SUBTYPE_POWERPC_ALL ) {
						*offset = OSSwapBigToHostInt32(archs[i].offset);
						*len = OSSwapBigToHostInt32(archs[i].size);
						return true;
					}
					break;
				case CPU_TYPE_I386:
					if ( (cpu_subtype_t)OSSwapBigToHostInt32(archs[i].cpusubtype) == CPU_SUBTYPE_I386_ALL ) {
						*offset = OSSwapBigToHostInt32(archs[i].offset);
						*len = OSSwapBigToHostInt32(archs[i].size);
						return true;
					}
					break;
			}
		}
	}
	return false;
}


//
// A fat file may contain multiple sub-images for the same cpu-type,
// each optimized for a different cpu-sub-type (e.g G3 or G5).
// This routine picks the optimal sub-image.
//
static bool fatFindBest(const fat_header* fh, uint64_t* offset, uint64_t* len)
{
	// assume all dylibs loaded must have same cpu type as main executable
	const cpu_type_t cpu = sMainExecutableMachHeader->cputype;

	// We only know the subtype to use if the main executable cpu type matches the host
	if ( (cpu & CPU_TYPE_MASK) == sHostCPU ) {
		// get preference ordered list of subtypes
		const cpu_subtype_t* subTypePreferenceList = findCPUSubtypeList(cpu, sHostCPUsubtype);
	
		// use ordered list to find best sub-image in fat file
		if ( subTypePreferenceList != NULL ) 
			return fatFindBestFromOrderedList(cpu, subTypePreferenceList, fh, offset, len);
		
		// if running cpu is not in list, try for an exact match
		if ( fatFindExactMatch(cpu, sHostCPUsubtype, fh, offset, len) )
			return true;
	}
	
	// running on an uknown cpu, can only load generic code
	return fatFindRunsOnAllCPUs(cpu, fh, offset, len);
}



//
// This is used to validate if a non-fat (aka thin or raw) mach-o file can be used
// on the current processor.  It is deemed compatible if any of the following are true:
//  1) mach_header subtype is in list of compatible subtypes for running processor
//  2) mach_header subtype is same as running processor subtype
//  3) mach_header subtype runs on all processor variants
//
//
bool isCompatibleMachO(const uint8_t* firstPage)
{
	const mach_header* mh = (mach_header*)firstPage;
	if ( mh->magic == sMainExecutableMachHeader->magic ) {
		if ( mh->cputype == sMainExecutableMachHeader->cputype ) {
			if ( (mh->cputype & CPU_TYPE_MASK) == sHostCPU ) {
				// get preference ordered list of subtypes that this machine can use
				const cpu_subtype_t* subTypePreferenceList = findCPUSubtypeList(mh->cputype, sHostCPUsubtype);
				if ( subTypePreferenceList != NULL ) {
					// if image's subtype is in the list, it is compatible
					for (const cpu_subtype_t* p = subTypePreferenceList; *p != CPU_SUBTYPE_END_OF_LIST; ++p) {
						if ( *p == mh->cpusubtype )
							return true;
					}
					// have list and not in list, so not compatible
					throw "incompatible cpu-subtype";
				}
				// unknown cpu sub-type, but if exact match for current subtype then ok to use
				if ( mh->cpusubtype == sHostCPUsubtype ) 
					return true;
			}
			
			// cpu unknown, so don't know if subtype is compatible
			// only load _ALL variant
			switch (mh->cputype) {
				case CPU_TYPE_POWERPC:
				case CPU_TYPE_POWERPC64:
					if ( mh->cpusubtype == CPU_SUBTYPE_POWERPC_ALL ) 
						return true;
					break;
				case CPU_TYPE_I386:
					if ( mh->cpusubtype == CPU_SUBTYPE_I386_ALL ) 
						return true;
					break;					
			}
		}
	}
	return false;
}


// The kernel maps in main executable before dyld gets control.  We need to 
// make an ImageLoader* for the already mapped in main executable.
static ImageLoader* instantiateFromLoadedImage(const struct mach_header* mh, const char* path)
{
	// try mach-o loader
	if ( isCompatibleMachO((const uint8_t*)mh) ) {
		ImageLoader* image = new ImageLoaderMachO(path, mh, 0, gLinkContext);
		addImage(image);
		return image;
	}
	
	throw "main executable not a known format";
}




// map in file and instantiate an ImageLoader
static ImageLoader* loadPhase6(int fd, struct stat& stat_buf, const char* path, const LoadContext& context)
{
	//fprintf(stderr, "%s(%s)\n", __func__ , path);
	uint64_t fileOffset = 0;
	uint64_t fileLength = stat_buf.st_size;
#if __ppc64__
	if ( *((uint32_t*)((char*)(&stat_buf)+0x60)) == 0xFEFEFEFE )
		fileLength = *((uint64_t*)((char*)(&stat_buf)+0x30));	// HACK work around for kernel stat bug rdar://problem/3845883
#endif

	// validate it is a file (not directory)
	if ( (stat_buf.st_mode & S_IFMT) != S_IFREG ) 
		throw "not a file";

	// min file is 4K
	if ( fileLength < 4096 ) {
		throw "file to short";
	}
	
	uint8_t firstPage[4096];
	pread(fd, firstPage, 4096,0);
	
	// if fat wrapper, find usable sub-file
	const fat_header* fileStartAsFat = (fat_header*)firstPage;
	if ( fileStartAsFat->magic == OSSwapBigToHostInt32(FAT_MAGIC) ) {
		if ( fatFindBest(fileStartAsFat, &fileOffset, &fileLength) ) {
			pread(fd, firstPage, 4096, fileOffset);
		}
		else {
			throw "no matching architecture in universal wrapper";
		}
	}
	
	// try mach-o loader
	if ( isCompatibleMachO(firstPage) ) {
		char realFilePath[PATH_MAX];
		if ( gLinkContext.slideAndPackDylibs ) {
			// when prebinding, we always want to track the real path of images
			if ( realpath(path, realFilePath) != NULL )
				path = realFilePath;
		}

		// instantiate an image
		ImageLoader* image = new ImageLoaderMachO(path, fd, firstPage, fileOffset, fileLength, stat_buf, gLinkContext);
		
		// now sanity check that this loaded image does not have the same install path as any existing image
		const char* loadedImageInstallPath = image->getInstallPath();
		if ( image->isDylib() && (loadedImageInstallPath != NULL) && (loadedImageInstallPath[0] == '/') ) {
			for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); it++) {
				ImageLoader* anImage = *it;
				const char* installPath = anImage->getInstallPath();
				if ( installPath != NULL) {
					if ( strcmp(loadedImageInstallPath, installPath) == 0 ) {
						//fprintf(stderr, "duplicate(%s) => %p\n", installPath, anImage);
						delete image;
						return anImage;
					}
				}
			}
		}

		// some API's restrict what they can load
		if ( context.mustBeBundle && !image->isBundle() )
			throw "not a bundle";
		if ( context.mustBeDylib && !image->isDylib() )
			throw "not a dylib";

		// don't add bundles to global list, they can be loaded but not linked.  When linked it will be added to list
		if ( ! image->isBundle() ) 
			addImage(image);

		return image;
	}
	
	// try other file formats...
	
	
	// throw error about what was found
	switch (*(uint32_t*)firstPage) {
		case MH_MAGIC:
		case MH_CIGAM:
		case MH_MAGIC_64:
		case MH_CIGAM_64:
			throw "mach-o, but wrong architecture";
		default:
		throwf("unknown file type, first eight bytes: 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X", 
			firstPage[0], firstPage[1], firstPage[2], firstPage[3], firstPage[4], firstPage[5], firstPage[6],firstPage[7]);
	}
}


// try to open file
static ImageLoader* loadPhase5open(const char* path, const LoadContext& context, std::vector<const char*>* exceptions)
{
	//fprintf(stdout, "%s(%s)\n", __func__, path);
	ImageLoader* image = NULL;

	// open file (automagically closed when this function exits)
	FileOpener file(path);
	
	//fprintf(stderr, "open(%s) => %d\n", path, file.getFileDescriptor() );
	
	if ( file.getFileDescriptor() == -1 )
		return NULL;
		
	struct stat stat_buf;
#if __ppc64__
	memset(&stat_buf, 254, sizeof(struct stat));	 // hack until rdar://problem/3845883 is fixed
#endif
	if ( fstat(file.getFileDescriptor(), &stat_buf) == -1)
		throw "stat error";
	
	// in case image was renamed or found via symlinks, check for inode match
	image = findLoadedImage(stat_buf);
	if ( image != NULL )
		return image;
	
	// needed to implement NSADDIMAGE_OPTION_RETURN_ONLY_IF_LOADED
	if ( context.dontLoad )
		return NULL;
	
	try {
		return loadPhase6(file.getFileDescriptor(), stat_buf, path, context);
	}
	catch (const char* msg) {
		char* newMsg = new char[strlen(msg) + strlen(path) + 8];
		sprintf(newMsg, "%s: %s", path, msg);
		exceptions->push_back(newMsg);
		return NULL;
	}
}

// look for path match with existing loaded images
static ImageLoader* loadPhase5check(const char* path, const LoadContext& context)
{
	//fprintf(stderr, "%s(%s)\n", __func__ , path);
	// search path against load-path and install-path of all already loaded images
	uint32_t hash = ImageLoader::hash(path);
	for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); it++) {
		ImageLoader* anImage = *it;
		// check has first to cut down on strcmp calls
		if ( anImage->getPathHash() == hash )
			if ( strcmp(path, anImage->getPath()) == 0 ) {
				// if we are looking for a dylib don't return something else
				if ( !context.mustBeDylib || anImage->isDylib() )
					return anImage;
			}
		if ( context.matchByInstallName || anImage->matchInstallPath() ) {
			const char* installPath = anImage->getInstallPath();
			if ( installPath != NULL) {
				if ( strcmp(path, installPath) == 0 ) {
					// if we are looking for a dylib don't return something else
					if ( !context.mustBeDylib || anImage->isDylib() )
						return anImage;
				}
			}
		}
	}
	
	//fprintf(stderr, "check(%s) => NULL\n", path);
	return NULL;
}


// open or check existing
static ImageLoader* loadPhase5(const char* path, const LoadContext& context, std::vector<const char*>* exceptions)
{
	//fprintf(stderr, "%s(%s)\n", __func__ , path);
	if ( exceptions != NULL ) 
		return loadPhase5open(path, context, exceptions);
	else
		return loadPhase5check(path, context);
}

// try with and without image suffix
static ImageLoader* loadPhase4(const char* path, const LoadContext& context, std::vector<const char*>* exceptions)
{
	//fprintf(stderr, "%s(%s)\n", __func__ , path);
	ImageLoader* image = NULL;
	if (  gLinkContext.imageSuffix != NULL ) {
		char pathWithSuffix[strlen(path)+strlen( gLinkContext.imageSuffix)+2];
		ImageLoader::addSuffix(path,  gLinkContext.imageSuffix, pathWithSuffix);
		image = loadPhase5(pathWithSuffix, context, exceptions);
	}
	if ( image == NULL )
		image = loadPhase5(path, context, exceptions);
	return image;
}


// expand @ variables
static ImageLoader* loadPhase3(const char* path, const LoadContext& context, std::vector<const char*>* exceptions)
{
	//fprintf(stderr, "%s(%s)\n", __func__ , path);
	ImageLoader* image = NULL;
	if ( strncmp(path, "@executable_path/", 17) == 0 ) {
		// handle @executable_path path prefix
		const char* executablePath = sExecPath;
		char newPath[strlen(executablePath) + strlen(path)];
		strcpy(newPath, executablePath);
		char* addPoint = strrchr(newPath,'/');
		if ( addPoint != NULL )
			strcpy(&addPoint[1], &path[17]);
		else
			strcpy(newPath, &path[17]);
		image = loadPhase4(newPath, context, exceptions);
		if ( image != NULL ) 
			return image;

		// perhaps main executable path is a sym link, find realpath and retry
		char resolvedPath[PATH_MAX];
		if ( realpath(sExecPath, resolvedPath) != NULL ) {
			char newRealPath[strlen(resolvedPath) + strlen(path)];
			strcpy(newRealPath, resolvedPath);
			char* addPoint = strrchr(newRealPath,'/');
			if ( addPoint != NULL )
				strcpy(&addPoint[1], &path[17]);
			else
				strcpy(newRealPath, &path[17]);
			image = loadPhase4(newRealPath, context, exceptions);
			if ( image != NULL ) 
				return image;
		}
	}
	else if ( (strncmp(path, "@loader_path/", 13) == 0) && (context.origin != NULL) ) {
		// handle @loader_path path prefix
		char newPath[strlen(context.origin) + strlen(path)];
		strcpy(newPath, context.origin);
		char* addPoint = strrchr(newPath,'/');
		if ( addPoint != NULL )
			strcpy(&addPoint[1], &path[13]);
		else
			strcpy(newPath, &path[13]);
		image = loadPhase4(newPath, context, exceptions);
		if ( image != NULL ) 
			return image;
		
		// perhaps loader path is a sym link, find realpath and retry
		char resolvedPath[PATH_MAX];
		if ( realpath(context.origin, resolvedPath) != NULL ) {
			char newRealPath[strlen(resolvedPath) + strlen(path)];
			strcpy(newRealPath, resolvedPath);
			char* addPoint = strrchr(newRealPath,'/');
			if ( addPoint != NULL )
				strcpy(&addPoint[1], &path[13]);
			else
				strcpy(newRealPath, &path[13]);
			image = loadPhase4(newRealPath, context, exceptions);
			if ( image != NULL ) 
				return image;
		}
	}
	
	return loadPhase4(path, context, exceptions);
}


// try search paths
static ImageLoader* loadPhase2(const char* path, const LoadContext& context, 
							   const char* const frameworkPaths[], const char* const libraryPaths[], 
							   std::vector<const char*>* exceptions)
{
	//fprintf(stderr, "%s(%s)\n", __func__ , path);
	ImageLoader* image = NULL;
	const char* frameworkPartialPath = getFrameworkPartialPath(path);
	if ( frameworkPaths != NULL ) {
		if ( frameworkPartialPath != NULL ) {
			const int frameworkPartialPathLen = strlen(frameworkPartialPath);
			for(const char* const* fp = frameworkPaths; *fp != NULL; ++fp) {
				char npath[strlen(*fp)+frameworkPartialPathLen+8];
				strcpy(npath, *fp);
				strcat(npath, "/");
				strcat(npath, frameworkPartialPath);
				//fprintf(stderr, "dyld: fallback framework path used: %s() -> loadPhase4(\"%s\", ...)\n", __func__, npath);
				image = loadPhase4(npath, context, exceptions);
				if ( image != NULL )
					return image;
			}
		}
	}
	if ( libraryPaths != NULL ) {
		const char* libraryLeafName = getLibraryLeafName(path);
		const int libraryLeafNameLen = strlen(libraryLeafName);
		for(const char* const* lp = libraryPaths; *lp != NULL; ++lp) {
			char libpath[strlen(*lp)+libraryLeafNameLen+8];
			strcpy(libpath, *lp);
			strcat(libpath, "/");
			strcat(libpath, libraryLeafName);
			//fprintf(stderr, "dyld: fallback library path used: %s() -> loadPhase4(\"%s\", ...)\n", __func__, libpath);
			image = loadPhase4(libpath, context, exceptions);
			if ( image != NULL )
				return image;
		}
	}
	return NULL;
}

// try search overrides and fallbacks
static ImageLoader* loadPhase1(const char* path, const LoadContext& context, std::vector<const char*>* exceptions)
{
	//fprintf(stderr, "%s(%s)\n", __func__ , path);
	ImageLoader* image = NULL;

	// handle LD_LIBRARY_PATH environment variables that force searching
	if ( context.useLdLibraryPath && (sEnv.LD_LIBRARY_PATH != NULL) ) {
		image = loadPhase2(path, context, NULL, sEnv.LD_LIBRARY_PATH, exceptions);
		if ( image != NULL )
			return image;
	}

	// handle DYLD_ environment variables that force searching
	if ( context.useSearchPaths && ((sEnv.DYLD_FRAMEWORK_PATH != NULL) || (sEnv.DYLD_LIBRARY_PATH != NULL)) ) {
		image = loadPhase2(path, context, sEnv.DYLD_FRAMEWORK_PATH, sEnv.DYLD_LIBRARY_PATH, exceptions);
		if ( image != NULL )
			return image;
	}
	
	// try raw path
	image = loadPhase3(path, context, exceptions);
	if ( image != NULL )
		return image;
	
	// try fallback paths during second time (will open file)
	if ( (exceptions != NULL) && ((sEnv.DYLD_FALLBACK_FRAMEWORK_PATH != NULL) || (sEnv.DYLD_FALLBACK_LIBRARY_PATH != NULL)) ) {
		image = loadPhase2(path, context, sEnv.DYLD_FALLBACK_FRAMEWORK_PATH, sEnv.DYLD_FALLBACK_LIBRARY_PATH, exceptions);
		if ( image != NULL )
			return image;
	}
		
	return NULL;
}

// try root substitutions
static ImageLoader* loadPhase0(const char* path, const LoadContext& context, std::vector<const char*>* exceptions)
{
	//fprintf(stderr, "%s(%s)\n", __func__ , path);
	
	// handle DYLD_ROOT_PATH which forces absolute paths to use a new root
	if ( (sEnv.DYLD_ROOT_PATH != NULL) && (path[0] == '/') ) {
		for(const char* const* rootPath = sEnv.DYLD_ROOT_PATH ; *rootPath != NULL; ++rootPath) {
			char newPath[strlen(*rootPath) + strlen(path)+2];
			strcpy(newPath, *rootPath);
			strcat(newPath, path);
			ImageLoader* image = loadPhase1(newPath, context, exceptions);
			if ( image != NULL )
				return image;
		}
	}

	// try raw path
	return loadPhase1(path, context, exceptions);
}

//
// Given all the DYLD_ environment variables, the general case for loading libraries
// is that any given path expands into a list of possible locations to load.  We
// also must take care to ensure two copies of the "same" library are never loaded.
//
// The algorithm used here is that there is a separate function for each "phase" of the
// path expansion.  Each phase function calls the next phase with each possible expansion
// of that phase.  The result is the last phase is called with all possible paths.  
//
// To catch duplicates the algorithm is run twice.  The first time, the last phase checks
// the path against all loaded images.  The second time, the last phase calls open() on 
// the path.  Either time, if an image is found, the phases all unwind without checking
// for other paths.
//
ImageLoader* load(const char* path, const LoadContext& context)
{
	//fprintf(stderr, "%s(%s)\n", __func__ , path);
	char realPath[PATH_MAX];
	// when DYLD_IMAGE_SUFFIX is in used, do a realpath(), otherwise a load of "Foo.framework/Foo" will not match
	if ( context.useSearchPaths && ( gLinkContext.imageSuffix != NULL) ) {
		if ( realpath(path, realPath) != NULL )
			path = realPath;
	}
	
	// try all path permutations and check against existing loaded images
	ImageLoader* image = loadPhase0(path, context, NULL);
	if ( image != NULL )
		return image;
	
	// try all path permutations and try open() until first sucesss
	std::vector<const char*> exceptions;
	image = loadPhase0(path, context, &exceptions);
	if ( image != NULL )
		return image;
	else if ( context.dontLoad )
		return NULL;
	else if ( exceptions.size() == 0 )
		throw "image not found";
	else {
		const char* msgStart = "no suitable image found.  Did find:";
		const char* delim = "\n\t";
		size_t allsizes = strlen(msgStart)+8;
		for (unsigned int i=0; i < exceptions.size(); ++i) 
			allsizes += (strlen(exceptions[i]) + strlen(delim));
		char* fullMsg = new char[allsizes];
		strcpy(fullMsg, msgStart);
		for (unsigned int i=0; i < exceptions.size(); ++i) {
			strcat(fullMsg, delim);
			strcat(fullMsg, exceptions[i]);
		}
		throw (const char*)fullMsg;
	}
}




// create when NSLinkModule is called for a second time on a bundle
ImageLoader* cloneImage(ImageLoader* image)
{
	const uint64_t offsetInFat = image->getOffsetInFatFile();

	// open file (automagically closed when this function exits)
	FileOpener file(image->getPath());
	
	struct stat stat_buf;
#if __ppc64__
	memset(&stat_buf, 254, sizeof(struct stat));	 // hack until rdar://problem/3845883 is fixed
#endif
	if ( fstat(file.getFileDescriptor(), &stat_buf) == -1)
		throw "stat error";
	
	// read first page of file
	uint8_t firstPage[4096];
	pread(file.getFileDescriptor(), firstPage, 4096, offsetInFat);
	
	// fat length is only used for sanity checking, since this image was already loaded once, just use upper bound
	uint64_t lenInFat = stat_buf.st_size - offsetInFat;
	
	// try mach-o loader
	if ( isCompatibleMachO(firstPage) ) {
		ImageLoader* clone = new ImageLoaderMachO(image->getPath(), file.getFileDescriptor(), firstPage, offsetInFat, lenInFat, stat_buf, gLinkContext);
		// don't add bundles to global list, they can be loaded but not linked.  When linked it will be added to list
		if ( ! image->isBundle() ) 
			addImage(clone);
		return clone;
	}
	
	// try other file formats...
	throw "can't clone image";
}


ImageLoader* loadFromMemory(const uint8_t* mem, uint64_t len, const char* moduleName)
{
	// if fat wrapper, find usable sub-file
	const fat_header* memStartAsFat = (fat_header*)mem;
	uint64_t fileOffset = 0;
	uint64_t fileLength = len;
	if ( memStartAsFat->magic == OSSwapBigToHostInt32(FAT_MAGIC) ) {
		if ( fatFindBest(memStartAsFat, &fileOffset, &fileLength) ) {
			mem = &mem[fileOffset];
			len = fileLength;
		}
		else {
			throw "no matching architecture in universal wrapper";
		}
	}

	// try mach-o each loader
	if ( isCompatibleMachO(mem) ) {
		ImageLoader* image = new ImageLoaderMachO(moduleName, (mach_header*)mem, len, gLinkContext);
		// don't add bundles to global list, they can be loaded but not linked.  When linked it will be added to list
		if ( ! image->isBundle() ) 
			addImage(image);
		return image;
	}
	
	// try other file formats...
	
	// throw error about what was found
	switch (*(uint32_t*)mem) {
		case MH_MAGIC:
		case MH_CIGAM:
		case MH_MAGIC_64:
		case MH_CIGAM_64:
			throw "mach-o, but wrong architecture";
		default:
		throwf("unknown file type, first eight bytes: 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X", 
			mem[0], mem[1], mem[2], mem[3], mem[4], mem[5], mem[6],mem[7]);
	}
}


void registerAddCallback(ImageCallback func)
{
	// now add to list to get notified when any more images are added
	sAddImageCallbacks.push_back(func);
	
	// call callback with all existing images, starting at roots
	const int rootCount = sImageRoots.size();
	for(int i=0; i < rootCount; ++i) {
		ImageLoader* image = sImageRoots[i];
		image->runNotification(gLinkContext, sAddImageCallbacks.size());
	}
	
//	for (std::vector<ImageLoader*>::iterator it=sImageRoots.begin(); it != sImageRoots.end(); it++) {
//		ImageLoader* image = *it;
//		image->runNotification(gLinkContext, sAddImageCallbacks.size());
//	}
}

void registerRemoveCallback(ImageCallback func)
{
	sRemoveImageCallbacks.push_back(func);
}

void clearErrorMessage()
{
	error_string[0] = '\0';
}

void setErrorMessage(const char* message)
{
	// save off error message in global buffer for CrashReporter to find
	strncpy(error_string, message, sizeof(error_string)-1);
	error_string[sizeof(error_string)-1] = '\0';
}

const char* getErrorMessage()
{
	return error_string;
}

void  halt(const char* message)
{
	fprintf(stderr, "dyld: %s\n", message);
	setErrorMessage(message);
	strncpy(error_string, message, sizeof(error_string)-1);
	error_string[sizeof(error_string)-1] = '\0';
	
#if __ppc__ || __ppc64__
	__asm__  ("trap");
#elif __i386__
	__asm__  ("int3");
#else
	#error unknown architecture
#endif
	abort();	// needed to suppress warning that noreturn function returns
}


uintptr_t bindLazySymbol(const mach_header* mh, uintptr_t* lazyPointer)
{
	uintptr_t result = 0;
	// acquire read-lock on dyld's data structures
#if 0 // rdar://problem/3811777 turn off locking until deadlock is resolved
	if ( gThreadHelpers != NULL ) 
		(*gThreadHelpers->lockForReading)();
#endif
	// lookup and bind lazy pointer and get target address
	try {
		ImageLoader* target;
	#if __i386__
		// fast stubs pass NULL for mh and image is instead found via the location of stub (aka lazyPointer)
		if ( mh == NULL )
			target = dyld::findImageContainingAddressThreadSafe(lazyPointer);
		else
			target = dyld::findImageByMachHeader(mh);
	#else
		// note, target should always be mach-o, because only mach-o lazy handler wired up to this
		target = dyld::findImageByMachHeader(mh);
	#endif
		if ( target == NULL )
			throw "image not found for lazy pointer";
		result = target->doBindLazySymbol(lazyPointer, gLinkContext);
	}
	catch (const char* message) {
		fprintf(stderr, "dyld: lazy symbol binding failed: %s\n", message);
		halt(message);
	}
	// release read-lock on dyld's data structures
#if 0
	if ( gThreadHelpers != NULL ) 
		(*gThreadHelpers->unlockForReading)();
#endif
	// return target address to glue which jumps to it with real parameters restored
	return result;
}


// SPI used by ZeroLink to lazy load bundles
void registerZeroLinkHandlers(BundleNotificationCallBack notify, BundleLocatorCallBack locate)
{
	sBundleNotifier = notify;
	sBundleLocation = locate;
}

void registerUndefinedHandler(UndefinedHandler handler)
{
	sUndefinedHandler = handler;
}

static void undefinedHandler(const char* symboName)
{
	if ( sUndefinedHandler != NULL ) {
		(*sUndefinedHandler)(symboName);
	}
}

static bool findExportedSymbol(const char* name, bool onlyInCoalesced, const ImageLoader::Symbol** sym, ImageLoader** image)
{
	// try ZeroLink short cut to finding bundle which exports this symbol
	if ( sBundleLocation != NULL ) {
		ImageLoader* zlImage = (*sBundleLocation)(name);
		if ( zlImage == ((ImageLoader*)(-1)) ) {
			// -1 is magic value that request symbol is in a bundle not yet linked into process
			// try calling handler to link in that symbol
			undefinedHandler(name);
			// call locator again
			zlImage = (*sBundleLocation)(name);
		}
		// if still not found, then ZeroLink has no idea where to find it
		if ( zlImage == ((ImageLoader*)(-1)) ) 
			return false;
		if ( zlImage != NULL ) {
			// ZeroLink cache knows where the symbol is
			*sym = zlImage->findExportedSymbol(name, NULL, false, image);
			if ( *sym != NULL ) {
				*image = zlImage;
				return true;
			}
		}
		else {
			// ZeroLink says it is in some bundle already loaded, but not linked, walk them all
			const unsigned int imageCount = sAllImages.size();
			for(unsigned int i=0; i < imageCount; ++i){
				ImageLoader* anImage = sAllImages[i];
				if ( anImage->isBundle() && !anImage->hasHiddenExports() ) {
					//fprintf(stderr, "dyld: search for %s in %s\n", name, anImage->getPath());
					*sym = anImage->findExportedSymbol(name, NULL, false, image);
					if ( *sym != NULL ) {
						return true;
					}
				}
			}
		}
	}

	// search all images in order
	ImageLoader* firstWeakImage = NULL;
	const ImageLoader::Symbol* firstWeakSym = NULL;
	const unsigned int imageCount = sAllImages.size();
	for(unsigned int i=0; i < imageCount; ++i){
		ImageLoader* anImage = sAllImages[i];
		if ( ! anImage->hasHiddenExports() && (!onlyInCoalesced || anImage->hasCoalescedExports()) ) {
			*sym = anImage->findExportedSymbol(name, NULL, false, image);
			if ( *sym != NULL ) {
				// if weak definition found, record first one found
				if ( ((*image)->getExportedSymbolInfo(*sym) & ImageLoader::kWeakDefinition) != 0 ) {
					if ( firstWeakImage == NULL ) {
						firstWeakImage = *image;
						firstWeakSym = *sym;
					}
				}
				else {
					// found non-weak, so immediately return with it
					return true;
				}
			}
		}
	}
	if ( firstWeakSym != NULL ) {
		// found a weak definition, but no non-weak, so return first weak found
		*sym = firstWeakSym;
		*image = firstWeakImage;
		return true;
	}
	
	return false;
}

bool flatFindExportedSymbol(const char* name, const ImageLoader::Symbol** sym, ImageLoader** image)
{
	return findExportedSymbol(name, false, sym, image);
}

bool findCoalescedExportedSymbol(const char* name, const ImageLoader::Symbol** sym, ImageLoader** image)
{
	return findExportedSymbol(name, true, sym, image);
}


bool flatFindExportedSymbolWithHint(const char* name, const char* librarySubstring, const ImageLoader::Symbol** sym, ImageLoader** image)
{
	// search all images in order
	const unsigned int imageCount = sAllImages.size();
	for(unsigned int i=0; i < imageCount; ++i){
		ImageLoader* anImage = sAllImages[i];
		// only look at images whose paths contain the hint string (NULL hint string is wildcard)
		if ( ! anImage->isBundle() && ((librarySubstring==NULL) || (strstr(anImage->getPath(), librarySubstring) != NULL)) ) {
			*sym = anImage->findExportedSymbol(name, NULL, false, image);
			if ( *sym != NULL ) {
				return true;
			}
		}
	}
	return false;
}

static void getMappedRegions(ImageLoader::RegionsVector& regions)
{
	const unsigned int imageCount = sAllImages.size();
	for(unsigned int i=0; i < imageCount; ++i){
		ImageLoader* anImage = sAllImages[i];
		anImage->addMappedRegions(regions);
	}
}


static ImageLoader* libraryLocator(const char* libraryName, bool search, const char* origin, const char* rpath[])
{
	dyld::LoadContext context;
	context.useSearchPaths		= search;
	context.useLdLibraryPath	= false;
	context.dontLoad			= false;
	context.mustBeBundle		= false;
	context.mustBeDylib			= true;
	context.matchByInstallName	= false;
	context.origin				= origin;
	context.rpath				= rpath;
	return load(libraryName, context);
}

	
static void setContext(int argc, const char* argv[], const char* envp[], const char* apple[])
{
	gLinkContext.loadLibrary			= &libraryLocator;
	gLinkContext.imageNotification		= &imageNotification;
	gLinkContext.terminationRecorder	= &terminationRecorder;
	gLinkContext.flatExportFinder		= &flatFindExportedSymbol;
	gLinkContext.coalescedExportFinder	= &findCoalescedExportedSymbol;
	gLinkContext.undefinedHandler		= &undefinedHandler;
	gLinkContext.addImageNeedingNotification = &addImageNeedingNotification;
	gLinkContext.notifyAdding			= &notifyAdding;
	gLinkContext.getAllMappedRegions	= &getMappedRegions;
	gLinkContext.bindingHandler			= NULL;
	gLinkContext.bindingOptions			= ImageLoader::kBindingNone;
	gLinkContext.mainExecutable			= sMainExecutable;
	gLinkContext.argc					= argc;
	gLinkContext.argv					= argv;
	gLinkContext.envp					= envp;
	gLinkContext.apple					= apple;
}

static bool checkEmulation()
{
#if __i386__
	int mib[] = { CTL_KERN, KERN_CLASSIC, getpid() };
	int is_classic = 0;
	size_t len = sizeof(int);
	int ret = sysctl(mib, 3, &is_classic, &len, NULL, 0);
	if ((ret != -1) && is_classic) {
		// When a 32-bit ppc program is run under emulation on an Intel processor,
		// we want any i386 dylibs (e.g. the emulator) to not load in the shared region
		// because the shared region is being used by ppc dylibs
		gLinkContext.sharedRegionMode = ImageLoader::kDontUseSharedRegion;
		return true;
	}
#endif
	return false;
}

void link(ImageLoader* image, ImageLoader::BindingLaziness bindness, ImageLoader::InitializerRunning runInitializers)
{
	// add to list of known images.  This did not happen at creation time for bundles
	if ( image->isBundle() )
		addImage(image);

	// we detect root images as those not linked in yet 
	if ( !image->isLinked() )
		addRootImage(image);
	
	// notify ZeroLink of new image with concat of logical and physical name
	if ( sBundleNotifier != NULL && image->isBundle() ) {
		const int logicalLen = strlen(image->getLogicalPath());
		char logAndPhys[strlen(image->getPath())+logicalLen+2];
		strcpy(logAndPhys, image->getLogicalPath());
		strcpy(&logAndPhys[logicalLen+1], image->getPath());
		(*sBundleNotifier)(logAndPhys, image);
	}

	// process images
	try {
		image->link(gLinkContext, bindness, runInitializers, sAddImageCallbacks.size());
	}
	catch (const char* msg) {
		sAllImagesMightContainUnlinkedImages = true;
		throw msg;
	}
	
#if OLD_GDB_DYLD_INTERFACE
	// notify gdb that loaded libraries have changed
	gdb_dyld_state_changed();
#endif
}


//
// _pthread_keys is partitioned in a lower part that dyld will use; libSystem
// will use the upper part.  We set __pthread_tsd_first to 1 as the start of
// the lower part.  Libc will take #1 and c++ exceptions will take #2.  There
// is one free key=3 left.
//
extern "C" {
	extern int __pthread_tsd_first;
}
 

//
// Entry point for dyld.  The kernel loads dyld and jumps to __dyld_start which
// sets up some registers and call this function.
//
// Returns address of main() in target program which __dyld_start jumps to
//
uintptr_t
_main(const struct mach_header* mainExecutableMH, int argc, const char* argv[], const char* envp[], const char* apple[])
{	
	// set pthread keys to dyld range
	__pthread_tsd_first = 1;
	
	bool isEmulated = checkEmulation();
	// Pickup the pointer to the exec path.
	sExecPath = apple[0];
	if (isEmulated) {
		// under Rosetta 
		sExecPath = strdup(apple[0] + strlen(apple[0]) + 1);
	}
	if ( sExecPath[0] != '/' ) {
		// have relative path, use cwd to make absolute
		char cwdbuff[MAXPATHLEN];
	    if ( getcwd(cwdbuff, MAXPATHLEN) != NULL ) {
			// maybe use static buffer to avoid calling malloc so early...
			char* s = new char[strlen(cwdbuff) + strlen(sExecPath) + 2];
			strcpy(s, cwdbuff);
			strcat(s, "/");
			strcat(s, sExecPath);
			sExecPath = s;
		}
	}
	uintptr_t result = 0;
	sMainExecutableMachHeader = mainExecutableMH;
	if ( issetugid() )
		pruneEnvironmentVariables(envp, &apple);
	else
		checkEnvironmentVariables(envp, isEmulated);
	if ( sEnv.DYLD_PRINT_OPTS ) 
		printOptions(argv);
	if ( sEnv.DYLD_PRINT_ENV ) 
		printEnvironmentVariables(envp);
	getHostInfo();
	setContext(argc, argv, envp, apple);
	ImageLoader::BindingLaziness bindness = sEnv.DYLD_BIND_AT_LAUNCH ? ImageLoader::kLazyAndNonLazy : ImageLoader::kNonLazyOnly;
	
	// load any inserted libraries before loading the main executable so that they are first in flat namespace
	int insertLibrariesCount = 0;
	if ( sEnv.DYLD_INSERT_LIBRARIES != NULL ) {
		for (const char* const* lib = sEnv.DYLD_INSERT_LIBRARIES; *lib != NULL; ++lib) {
			insertLibrariesCount++;
		}
	}
	ImageLoader* insertedImages[insertLibrariesCount];
	if ( insertLibrariesCount > 0 ) {
		for (int i=0; i < insertLibrariesCount; ++i) {
			try {
				LoadContext context;
				context.useSearchPaths		= false;
				context.useLdLibraryPath	= false;
				context.dontLoad			= false;
				context.mustBeBundle		= false;
				context.mustBeDylib			= true;
				context.matchByInstallName	= false;
				context.origin				= NULL;	// can't use @loader_path with DYLD_INSERT_LIBRARIES
				context.rpath				= NULL;
				insertedImages[i] = load(sEnv.DYLD_INSERT_LIBRARIES[i], context);
			}
			catch (...) {
				char buf[strlen(sEnv.DYLD_INSERT_LIBRARIES[i])+50];
				sprintf(buf, "could not load inserted library: %s\n", sEnv.DYLD_INSERT_LIBRARIES[i]);
				insertedImages[i]  = NULL;
				halt(buf);
			}
		}
	}
	
	// load and link main executable
	try {
		sMainExecutable = instantiateFromLoadedImage(mainExecutableMH, sExecPath);
		gLinkContext.mainExecutable = sMainExecutable;
		if ( sMainExecutable->forceFlat() ) {
			gLinkContext.bindFlat = true;
			gLinkContext.prebindUsage = ImageLoader::kUseNoPrebinding;
		}
		link(sMainExecutable, bindness, ImageLoader::kDontRunInitializers);
		result = (uintptr_t)sMainExecutable->getMain();
	}
	catch(const char* message) {
		halt(message);
	}
	catch(...) {
		fprintf(stderr, "dyld: launch failed\n");
	}
	
	// Link in any inserted libraries.  
	// Do this after link main executable so any extra libraries pulled in by inserted libraries are at end of flat namespace
	if ( insertLibrariesCount > 0 ) {
		for (int i=0; i < insertLibrariesCount; ++i) {
			try {
				if ( insertedImages[i] != NULL )
					link(insertedImages[i], bindness, ImageLoader::kDontRunInitializers);
			}
			catch (const char* message) {
				char buf[strlen(sEnv.DYLD_INSERT_LIBRARIES[i])+50+strlen(message)];
				sprintf(buf, "could not link inserted library: %s\n%s\n", sEnv.DYLD_INSERT_LIBRARIES[i], message);
				halt(buf);
			}
		}
	}
	
	return result;
}




}; // namespace



