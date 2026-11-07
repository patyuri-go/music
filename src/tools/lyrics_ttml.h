#ifndef TOOLS_LYRICS_TTML_H
#define TOOLS_LYRICS_TTML_H

#include "media_library/domain.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace media_library {

// 调用方可选提供歌曲元数据；拿不到就留空，转换时直接省略，不编造。
struct LyricsTtmlMeta {
  std::string music_name;
  std::string album;
  std::vector<std::string> artists;
  // 整首歌时长：用于最后一行 end 与 body dur 的回退。
  std::optional<std::int64_t> duration_ms;
};

// 把库内歌词领域模型转换成 AMLL TTML DB 规范的标准 TTML 文本。
//
// 转换规则：
//   - 逐字：任一行带 words 时整首按 itunes:timing="Word" 输出，每个音节
//     一个带 begin/end 的 <span>；
//   - 逐行 / 无时间：其余情况按 itunes:timing="Line" 输出，<p> 内直接放文本；
//   - 行 end：优先用领域模型的 end，其次下一行 start，最后整首时长；
//   - 演唱者：领域模型没有演唱者信息，统一用单个 agent v1（规范要求 <p>
//     上的 ttm:agent 必填）；
//   - 辅助轨：译文/注音默认挂成行内 x-translation / x-roman；若辅助轨本身
//     逐字（aux.words 非空），改挂 head 的 iTunesMetadata，与规范里逐字
//     翻译/音译的形态一致；语言码拿不到时不写；
//   - amll:meta：只写调用方给到的 musicName / artists / album。
std::string lyrics_to_ttml(const Lyrics &lyrics,
                           const LyricsTtmlMeta &meta = {});

} // namespace media_library

#endif // TOOLS_LYRICS_TTML_H
