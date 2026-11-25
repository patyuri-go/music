#ifndef SUBSONIC_SUBSONIC_H
#define SUBSONIC_SUBSONIC_H

#include "config.h"

namespace subsonic {

// 把 Subsonic 兼容路由挂到进程内共享的 Drogon app 上（监听 cfg.subsonic.port）。
// 必须在 HTTP 服务 run() 之前调用；本函数不阻塞。
//
// 只经 MediaRouter / PlaylistDirectory / stream_http 取数据：不持有具体库类型、
// 不读目录快照、不遍历注册表取数据。远程媒体库永不全量扫描，因此一切「列举」
// 类端点只覆盖声明了 catalog_list 的库。
//
// 不做账号校验：u / t / s / p 一律忽略。
void mount(const config::Config &cfg);

} // namespace subsonic

#endif // SUBSONIC_SUBSONIC_H