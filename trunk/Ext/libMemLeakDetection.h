#ifndef __LIB_MEM_LEAK_DETECTION_H__
#define __LIB_MEM_LEAK_DETECTION_H__

#if defined(WIN32) && defined(_DEBUG)
	#define _CRTDBG_MAP_ALLOC
	#include <stdlib.h>
	#include <crtdbg.h>

	#define new new(__FILE__, __LINE__)
	#define malloc(s) _malloc_dbg(s, _NORMAL_BLOCK, __FILE__, __LINE__)
#endif

#endif