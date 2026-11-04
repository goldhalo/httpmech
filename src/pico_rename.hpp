// =============================================================================
//  httpmech - 内部：vendored picohttpparser 的符号改名
// =============================================================================
//  为什么需要这个文件
//  ---------------------------------------------------------------------------
//  picohttpparser 是 C 代码，函数是**全局 C 符号**（`phr_parse_request` 等，
//  头里有 `extern "C"` 守卫，换成 C++ 编译也还是这个名字）。
//
//  一旦把它编进本库（而不是单独出 libpicohttpparser.a），就会出现新的风险：
//  同一进程里 httpmech 和 rtspmech 各带一份 → 链接期 multiple definition。
//  所以在这里统一加前缀 `httpmech_`，两边各用各的名字，谁也不撞谁。
//
//  用法：在**任何**包含 picohttpparser 的 .c/.h 之前先包含本文件。
//  （目前只有 src/pico_impl.cpp 和 src/parser.cpp 两处）
// =============================================================================
#pragma once

#define phr_parse_request              httpmech_phr_parse_request
#define phr_parse_response             httpmech_phr_parse_response
#define phr_parse_headers              httpmech_phr_parse_headers
#define phr_decode_chunked             httpmech_phr_decode_chunked
#define phr_decode_chunked_is_in_data  httpmech_phr_decode_chunked_is_in_data
