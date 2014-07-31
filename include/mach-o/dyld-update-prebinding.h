
#ifndef _DYLD_UPDATE_PREBINDING_H_
#define _DYLD_UPDATE_PREBINDING_H_

#define UPDATE_PREBINDING_DRY_RUN		0x00000001
#define UPDATE_PREBINDING_PROGRESS		0x00000002
#define UPDATE_PREBINDING_DEBUG			0x00000004
#define UPDATE_PREBINDING_FORCE			0x00000008
#define UPDATE_PREBINDING_XBUILD_LOG	0x00000010
#define UPDATE_PREBINDING_SORT			0x00000020



typedef void (*update_prebinding_ptr)(int pathCount, const char* paths[], uint32_t flags) __attribute__((noreturn));




#endif
