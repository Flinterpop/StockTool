// Single place that includes GDI+ correctly under WIN32_LEAN_AND_MEAN/NOMINMAX.
#pragma once

#include <windows.h>
#include <objidl.h>  // IStream/PROPID for gdiplus.h (excluded by WIN32_LEAN_AND_MEAN)

#include <algorithm>
using std::max;  // gdiplus.h expects min/max in scope (NOMINMAX is defined)
using std::min;
#pragma warning(push, 1)
#include <gdiplus.h>
#pragma warning(pop)
