#ifndef MEDIA_LIBRARY_LIBRARY_RUNTIME_H
#define MEDIA_LIBRARY_LIBRARY_RUNTIME_H

#include "media_library/local_library.h"

#include <memory>
#include <string>

namespace media_library {

// 进程内唯一的本地库对象。
//
// 5666 主服务和 9000 Jellyfin 兼容层必须共用同一个实例，避免各自加载
// 一套元数据、各自维护一套 entity_mapping 缓存。
//
// 初始化只在 main() 启动阶段调用一次；初始化后保持只读，可被多个线程并发读取。
void initialize_library(std::string library_id, std::string display_name,
                        std::string db_path);

bool library_initialized() noexcept;

// 已初始化时返回库对象，否则返回空指针（空的 shared_ptr）。
std::shared_ptr<LocalLibrary> library_ptr() noexcept;

// 已初始化时返回库对象引用；未初始化是程序错误，调用前应先检查
// library_initialized()。
LocalLibrary &library();

// 进程退出前释放库对象。
void shutdown_library() noexcept;

} // namespace media_library

#endif // MEDIA_LIBRARY_LIBRARY_RUNTIME_H
