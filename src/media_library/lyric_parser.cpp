#include "media_library/lyric_parser.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace media_library {
namespace {

std::string trim_cr(std::string line) {
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
    line.pop_back();
  }
  return line;
}

// "mm:ss" / "mm:ss.xx" / "mm:ss.xxx" / "mm:ss:xx"（部分平台用冒号分隔小数）
bool parse_clock(const std::string &text, std::int64_t &out_ms) {
  const std::size_t colon = text.find(':');
  if (colon == std::string::npos || colon == 0) {
    return false;
  }
  const std::string minutes = text.substr(0, colon);
  const std::string rest = text.substr(colon + 1);
  if (minutes.empty() || rest.empty()) {
    return false;
  }
  for (const char c : minutes) {
    if (c < '0' || c > '9') {
      return false;
    }
  }

  // 秒部分可以是 "ss"、"ss.xx"、"ss:xx"。
  const std::size_t dot = rest.find_first_of(".:");
  const std::string seconds = dot == std::string::npos ? rest : rest.substr(0, dot);
  if (seconds.empty()) {
    return false;
  }
  for (const char c : seconds) {
    if (c < '0' || c > '9') {
      return false;
    }
  }

  std::int64_t frac_ms = 0;
  if (dot != std::string::npos) {
    std::string digits;
    for (std::size_t i = dot + 1; i < rest.size(); ++i) {
      if (std::isdigit(static_cast<unsigned char>(rest[i])) == 0) {
        return false;
      }
      digits.push_back(rest[i]);
    }
    if (digits.size() > 3) {
      digits = digits.substr(0, 3);
    }
    while (digits.size() < 3) {
      digits.push_back('0');
    }
    if (!digits.empty()) {
      frac_ms = std::stoll(digits);
    }
  }

  out_ms = std::stoll(minutes) * 60000 + std::stoll(seconds) * 1000 + frac_ms;
  return true;
}

// 取一行开头的全部 [ ... ] 标签：时间标签收集进 times，offset 标签折算进 offset_ms。
// 返回标签之后正文的起始位置；没有时间标签时返回 npos。
std::size_t parse_leading_tags(const std::string &raw,
                               std::vector<std::int64_t> &times,
                               std::int64_t &offset_ms) {
  std::size_t pos = 0;
  while (pos < raw.size() && raw[pos] == '[') {
    const std::size_t close = raw.find(']', pos);
    if (close == std::string::npos) {
      return std::string::npos;
    }
    const std::string tag = raw.substr(pos + 1, close - pos - 1);
    if (tag.rfind("offset:", 0) == 0) {
      try {
        offset_ms = std::stoll(tag.substr(7));
      } catch (const std::exception &) {
        offset_ms = 0;
      }
    } else {
      std::int64_t value = 0;
      if (parse_clock(tag, value)) {
        times.push_back(value);
      }
    }
    pos = close + 1;
  }
  return times.empty() ? std::string::npos : pos;
}

