#ifndef JELLYFIN_H
#define JELLYFIN_H

#include "config.h"

namespace jellyfin {

// 在独立线程中启动 Jellyfin 兼容服务（监听 cfg.jellyfin_port），阻塞运行。
void run(const config::Config &cfg);

} // namespace jellyfin

#endif
