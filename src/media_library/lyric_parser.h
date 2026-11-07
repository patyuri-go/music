#ifndef MEDIA_LIBRARY_LYRIC_PARSER_H
#define MEDIA_LIBRARY_LYRIC_PARSER_H

#include "media_library/domain.h"

#include <optional>
#include <string>
#include <vector>

namespace media_library {

// ---------------------------------------------------------------------------
// 歌词文本 → 领域模型
//
// 媒体库层是唯一的解析方：远端平台与本地 .lrc / 内嵌 LYRICS 都只交出
// 原始文本，各库不再自己拼 LyricLine。这样「同一份歌词从哪来」不影响
// 上层看到的行/词/辅助轨结构。
//
// 支持的文本形态：
//   - LRC        [mm:ss.xxx]正文
//   - 增强 LRC   <mm:ss.xxx> 内联时间（按词切分）
//   - YRC        [行起始,行时长](词起始,词时长,0)词…（逐字同步，网易云）
//   - 多行时间标签 [t1][t2]正文（同一句重复出现在多个时间点）
//   - 双语       原文与译文各自一条轨，按行时间对齐后挂到 line.items
//   - 注音       romalrc / ruby 轨，同样按行时间对齐后挂到 line.items
// ---------------------------------------------------------------------------

// 一条按时间排好序的文本轨（LRC / 译文 / 注音都是这个形状）。
struct LyricTrackLine {
  std::int64_t start_ms = 0;
  std::string text;
};

// 解析 LRC 文本。返回的行已按 start_ms 升序排列；
// 以 '[' 开头但时间标签无法解析的行（例如平台混进来的 JSON 人员行）被跳过。
std::vector<LyricTrackLine> parse_lrc_track(const std::string &text);

// 解析 YRC 文本（逐字同步）。无法解析的行被跳过。
std::vector<LyricLine> parse_yrc_track(const std::string &text);

// 解析带内联时间标签的增强 LRC（<mm:ss.xxx>word…）。
std::vector<LyricLine> parse_enhanced_lrc_track(const std::string &text);

// 把一条文本轨按行时间对齐到已解析的行上，作为辅助轨追加。
// 空文本行是平台的间奏占位，必须保留：主行会补 `······`，副轨保留空串，
// 用来阻止客户端沿用上一句；对齐不上的行丢弃（不猜时间）。
void attach_aux_track(std::vector<LyricLine> &lines,
                      const std::vector<LyricTrackLine> &track,
                      LyricItemKind kind);

// 从已解析的行重建纯文本（无时间轴时的降级表示）。
std::string plain_text_of(const std::vector<LyricLine> &lines);

// 判断一段文本是否含可解析的时间标签（用于区分「同步」与「纯文本」）。
bool looks_time_tagged(const std::string &text);

// 本地库的统一入口：一段原始歌词文本 → Lyrics（无辅助轨）。
// 有可解析时间标签则 LineSynced，否则 Unsynced（清掉所有时间字段）。
// 带时间轴的空行（间奏）只在持续超过 3 秒时补 `······`，否则删除。
Lyrics parse_plain_lyrics(const TrackRef &track, const std::string &text);

// 远端平台的统一入口：正文 + 可选辅助轨 → Lyrics。
//   body        LRC 或 YRC 文本（YRC 优先，逐字同步）
//   translation 译文轨（LRC，可为空）
//   romanization 注音轨（LRC，可为空）
// 正文的空行（间奏）只在持续超过 3 秒时补 `······`，否则删除；
// 辅助轨只保留空串占位。
Lyrics parse_platform_lyrics(const TrackRef &track, const std::string &body,
                             const std::string &translation,
                             const std::string &romanization);

} // namespace media_library

#endif // MEDIA_LIBRARY_LYRIC_PARSER_H