// 网易云会把人声/演唱者信息塞进 lrc 正文，混在 LRC 行里：
//   {"t":0,"c":[{"tx":"作词: "},{"tx":"ビートまりお"}]}
//   {"t":178,"c":[{"tx":"作曲: "},{"tx":"ZUN"}]}
// 这类行既不是时间标签行也不是正文，解析时必须原样保留成带时间戳的
// 文本行，否则 `_BREAK_` 会挂到错误的行上（歌词整体错位一句）。
//
// 返回 true 时 out_ms / out_text 已填好。
bool parse_netease_credit_line(const std::string &raw, std::int64_t &out_ms,
                               std::string &out_text) {
  if (raw.empty() || raw.front() != '{') {
    return false;
  }
  const std::string tkey = "\"t\":";
  const std::size_t tpos = raw.find(tkey);
  if (tpos == std::string::npos) {
    return false;
  }
  std::size_t i = tpos + tkey.size();
  while (i < raw.size() && raw[i] == ' ') {
    ++i;
  }
  std::string digits;
  while (i < raw.size() && raw[i] >= '0' && raw[i] <= '9') {
    digits.push_back(raw[i]);
    ++i;
  }
  if (digits.empty()) {
    return false;
  }
  std::int64_t ms = 0;
  try {
    ms = std::stoll(digits);
  } catch (const std::exception &) {
    return false;
  }

  // 收集所有 "tx":"..." 片段，按出现顺序拼接（"tx" 里不含转义双引号）。
  std::string text;
  const std::string txkey = "\"tx\":\"";
  std::size_t pos = 0;
  while (true) {
    const std::size_t at = raw.find(txkey, pos);
    if (at == std::string::npos) {
      break;
    }
    const std::size_t begin = at + txkey.size();
    const std::size_t end = raw.find('"', begin);
    if (end == std::string::npos) {
      break;
    }
    text += raw.substr(begin, end - begin);
    pos = end + 1;
  }
  if (text.empty()) {
    return false;
  }
  out_ms = ms;
  out_text = std::move(text);
  return true;
}

std::vector<std::string> split_lines(const std::string &text) {
  std::vector<std::string> out;
  std::size_t begin = 0;
  while (begin <= text.size()) {
    const std::size_t end = text.find('\n', begin);
    out.push_back(trim_cr(text.substr(
        begin, end == std::string::npos ? std::string::npos : end - begin)));
    if (end == std::string::npos) {
      break;
    }
    begin = end + 1;
  }
  return out;
}

// 增强 LRC 的内联时间标签：<mm:ss.xxx>。按标签切成词。
void split_inline_words(const std::string &body, LyricLine &line) {
  std::size_t pos = 0;
  while (pos < body.size()) {
    const std::size_t open = body.find('<', pos);
    if (open == std::string::npos) {
      // 尾部残留文本并进上一词，没有上一词则并进正文。
      if (pos < body.size()) {
        if (!line.words.empty()) {
          line.words.back().text += body.substr(pos);
          line.text += body.substr(pos);
        } else {
          line.text += body.substr(pos);
        }
      }
      break;
    }
    const std::size_t close = body.find('>', open);
    if (close == std::string::npos) {
      break;
    }
    const std::string tag = body.substr(open + 1, close - open - 1);
    std::int64_t start = 0;
    if (!parse_clock(tag, start)) {
      pos = close + 1;
      continue;
    }
    const std::size_t next = body.find('<', close + 1);
    const std::string word_text =
        body.substr(close + 1, next == std::string::npos ? std::string::npos
                                                         : next - close - 1);
    LyricWord word;
    word.start_ms = start;
    word.text = word_text;
    if (!line.words.empty()) {
      // 上一个词的结束时间就是下一个词的开始时间。
      line.words.back().end_ms = start;
    }
    line.words.push_back(std::move(word));
    line.text += word_text;
    pos = next == std::string::npos ? body.size() : next;
  }
}

bool is_yrc_head(const std::string &raw) {
  if (raw.empty() || raw.front() != '[') {
    return false;
  }
  const std::size_t close = raw.find(']');
  if (close == std::string::npos) {
    return false;
  }
  return raw.substr(1, close - 1).find(',') != std::string::npos;
}

std::int64_t normalize_start(std::int64_t start, std::int64_t offset_ms) {
  return std::max<std::int64_t>(0, start + offset_ms);
}

// 间奏占位：平台在间奏处给出的是「有时间戳、正文为空」的行，不是点。
// 只有持续超过 3 秒的空行才补 `······`；短空行只是换行/停顿，直接删除。
// 时长优先用行自带的 end_ms（YRC），否则用下一行的开始时间推算；
// 末行没有 end_ms 时无法判断，按“不超过 3 秒”处理并删除。
constexpr const char *kInterludePlaceholder = "······";
constexpr std::int64_t kInterludeMinDurationMs = 3000;

