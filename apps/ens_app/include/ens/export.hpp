#pragma once

// SHARED 模块符号导出宏（ENS-DEV-ARCH §3.7）
// 仅 ens::channel / ens::business 使用；STATIC 模块（protocol/datahub/ui）严禁引入。
//
// 用法：在 SHARED 库自身的 CMakeLists 里定义 ENS_BUILD_SHARED，
// 再让该库的源文件 #include <ens/export.hpp> 并使用 ENS_CHANNEL_API / ENS_BUSINESS_API。

#if defined(_WIN32)
  #ifdef ENS_BUILD_SHARED
    #define ENS_API __declspec(dllexport)
  #else
    #define ENS_API __declspec(dllimport)
  #endif
#else
  #define ENS_API __attribute__((visibility("default")))
#endif

// 各层专用别名（后续层实现时改用对应宏即可）
#define ENS_CHANNEL_API  ENS_API
#define ENS_BUSINESS_API ENS_API
