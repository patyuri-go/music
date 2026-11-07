// AMLL TTML 正反解析测试。
//
// 覆盖《amll-ttml-db 格式规范》里的全部特性：agents、amll:meta、
// iTunesMetadata（translations / transliterations / songwriters）、
// div + songPart、逐字 word、Ruby、x-bg（含内嵌音节与翻译）、
// 行内 x-translation / x-roman、逐行 <p>、body@dur。
//
// 两个方向：
//   1. 正：TTML -> Document，逐字段断言。
//   2. 反：Document -> TTML，再 parse 一次，serialize 结果必须与原串等价。

#include "tools/amll_ttml.h"

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const std::string &what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::cerr << "FAIL: " << what << '\n';
  }
}

void check_str(const std::string &actual, const std::string &expected,
               const std::string &what) {
  check(actual == expected, what + " (got \"" + actual + "\", want \"" +
                                expected + "\")");
}

void check_int(const std::optional<std::int64_t> &actual,
               std::int64_t expected, const std::string &what) {
  const bool ok = actual.has_value() && *actual == expected;
  check(ok, what + " (got " +
                (actual.has_value() ? std::to_string(*actual)
                                    : std::string("nullopt")) +
                ", want " + std::to_string(expected) + ")");
}

void check_size(std::size_t actual, std::size_t expected,
                const std::string &what) {
  check(actual == expected, what + " (got " + std::to_string(actual) +
                                ", want " + std::to_string(expected) + ")");
}

// 规范里所有特性各来一份的样例。
const char *kSample = R"TTML(<?xml version="1.0" encoding="UTF-8"?>
<tt xmlns="http://www.w3.org/ns/ttml" xmlns:ttm="http://www.w3.org/ns/ttml#metadata" xmlns:tts="http://www.w3.org/ns/ttml#styling" xmlns:itunes="http://music.apple.com/lyric-ttml-internal" xmlns:amll="http://www.example.com/ns/amll" xml:lang="ja" itunes:timing="Word">
  <head>
    <metadata>
      <ttm:title>夜に駆ける</ttm:title>
      <ttm:agent type="person" xml:id="v1">
        <ttm:name type="full">幾田りら</ttm:name>
      </ttm:agent>
      <ttm:agent type="person" xml:id="v2">
        <ttm:name type="full">Ayase</ttm:name>
      </ttm:agent>
      <amll:meta key="musicName" value="夜に駆ける" />
      <amll:meta key="artists" value="YOASOBI" />
      <amll:meta key="album" value="THE BOOK" />
      <amll:meta key="ncmMusicId" value="1409317966" />
      <iTunesMetadata xmlns="http://music.apple.com/lyric-ttml-internal">
        <translations>
          <translation type="subtitle" xml:lang="zh-Hans">
            <text for="L1">沉溺于夜色之中</text>
            <text for="L2"><span begin="2.000" end="2.400">今</span><span begin="2.400" end="2.800">夜</span></text>
          </translation>
        </translations>
        <transliterations>
          <transliteration xml:lang="ja-Latn">
            <text for="L1">shizumu you ni</text>
          </transliteration>
        </transliterations>
        <songwriters>
          <songwriter>Ayase</songwriter>
        </songwriters>
      </iTunesMetadata>
    </metadata>
  </head>
  <body dur="6.000">
    <div begin="0.000" end="4.000" itunes:songPart="Verse">
      <p begin="0.000" end="2.000" itunes:key="L1" ttm:agent="v1">沈むように<span begin="0.500" end="1.200">溶</span>けて<span ttm:role="x-translation" xml:lang="zh-CN">沉溺般</span><span ttm:role="x-roman" xml:lang="ja-Latn">shizumu you ni</span></p>
      <p begin="2.000" end="4.000" itunes:key="L2" ttm:agent="v1"><span tts:ruby="container"><span tts:ruby="base">今</span><span tts:ruby="textContainer"><span tts:ruby="text" begin="2.000" end="2.400">い</span><span tts:ruby="text" begin="2.400" end="2.800">ま</span></span></span><span begin="2.800" end="3.400">、</span><span ttm:role="x-bg" begin="3.400" end="4.000"><span begin="3.400" end="3.700">ドキ</span><span begin="3.700" end="4.000">ドキ</span><span ttm:role="x-translation" xml:lang="zh-CN">心跳</span></span></p>
    </div>
    <div begin="4.000" end="6.000" itunes:songPart="Chorus">
      <p begin="4.000" end="5.000" itunes:key="L3" ttm:agent="v2">走り出す<span begin="4.400" end="5.000">影</span></p>
      <p begin="5.000" end="6.000" itunes:key="L4" ttm:agent="v2">溶けてしまいそう</p>
    </div>
  </body>