void normalize_interlude_placeholders(std::vector<LyricLine> &lines) {
  std::vector<LyricLine> kept;
  kept.reserve(lines.size());
  for (std::size_t i = 0; i < lines.size(); ++i) {
    LyricLine &line = lines[i];
    if (!line.start_ms.has_value() || !line.text.empty()) {
      kept.push_back(std::move(line));
      continue;
    }
    std::int64_t duration_ms = -1;
    if (line.end_ms.has_value()) {
      duration_ms = *line.end_ms - *line.start_ms;
    } else if (i + 1 < lines.size() && lines[i + 1].start_ms.has_value()) {
      duration_ms = *lines[i + 1].start_ms - *line.start_ms;
    }
    if (duration_ms > kInterludeMinDurationMs) {
      line.text = kInterludePlaceholder;
      kept.push_back(std::move(line));
    }
  }
  lines = std::move(kept);
}

// 行时间对齐：先找完全相同的时间，其次容差内最近的，最后（行数相同时）按序号。
void attach_one(std::vector<LyricLine> &lines,
                const std::vector<LyricTrackLine> &track,
                LyricAuxKind kind) {
  constexpr std::int64_t kToleranceMs = 1200;
  const bool same_count = lines.size() == track.size();
  for (std::size_t i = 0; i < track.size(); ++i) {
    const std::int64_t stamp = track[i].start_ms;
    std::size_t best = lines.size();
    std::int64_t best_delta = -1;
    for (std::size_t j = 0; j < lines.size(); ++j) {
      if (!lines[j].start_ms.has_value()) {
        continue;
      }
      const std::int64_t delta = std::abs(*lines[j].start_ms - stamp);
      if (best_delta < 0 || delta < best_delta) {
        best_delta = delta;
        best = j;
      }
    }
    if (best == lines.size() || best_delta > kToleranceMs) {
      if (!same_count) {
        continue;
      }
      best = i;
    }
    LyricAux aux;
    aux.kind = kind;
    // 空文本也要保留：这是副轨在该时间点的“无内容”占位，
    // 用来阻止客户端把上一句的译文/注音回退套到这里。
    aux.text = track[i].text;
    lines[best].aux.push_back(std::move(aux));
  }
}

} // namespace

std::vector<LyricTrackLine> parse_lrc_track(const std::string &text) {
  std::vector<LyricTrackLine> out;
  std::int64_t offset_ms = 0;
  for (const std::string &raw : split_lines(text)) {
    // 网易云的人声/制作人员行（JSON 形状）优先识别，否则会被当成无时间行丢弃。
    std::int64_t credit_ms = 0;
    std::string credit_text;
    if (parse_netease_credit_line(raw, credit_ms, credit_text)) {
      LyricTrackLine line;
      line.start_ms = credit_ms;
      line.text = std::move(credit_text);
      out.push_back(std::move(line));
      continue;
    }
    std::vector<std::int64_t> times;
    const std::size_t body_at = parse_leading_tags(raw, times, offset_ms);
    if (body_at == std::string::npos) {
      continue;
    }
    const std::string body = raw.substr(body_at);
    // 多行时间标签：同一句在多个时间点重复出现。
    for (const std::int64_t stamp : times) {
      LyricTrackLine line;
      line.start_ms = normalize_start(stamp, offset_ms);
      line.text = body;
      out.push_back(std::move(line));
    }
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const LyricTrackLine &a, const LyricTrackLine &b) {
                     return a.start_ms < b.start_ms;
                   });
  return out;
}

