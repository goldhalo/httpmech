// =============================================================================
//  httpmech - 把 vendored picohttpparser 编进本库
// =============================================================================
//  以前它是独立的 `libpicohttpparser.a`：使用者链接 httpmech 时还得额外
//  `-lpicohttpparser`，安装目录里也会多出 `include/httpmech/thirdparty/`。
//  可是 picohttpparser 完全是**实现细节**（只有 parser.cpp 用），没理由让
//  使用方为它买单。
//
//  现在直接把 .c 作为本库的一个编译单元，配合 pico_rename.hpp 的符号前缀，
//  做到：
//    · 发布物只有 libhttpmech.a 一个库文件
//    · 公开头目录里没有 picohttpparser.h
//    · 与 rtspmech 各自内嵌时不会撞符号（前缀不同）
//
//  ★ 为什么把 .c 当 C++ 编
//    picohttpparser 是干净的 C，直接用 C++ 编译器编没问题（上游也这么建议）。
//    这样就不需要在一个 target 里混 C 和 C++ 源文件，CMake 侧更简单。
// =============================================================================
#include "pico_rename.hpp"

#include "../thirdparty/picohttpparser/picohttpparser.c"