</tt>
)TTML";

void test_timestamps() {
  check_int(amll::parse_timestamp("2"), 2000, "parse ss");
  check_int(amll::parse_timestamp("2.5"), 2500, "parse ss.xxx");
  check_int(amll::parse_timestamp("1:02.345"), 62345, "parse mm:ss.xxx");
  check_int(amll::parse_timestamp("01:02:03.004"), 3723004, "parse hh:mm:ss.xxx");
  check_int(amll::parse_timestamp("00:00"), 0, "parse mm:ss 全零");
  check(!amll::parse_timestamp("abc").has_value(), "非数字应失败");
  check(!amll::parse_timestamp("1s").has_value(), "带单位应失败");
  check(!amll::parse_timestamp("").has_value(), "空串应失败");

  check_str(amll::format_timestamp(62345), "01:02.345", "format mm:ss.xxx");
  check_str(amll::format_timestamp(3723004), "01:02:03.004", "format hh:mm:ss.xxx");
  check_str(amll::format_timestamp(0), "00:00.000", "format 零");
  // 往返：格式化的结果必须还能解析回同一个毫秒数。
  for (std::int64_t ms : {0LL, 999LL, 1000LL, 59999LL, 60000LL, 3723004LL}) {
    const auto back = amll::parse_timestamp(amll::format_timestamp(ms));
    check_int(back, ms, "时间戳往返 " + std::to_string(ms));
  }
}