std::vector<LyricLine> parse_yrc_track(const std::string &text) {
  std::vector<LyricLine> out;
  for (const std::string &raw : split_lines(text)) {
    if (!is_yrc_head(raw)) {
      continue;
    }
    const std::size_t close = raw.find(']');
    const std::string head = raw.substr(1, close - 1);
    const std::size_t comma = head.find(',');
    std::int64_t start = 0;
    std::int64_t duration = 0;
    try {
      start = std::stoll(head.substr(0, comma));
      duration = std::stoll(head.substr(comma + 1));
    } catch (const std::exception &) {
      continue;
    }
    LyricLine line;
    line.start_ms = start;
    if (duration > 0) {
      line.end_ms = start + duration;
    }
    std::size_t pos = close + 1;
    while (pos < raw.size() && raw[pos] == '(') {
      const std::size_t paren = raw.find(')', pos);
      if (paren == std::string::npos) {
        break;
      }
      const std::string word_head = raw.substr(pos + 1, paren - pos - 1);
      const std::size_t c1 = word_head.find(',');
      const std::size_t c2 =
          c1 == std::string::npos ? std::string::npos : word_head.find(',', c1 + 1);
      const std::size_t next = raw.find('(', paren + 1);
      const std::string word_text =
          raw.substr(paren + 1, next == std::string::npos ? std::string::npos
                                                          : next - paren - 1);
      if (c1 != std::string::npos && c2 != std::string::npos) {
        try {
          std::int64_t word_start = std::stoll(word_head.substr(0, c1));
          const std::int64_t word_duration =
              std::stoll(word_head.substr(c1 + 1, c2 - c1 - 1));
          // 实测 wordStart 是绝对毫秒；出现相对值时按相对处理。
          if (word_start < start) {
            word_start += start;
          }
          LyricWord word;
          word.start_ms = word_start;
          if (word_duration > 0) {
            word.end_ms = word_start + word_duration;
          }
          word.text = word_text;
          line.words.push_back(std::move(word));
        } catch (const std::exception &) {
          // 单个坏词不影响整行。
        }
      }
      line.text += word_text;
      pos = next == std::string::npos ? raw.size() : next;
    }
    out.push_back(std::move(line));
  }
  return out;
}

std::vector<LyricLine> parse_enhanced_lrc_track(const std::string &text) {
  std::vector<LyricLine> out;
  std::int64_t offset_ms = 0;
  for (const std::string &raw : split_lines(text)) {
    // 人员行（`{"t":0,"c":[{"tx":"作词: "}...]}`）与普通 LRC 行同处一个文本，
    // 这里也要认，否则它会在「正文优先走增强解析」的分支里被丢掉。
    std::int64_t credit_ms = 0;
    std::string credit_text;
    if (parse_netease_credit_line(raw, credit_ms, credit_text)) {
      LyricLine line;
      line.start_ms = credit_ms;
      line.text = std::move(credit_text);
      out.push_back(std::move(line));
      continue;
    }
    std::vector<std::int64_t> times;
    const std::size_t body_at = parse_leading_tags(raw, times, offset_ms);
    if (body_at == std::string::npos) {
      continue;
    }
    const std::string body = raw.substr(body_at);
    for (const std::int64_t stamp : times) {
      LyricLine line;
      line.start_ms = normalize_start(stamp, offset_ms);
      split_inline_words(body, line);
      out.push_back(std::move(line));
    }
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const LyricLine &a, const LyricLine &b) {
                     return a.start_ms.value_or(0) < b.start_ms.value_or(0);
                   });
  return out;
}

void attach_aux_track(std::vector<LyricLine> &lines,
                      const std::vector<LyricTrackLine> &track,
                      LyricAuxKind kind) {
  if (lines.empty() || track.empty()) {
    return;
  }
  attach_one(lines, track, kind);
}

std::string plain_text_of(const std::vector<LyricLine> &lines) {
  std::string out;
  for (const LyricLine &line : lines) {
    if (!out.empty()) {
      out.push_back('\n');
    }
    out += line.text;
  }
  return out;
}

bool looks_time_tagged(const std::string &text) {
  for (const std::string &raw : split_lines(text)) {
    std::vector<std::int64_t> times;
    std::int64_t offset_ms = 0;
    if (parse_leading_tags(raw, times, offset_ms) != std::string::npos) {
      return true;
    }
  }
  return false;
}

