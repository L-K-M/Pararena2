/* AppleTalk.h -- shim stand-in for the THINK C system header */
#include "MacShim.h"
/* AddrBlock for AppleTalkDDP.h */
#ifndef SHIM_APPLETALK_TYPES
#define SHIM_APPLETALK_TYPES
typedef struct AddrBlock { short aNet; unsigned char aNode; unsigned char aSocket; } AddrBlock;
#endif
