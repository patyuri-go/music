#ifndef TOOLS_AMLL_TTML_H
#define TOOLS_AMLL_TTML_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// AMLL TTML DB 歌词格式的正反解析。
//
// 规范来源：https://github.com/amll-dev/amll-ttml-db 的《格式规范》wiki。
// 该格式是 TTML 的一个子集 + Apple Music 的 itunes:* / iTunesMetadata 扩展
// + amll:meta 元数据，解析用的是 third_party/pugixml（pugixml 不做命名空间
// 解析，所有名字按限定名逐字匹配，如 "ttm:agent" / "amll:meta"）。
//
// 这里刻意不复用 media_library::Lyrics：
// 那套模型表达不了 agent（每行演唱者）、songPart（段落）、Ruby（注音）、
// 背景人声、以及翻译/音译的 xml:lang 与 head 级多轨翻译，硬塞会丢信息。
// 需要接入播放链路时，再由调用方从本结构转换。

namespace amll {

// 一个带时轴的音节/词（逐字歌词的最小单位）。时间单位统一为毫秒。
struct Word {
  std::string text;      // 音节文本，不做 trim（音节间的空格是有意义的）
  std::int64_t begin_ms = 0;
  std::int64_t end_ms = 0;
};

// Ruby（注音）里带时轴的读音段，对应 <span tts:ruby="text">。
struct RubyText {
  std::string text;
  std::int64_t begin_ms = 0;
  std::int64_t end_ms = 0;
};

// Ruby（注音）：<span tts:ruby="container"> 的两半。
// base 是汉字原文（规范不允许带时间），texts 是逐字的注音读音。
struct Ruby {
  std::string base;
  std::vector<RubyText> texts;
};

enum class ItemKind {
  Text,        // 行级纯文本：<p> 直文、<text for> 直文、x-bg 内直文
  Word,        // 逐字音节：<span begin="..." end="...">字</span>
  Ruby,        // 注音：tts:ruby="container"
  Background,  // 背景人声：<span ttm:role="x-bg">，children 里可再带时间轴
  Translation, // 行内翻译：<span ttm:role="x-translation" xml:lang="zh-CN">
  Roman,       // 行内音译：<span ttm:role="x-roman" xml:lang="ja-Latn">
};

// 一行歌词内容的组成单元。保持原始先后顺序：规范建议背景人声在主唱之前
// 出现时放前面、否则放行末，顺序本身就是语义，不能重排。
struct Item {
  ItemKind kind = ItemKind::Text;

  std::string text;                     // Text / Translation / Roman
  std::optional<std::int64_t> begin_ms; // Word / 可选的 Background 时间
  std::optional<std::int64_t> end_ms;
  std::optional<std::string> lang;      // Translation / Roman 的 xml:lang

  Ruby ruby;                            // kind == Ruby
  std::vector<Item> children;           // kind == Background
};

// <p>：一行歌词。itunes:key 是 "L" + 行号（L1、L2…），ttm:agent 指向 head
// 里的 agent（单人也要写 v1）。
struct Line {
  std::optional<std::int64_t> begin_ms;
  std::optional<std::int64_t> end_ms;
  std::string key;
  std::string agent;
  std::vector<Item> items;
};

// <div>：段落（可选）。song_part 取 itunes:songPart（读时兼容旧写法
// itunes:song-part），如 Verse / Chorus / Bridge。
struct Section {
  std::optional<std::int64_t> begin_ms;
  std::optional<std::int64_t> end_ms;
  std::string song_part;
  std::vector<Line> lines;
};

// <ttm:agent>：演唱者/角色。
struct Agent {
  std::string id;    // xml:id，如 v1
  std::string type;  // person / character / organization / group / other
  std::string name;  // <ttm:name type="full">，缺省为空
};

// <amll:meta>：音乐元数据，保持原始顺序（同一 key 允许出现多次）。
struct Meta {
  std::string key;
  std::string value;
};

// <text for="L1">：一条与某行对应的翻译/音译内容。
// 逐行时 items 里通常只有一个 Text；逐字时是 Word / Ruby / Background。
struct AuxText {
  std::string ref;  // for 属性，即 Line::key
  std::vector<Item> items;
};

// <translation> / <transliteration>：一条 head 级翻译或音译轨。
struct AuxTrack {
  std::string type;                     // subtitle / replacement，音译轨为空
  std::optional<std::string> lang;      // xml:lang
  std::vector<AuxText> texts;
};

// 一整份 TTML 文档。
struct Document {
  std::optional<std::string> lang;        // <tt xml:lang>
  std::string timing;                     // itunes:timing："Word" / "Line"
  std::string title;                      // <ttm:title>，与 amll:meta 的 musicName 二选一
  std::vector<Agent> agents;
  std::vector<Meta> metas;
  std::vector<AuxTrack> translations;     // iTunesMetadata/translations
  std::vector<AuxTrack> transliterations; // iTunesMetadata/transliterations
  std::vector<std::string> songwriters;   // iTunesMetadata/songwriters
  std::optional<std::int64_t> body_dur_ms; // <body dur>，仅供参考
  std::vector<Section> sections;

  // 取第一个匹配 key 的 amll:meta 值。
  std::optional<std::string> meta(const std::string &key) const;
  // 覆盖写入（同 key 已存在则改第一个，否则追加）。
  void set_meta(const std::string &key, const std::string &value);
};

// 正向解析：TTML 文本 -> Document。失败返回 nullopt，error 里带 pugixml 的
// 出错位置说明（error 可为 nullptr）。
std::optional<Document> parse(const std::string &xml, std::string *error);

// 反向序列化：Document -> TTML 文本（带 XML 声明与全部命名空间）。
std::string serialize(const Document &document);

// 时间戳互转：支持 ss / ss.xxx / mm:ss / mm:ss.xxx / hh:mm:ss / hh:mm:ss.xxx，
// 小数部分按毫秒解释。parse 失败返回 nullopt。
std::optional<std::int64_t> parse_timestamp(const std::string &text);
std::string format_timestamp(std::int64_t ms);

} // namespace amll

#endif // TOOLS_AMLL_TTML_H
