#ifndef __MFC_MEM_LEAK_DETECTION_H__
#define __MFC_MEM_LEAK_DETECTION_H__

#if defined(WIN32) && defined(_DEBUG)
#define new new(THIS_FILE, __LINE__)
#define malloc(s) _malloc_dbg(s, _NORMAL_BLOCK, __FILE__, __LINE__)
#endif

#endif