void test_parse(const amll::Document &doc) {
  check_str(doc.lang.value_or(""), "ja", "xml:lang");
  check_str(doc.timing, "Word", "itunes:timing");
  check_str(doc.title, "夜に駆ける", "ttm:title");
  check_int(doc.body_dur_ms, 6000, "body dur");

  // agents
  check_size(doc.agents.size(), 2, "agent 数量");
  if (doc.agents.size() == 2) {
    check_str(doc.agents[0].id, "v1", "agent[0].id");
    check_str(doc.agents[0].type, "person", "agent[0].type");
    check_str(doc.agents[0].name, "幾田りら", "agent[0].name");
    check_str(doc.agents[1].id, "v2", "agent[1].id");
    check_str(doc.agents[1].name, "Ayase", "agent[1].name");
  }

  // amll:meta
  check_size(doc.metas.size(), 4, "meta 数量");
  check_str(doc.meta("musicName").value_or(""), "夜に駆ける", "meta musicName");
  check_str(doc.meta("artists").value_or(""), "YOASOBI", "meta artists");
  check_str(doc.meta("album").value_or(""), "THE BOOK", "meta album");
  check_str(doc.meta("ncmMusicId").value_or(""), "1409317966", "meta ncmMusicId");
  check(!doc.meta("qqMusicId").has_value(), "不存在的 meta 应为 nullopt");

  // head 级翻译
  check_size(doc.translations.size(), 1, "translations 数量");
  if (doc.translations.size() == 1) {
    const amll::AuxTrack &track = doc.translations[0];
    check_str(track.type, "subtitle", "translation type");
    check_str(track.lang.value_or(""), "zh-Hans", "translation xml:lang");
    check_size(track.texts.size(), 2, "translation text 数量");
    if (track.texts.size() == 2) {
      check_str(track.texts[0].ref, "L1", "translation[0].for");
      check_size(track.texts[0].items.size(), 1, "translation[0] item 数量");
      if (!track.texts[0].items.empty()) {
        check(track.texts[0].items[0].kind == amll::ItemKind::Text,
              "translation[0] 是纯文本");
        check_str(track.texts[0].items[0].text, "沉溺于夜色之中",
                  "translation[0] 文本");
      }
      check_str(track.texts[1].ref, "L2", "translation[1].for");
      check_size(track.texts[1].items.size(), 2, "translation[1] 逐字数量");
      if (track.texts[1].items.size() == 2) {
        check(track.texts[1].items[0].kind == amll::ItemKind::Word,
              "translation[1][0] 是逐字");
        check_str(track.texts[1].items[0].text, "今", "translation[1][0] 文本");
        check_int(track.texts[1].items[0].begin_ms, 2000,
                  "translation[1][0] begin");
        check_int(track.texts[1].items[0].end_ms, 2400,
                  "translation[1][0] end");
        check_int(track.texts[1].items[1].begin_ms, 2400,
                  "translation[1][1] begin");
      }
    }
  }

  // head 级音译
  check_size(doc.transliterations.size(), 1, "transliterations 数量");
  if (doc.transliterations.size() == 1) {
    check_str(doc.transliterations[0].lang.value_or(""), "ja-Latn",
              "transliteration xml:lang");
    check(doc.transliterations[0].type.empty(), "transliteration 无 type");
    check_size(doc.transliterations[0].texts.size(), 1,
               "transliteration text 数量");
  }

  // songwriters
  check_size(doc.songwriters.size(), 1, "songwriter 数量");
  if (!doc.songwriters.empty()) {
    check_str(doc.songwriters[0], "Ayase", "songwriter");
  }

  // sections
  check_size(doc.sections.size(), 2, "section 数量");
  if (doc.sections.size() != 2) {
    return;
  }
  const amll::Section &verse = doc.sections[0];
  check_str(verse.song_part, "Verse", "section[0].songPart");
  check_int(verse.begin_ms, 0, "section[0].begin");
  check_int(verse.end_ms, 4000, "section[0].end");
  check_size(verse.lines.size(), 2, "section[0] 行数");

  const amll::Section &chorus = doc.sections[1];
  check_str(chorus.song_part, "Chorus", "section[1].songPart");
  check_int(chorus.begin_ms, 4000, "section[1].begin");
  check_size(chorus.lines.size(), 2, "section[1] 行数");

  // L1：行级文本 + 逐字 + 行内翻译/音译
  if (verse.lines.size() == 2) {
    const amll::Line &l1 = verse.lines[0];
    check_str(l1.key, "L1", "L1 key");
    check_str(l1.agent, "v1", "L1 agent");
    check_int(l1.begin_ms, 0, "L1 begin");
    check_int(l1.end_ms, 2000, "L1 end");
    check_size(l1.items.size(), 5, "L1 item 数量");
    if (l1.items.size() == 5) {
      check(l1.items[0].kind == amll::ItemKind::Text, "L1[0] Text");
      check_str(l1.items[0].text, "沈むように", "L1[0] 文本");
      check(l1.items[1].kind == amll::ItemKind::Word, "L1[1] Word");
      check_str(l1.items[1].text, "溶", "L1[1] 文本");
      check_int(l1.items[1].begin_ms, 500, "L1[1] begin");
      check_int(l1.items[1].end_ms, 1200, "L1[1] end");
      check_str(l1.items[2].text, "けて", "L1[2] 文本");
      check(l1.items[3].kind == amll::ItemKind::Translation, "L1[3] 翻译");
      check_str(l1.items[3].text, "沉溺般", "L1[3] 文本");
      check_str(l1.items[3].lang.value_or(""), "zh-CN", "L1[3] xml:lang");
      check(l1.items[4].kind == amll::ItemKind::Roman, "L1[4] 音译");
      check_str(l1.items[4].text, "shizumu you ni", "L1[4] 文本");
      check_str(l1.items[4].lang.value_or(""), "ja-Latn", "L1[4] xml:lang");
    }

    // L2：Ruby + 逐字 + 背景人声（内含逐字与翻译）
    const amll::Line &l2 = verse.lines[1];
    check_str(l2.key, "L2", "L2 key");
    check_size(l2.items.size(), 3, "L2 item 数量（Ruby / 标点 / x-bg）");
    if (l2.items.size() == 3) {
      const amll::Item &ruby = l2.items[0];
      check(ruby.kind == amll::ItemKind::Ruby, "L2[0] Ruby");
      check_str(ruby.ruby.base, "今", "L2[0] ruby base");
      check_size(ruby.ruby.texts.size(), 2, "L2[0] ruby text 数量");
      if (ruby.ruby.texts.size() == 2) {
        check_str(ruby.ruby.texts[0].text, "い", "L2[0] ruby[0]");
        check_int(ruby.ruby.texts[0].begin_ms, 2000, "L2[0] ruby[0] begin");
        check_int(ruby.ruby.texts[0].end_ms, 2400, "L2[0] ruby[0] end");
        check_str(ruby.ruby.texts[1].text, "ま", "L2[0] ruby[1]");
        check_int(ruby.ruby.texts[1].begin_ms, 2400, "L2[0] ruby[1] begin");
        check_int(ruby.ruby.texts[1].end_ms, 2800, "L2[0] ruby[1] end");
      }

      check(l2.items[1].kind == amll::ItemKind::Word, "L2[1] Word");
      check_str(l2.items[1].text, "、", "L2[1] 文本");

      const amll::Item &bg = l2.items[2];
      check(bg.kind == amll::ItemKind::Background, "L2[2] Background");
      check_int(bg.begin_ms, 3400, "L2[2] begin");
      check_int(bg.end_ms, 4000, "L2[2] end");
      check_size(bg.children.size(), 3, "L2[2] 子项数量");
      if (bg.children.size() == 3) {
        check(bg.children[0].kind == amll::ItemKind::Word, "L2[2][0] Word");
        check_str(bg.children[0].text, "ドキ", "L2[2][0] 文本");
        check_int(bg.children[0].begin_ms, 3400, "L2[2][0] begin");
        check_str(bg.children[1].text, "ドキ", "L2[2][1] 文本");
        check_int(bg.children[1].end_ms, 4000, "L2[2][1] end");
        check(bg.children[2].kind == amll::ItemKind::Translation,
              "L2[2][2] 翻译");
        check_str(bg.children[2].text, "心跳", "L2[2][2] 文本");
        check_str(bg.children[2].lang.value_or(""), "zh-CN",
                  "L2[2][2] xml:lang");
      }
    }
  }

  // L4：整行纯文本（逐行形态）
  if (chorus.lines.size() == 2) {
    const amll::Line &l4 = chorus.lines[1];
    check_str(l4.key, "L4", "L4 key");
    check_str(l4.agent, "v2", "L4 agent");
    check_size(l4.items.size(), 1, "L4 item 数量");
    if (!l4.items.empty()) {
      check(l4.items[0].kind == amll::ItemKind::Text, "L4[0] Text");
      check_str(l4.items[0].text, "溶けてしまいそう", "L4[0] 文本");
    }
  }
}