Lyrics parse_plain_lyrics(const TrackRef &track, const std::string &text) {
  Lyrics lyrics;
  lyrics.track = track;
  lyrics.plain_text = text;
  if (text.empty()) {
    return lyrics;
  }

  std::vector<LyricLine> lines;
  if (looks_time_tagged(text)) {
    // 本地 .lrc 也可能带内联时间（增强 LRC），与远端走同一套解析。
    for (const LyricLine &entry : parse_enhanced_lrc_track(text)) {
      const bool dup_time =
          !lines.empty() && lines.back().start_ms.has_value() &&
          entry.start_ms.has_value() && *lines.back().start_ms == *entry.start_ms;
      // 本地双语 .lrc 的常见写法：同一时间戳连着两行（原文、译文）。
      // 时间完全相同的第二行不是新的一句，挂成第一行的译文辅助轨。
      if (dup_time && lines.back().aux.empty() &&
          lines.back().text != entry.text) {
        LyricAux aux;
        aux.kind = LyricAuxKind::Translation;
        aux.text = entry.text;
        lines.back().aux.push_back(std::move(aux));
        continue;
      }
      lines.push_back(entry);
    }
    // 本地带时间轴的歌词同样处理间奏：超过 3 秒补占位，短空行删除。
    normalize_interlude_placeholders(lines);
    // 纯文本视图与处理后的行保持一致，避免它还保留被删掉的空行。
    lyrics.plain_text = plain_text_of(lines);
    const bool any_words = std::any_of(
        lines.begin(), lines.end(),
        [](const LyricLine &line) { return !line.words.empty(); });
    lyrics.mode = lines.empty()
                      ? LyricsMode::Unsynced
                      : (any_words ? LyricsMode::WordSynced
                                   : LyricsMode::LineSynced);
  } else {
    for (const std::string &raw : split_lines(text)) {
      LyricLine line;
      line.text = raw;
      lines.push_back(std::move(line));
    }
    lyrics.mode = LyricsMode::Unsynced;
  }

  lyrics.lines = std::move(lines);
  return lyrics;
}

Lyrics parse_platform_lyrics(const TrackRef &track, const std::string &body,
                             const std::string &translation,
                             const std::string &romanization) {
  Lyrics lyrics;
  lyrics.track = track;

  std::vector<LyricLine> lines = parse_yrc_track(body);
  if (!lines.empty()) {
    lyrics.mode = LyricsMode::WordSynced;
  } else {
    lines = parse_enhanced_lrc_track(body);
    if (!lines.empty() && !lines.front().words.empty()) {
      lyrics.mode = LyricsMode::WordSynced;
    } else if (!lines.empty()) {
      lyrics.mode = LyricsMode::LineSynced;
    } else {
      for (const LyricTrackLine &entry : parse_lrc_track(body)) {
        LyricLine line;
        line.start_ms = entry.start_ms;
        line.text = entry.text;
        lines.push_back(std::move(line));
      }
      lyrics.mode = lines.empty() ? LyricsMode::Unsynced : LyricsMode::LineSynced;
    }
  }

  if (!translation.empty()) {
    attach_aux_track(lines, parse_lrc_track(translation),
                     LyricAuxKind::Translation);
  }
  if (!romanization.empty()) {
    attach_aux_track(lines, parse_lrc_track(romanization),
                     LyricAuxKind::Romanization);
  }

  // 先挂副轨再处理间奏：短空行连同它的空副轨一起删除，
  // 避免删行后副轨被错误地对到相邻的正文行。
  // 超过 3 秒的空行补成主行占位 `······`，副轨仍保留空串用于精确对齐。
  normalize_interlude_placeholders(lines);

  if (lines.empty()) {
    return lyrics;
  }
  lyrics.lines = std::move(lines);
  const std::string plain = plain_text_of(lyrics.lines);
  if (!plain.empty()) {
    lyrics.plain_text = plain;
  }
  return lyrics;
}

} // namespace media_library
