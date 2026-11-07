#ifndef TOOLS_LYRICS_TTML_H
#define TOOLS_LYRICS_TTML_H

#include "media_library/domain.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace media_library {

// 转换 TTML 时可选写入 head 的歌曲元数据。领域模型里没有这些字段，
// 由调用方（路由层）从 Track 取；拿不到就留空，不编造。
struct LyricsTtmlMeta {
  std::string music_name;
  std::string album;
  std::vector<std::string> artists;
  std::optional<std::int64_t> duration_ms; // 行/词 end 与 body dur 的最后回退
};

// 把库内歌词领域模型转换成 AMLL TTML DB 规范的标准 TTML 文本。
//
// 领域模型本身就是有序的 LyricItem 列表，这里做 1:1 映射：
//   Text -> Text、Word -> Word、Ruby -> Ruby、Background -> Background、
//   Translation -> x-translation、Romanization -> x-roman；子项递归。
//   - itunes:timing：modes 含 WordSynced 时 "Word"，否则 "Line"；
//   - 演唱者：领域模型没有演唱者信息，统一用单个 agent v1（规范要求
//     <p> 上的 ttm:agent 必填）；
//   - end 缺失时按「自身 end -> 下一个同类时间 -> 行 end -> 下一行 start
//     -> meta.duration_ms」回退；
//   - amll:meta 只写调用方给到的 musicName / artists / album；
//   - 语言码数据源没有就不写，不猜。
std::string lyrics_to_ttml(const Lyrics &lyrics,
                           const LyricsTtmlMeta &meta = {});

} // namespace media_library

#endif // TOOLS_LYRICS_TTML_H