void test_serialize_roundtrip(const amll::Document &doc) {
  const std::string once = amll::serialize(doc);
  check(!once.empty(), "serialize 非空");
  check(once.find("<tt ") != std::string::npos, "序列化含 <tt> 根元素");
  check(once.find("xmlns:itunes=\"http://music.apple.com/lyric-ttml-internal\"") !=
            std::string::npos,
        "序列化使用当前 Apple 命名空间");
  check(once.find("ttm:agent") != std::string::npos, "序列化含 agent");
  check(once.find("<iTunesMetadata") != std::string::npos,
        "序列化含 iTunesMetadata");
  check(once.find("tts:ruby=\"container\"") != std::string::npos,
        "序列化含 ruby");
  check(once.find("ttm:role=\"x-bg\"") != std::string::npos,
        "序列化含 x-bg");

  std::string error;
  const std::optional<amll::Document> reparsed = amll::parse(once, &error);
  check(reparsed.has_value(), "序列化结果可再次解析: " + error);
  if (!reparsed.has_value()) {
    return;
  }
  const std::string twice = amll::serialize(*reparsed);
  check(twice == once, "serialize -> parse -> serialize 幂等");
  if (twice != once) {
    // 打印首个差异位置附近的片段，方便定位。
    std::size_t i = 0;
    while (i < once.size() && i < twice.size() && once[i] == twice[i]) {
      ++i;
    }
    const std::size_t from = i > 60 ? i - 60 : 0;
    std::cerr << "  首次差异 @ " << i << "\n  once:  "
              << once.substr(from, 160) << "\n  twice: "
              << twice.substr(from, 160) << '\n';
  }

  // 再解析一次后字段仍然一致（防止序列化丢信息）。
  test_parse(*reparsed);
}

} // namespace

int main() {
  test_timestamps();

  std::string error;
  const std::optional<amll::Document> doc = amll::parse(kSample, &error);
  check(doc.has_value(), "样例可解析: " + error);
  if (!doc.has_value()) {
    std::cerr << "解析失败，跳过后续断言\n";
    return 1;
  }

  test_parse(*doc);
  test_serialize_roundtrip(*doc);

  // 错误处理：非法 XML 必须返回 nullopt 并给出错误说明。
  std::string bad_error;
  check(!amll::parse("<tt><body>", &bad_error).has_value(), "非法 XML 应失败");
  check(!bad_error.empty(), "非法 XML 应有错误说明");
  check(!amll::parse("<html/>", nullptr).has_value(), "根元素非 tt 应失败");

  // serialize 一个空 Document 也要产出合法 TTML。
  {
    const amll::Document empty;
    const std::string text = amll::serialize(empty);
    const std::optional<amll::Document> back = amll::parse(text, &error);
    check(back.has_value(), "空文档序列化结果可解析: " + error);
    if (back.has_value()) {
      check(back->sections.empty(), "空文档无 section");
      check(back->agents.empty(), "空文档无 agent");
    }
  }

  std::cout << (g_failures == 0 ? "OK" : "FAILED") << ": " << g_checks
            << " checks, " << g_failures << " failure(s)\n";
  return g_failures == 0 ? 0 : 1;
}
