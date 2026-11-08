// =============================================================================
//  httpmech/export.hpp —— 共享库导出宏
// =============================================================================
//  cmake -DHTTPMECH_LIBRARY_TYPE=SHARED：
//    · 库本身定义 HTTPMECH_BUILDING_LIBRARY → dllexport
//    · 使用方定义 HTTPMECH_SHARED → dllimport（Windows）
//  Linux 配合 CXX_VISIBILITY_PRESET=hidden，只导出标了 HTTPMECH_API 的符号。
//  STATIC（默认）展开为空。
// =============================================================================
#pragma once

#if defined(_WIN32) && defined(HTTPMECH_SHARED)
#  if defined(HTTPMECH_BUILDING_LIBRARY)
#    define HTTPMECH_API __declspec(dllexport)
#  else
#    define HTTPMECH_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) && defined(HTTPMECH_SHARED)
#  define HTTPMECH_API __attribute__((visibility("default")))
#else
#  define HTTPMECH_API
#endif
