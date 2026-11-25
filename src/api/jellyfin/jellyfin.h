#ifndef JELLYFIN_H
#define JELLYFIN_H

#include "config.h"

namespace jellyfin {

// 把 Jellyfin 兼容路由挂到进程内共享的 Drogon app 上（监听 cfg.jellyfin_port）。
// 必须在 HTTP 服务 run() 之前调用；本函数不阻塞。
void mount(const config::Config &cfg);

} // namespace jellyfin

#endif