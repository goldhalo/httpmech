// =============================================================================
//  httpmech - 汇总头
// =============================================================================
//  文件    : httpmech/httpmech.hpp
//  用途    : 一次性引入全部公开接口。只想用服务端的可以只包含 server.hpp
//            （会顺带引入 message/headers/common，但不会拖 client）。
//  编码    : UTF-8
// =============================================================================
#pragma once

#include <httpmech/common.hpp>
#include <httpmech/span.hpp>
#include <httpmech/headers.hpp>
#include <httpmech/message.hpp>
#include <httpmech/parser.hpp>
#include <httpmech/websocket.hpp>
#include <httpmech/server.hpp>
#include <httpmech/router.hpp>

// 客户端是 v0.2 的范围，进来只为让接口形状一起评审；v0.1 不实现。
#include <httpmech/client.hpp>

// 版本号与 httpmech 的 CMake project(VERSION) 保持一致
#define HTTPMECH_VERSION_MAJOR 0
#define HTTPMECH_VERSION_MINOR 1
#define HTTPMECH_VERSION_PATCH 0
#define HTTPMECH_VERSION_STRING "0.1.0"
