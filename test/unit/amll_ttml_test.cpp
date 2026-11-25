// AMLL TTML 正反解析单元测试（GTest）。
//
// 覆盖《amll-ttml-db 格式规范》里的全部特性：agents、amll:meta、
// iTunesMetadata（translations / transliterations / songwriters）、
// div + songPart、逐字 word、Ruby、x-bg（含内嵌音节与翻译）、
// 行内 x-translation / x-roman、逐行 <p>、body@dur。
//
// 正向：TTML -> Document，按域逐字段断言。
// 反向：Document -> TTML，再 parse，serialize 结果必须幂等且字段不丢。

#include "tools/amll_ttml.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

// 规范里所有特性各来一份的样例。
constexpr const char *kSample = R"TTML(<?xml version="1.0" encoding="UTF-8"?>
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

void expectTimestamp(std::string_view text, std::int64_t expected) {
  const auto actual = amll::parse_timestamp(std::string(text));
  ASSERT_TRUE(actual.has_value()) << std::string(text);
  EXPECT_EQ(*actual, expected) << std::string(text);
}

void expectMetadata(const amll::Document &doc) {
  EXPECT_EQ(doc.lang.value_or(""), "ja");
  EXPECT_EQ(doc.timing, "Word");
  EXPECT_EQ(doc.title, "夜に駆ける");
  ASSERT_TRUE(doc.body_dur_ms.has_value());
  EXPECT_EQ(*doc.body_dur_ms, 6000);
}

void expectAgents(const amll::Document &doc) {
  ASSERT_EQ(doc.agents.size(), 2u);
  EXPECT_EQ(doc.agents[0].id, "v1");
  EXPECT_EQ(doc.agents[0].type, "person");
  EXPECT_EQ(doc.agents[0].name, "幾田りら");
  EXPECT_EQ(doc.agents[1].id, "v2");
  EXPECT_EQ(doc.agents[1].name, "Ayase");
}

void expectMetas(const amll::Document &doc) {
  ASSERT_EQ(doc.metas.size(), 4u);
  EXPECT_EQ(doc.meta("musicName").value_or(""), "夜に駆ける");
  EXPECT_EQ(doc.meta("artists").value_or(""), "YOASOBI");
  EXPECT_EQ(doc.meta("album").value_or(""), "THE BOOK");
  EXPECT_EQ(doc.meta("ncmMusicId").value_or(""), "1409317966");
  EXPECT_FALSE(doc.meta("qqMusicId").has_value());
}

void expectTranslations(const amll::Document &doc) {
  ASSERT_EQ(doc.translations.size(), 1u);
  const amll::AuxTrack &track = doc.translations[0];
  EXPECT_EQ(track.type, "subtitle");
  EXPECT_EQ(track.lang.value_or(""), "zh-Hans");
  ASSERT_EQ(track.texts.size(), 2u);

  const amll::AuxText &plain = track.texts[0];
  EXPECT_EQ(plain.ref, "L1");
  ASSERT_EQ(plain.items.size(), 1u);
  EXPECT_EQ(plain.items[0].kind, amll::ItemKind::Text);
  EXPECT_EQ(plain.items[0].text, "沉溺于夜色之中");

  const amll::AuxText &worded = track.texts[1];
  EXPECT_EQ(worded.ref, "L2");
  ASSERT_EQ(worded.items.size(), 2u);
  EXPECT_EQ(worded.items[0].kind, amll::ItemKind::Word);
  EXPECT_EQ(worded.items[0].text, "今");
  ASSERT_TRUE(worded.items[0].begin_ms.has_value());
  EXPECT_EQ(*worded.items[0].begin_ms, 2000);
  ASSERT_TRUE(worded.items[0].end_ms.has_value());
  EXPECT_EQ(*worded.items[0].end_ms, 2400);
  ASSERT_TRUE(worded.items[1].begin_ms.has_value());
  EXPECT_EQ(*worded.items[1].begin_ms, 2400);
}

void expectTransliterations(const amll::Document &doc) {
  ASSERT_EQ(doc.transliterations.size(), 1u);
  const amll::AuxTrack &track = doc.transliterations[0];
  EXPECT_EQ(track.lang.value_or(""), "ja-Latn");
  EXPECT_TRUE(track.type.empty());
  ASSERT_EQ(track.texts.size(), 1u);
}

void expectSongwriters(const amll::Document &doc) {
  ASSERT_EQ(doc.songwriters.size(), 1u);
  EXPECT_EQ(doc.songwriters[0], "Ayase");
}

void expectSections(const amll::Document &doc) {
  ASSERT_EQ(doc.sections.size(), 2u);

  const amll::Section &verse = doc.sections[0];
  EXPECT_EQ(verse.song_part, "Verse");
  ASSERT_TRUE(verse.begin_ms.has_value());
  EXPECT_EQ(*verse.begin_ms, 0);
  ASSERT_TRUE(verse.end_ms.has_value());
  EXPECT_EQ(*verse.end_ms, 4000);
  ASSERT_EQ(verse.lines.size(), 2u);

  const amll::Section &chorus = doc.sections[1];
  EXPECT_EQ(chorus.song_part, "Chorus");
  ASSERT_TRUE(chorus.begin_ms.has_value());
  EXPECT_EQ(*chorus.begin_ms, 4000);
  ASSERT_EQ(chorus.lines.size(), 2u);
}

void expectLines(const amll::Document &doc) {
  ASSERT_EQ(doc.sections.size(), 2u);
  const amll::Section &verse = doc.sections[0];
  ASSERT_EQ(verse.lines.size(), 2u);

  // L1：行级文本 + 逐字 + 行内翻译/音译。
  const amll::Line &l1 = verse.lines[0];
  EXPECT_EQ(l1.key, "L1");
  EXPECT_EQ(l1.agent, "v1");
  ASSERT_TRUE(l1.begin_ms.has_value());
  EXPECT_EQ(*l1.begin_ms, 0);
  ASSERT_TRUE(l1.end_ms.has_value());
  EXPECT_EQ(*l1.end_ms, 2000);
  ASSERT_EQ(l1.items.size(), 5u);
  EXPECT_EQ(l1.items[0].kind, amll::ItemKind::Text);
  EXPECT_EQ(l1.items[0].text, "沈むように");
  EXPECT_EQ(l1.items[1].kind, amll::ItemKind::Word);
  EXPECT_EQ(l1.items[1].text, "溶");
  ASSERT_TRUE(l1.items[1].begin_ms.has_value());
  EXPECT_EQ(*l1.items[1].begin_ms, 500);
  ASSERT_TRUE(l1.items[1].end_ms.has_value());
  EXPECT_EQ(*l1.items[1].end_ms, 1200);
  EXPECT_EQ(l1.items[2].kind, amll::ItemKind::Text);
  EXPECT_EQ(l1.items[2].text, "けて");
  EXPECT_EQ(l1.items[3].kind, amll::ItemKind::Translation);
  EXPECT_EQ(l1.items[3].text, "沉溺般");
  EXPECT_EQ(l1.items[3].lang.value_or(""), "zh-CN");
  EXPECT_EQ(l1.items[4].kind, amll::ItemKind::Roman);
  EXPECT_EQ(l1.items[4].text, "shizumu you ni");
  EXPECT_EQ(l1.items[4].lang.value_or(""), "ja-Latn");

  // L2：Ruby + 标点 + 背景人声（内含逐字与翻译）。
  const amll::Line &l2 = verse.lines[1];
  EXPECT_EQ(l2.key, "L2");
  ASSERT_EQ(l2.items.size(), 3u);

  const amll::Item &ruby = l2.items[0];
  EXPECT_EQ(ruby.kind, amll::ItemKind::Ruby);
  EXPECT_EQ(ruby.ruby.base, "今");
  ASSERT_EQ(ruby.ruby.texts.size(), 2u);
  EXPECT_EQ(ruby.ruby.texts[0].text, "い");
  EXPECT_EQ(ruby.ruby.texts[0].begin_ms, 2000);
  EXPECT_EQ(ruby.ruby.texts[0].end_ms, 2400);
  EXPECT_EQ(ruby.ruby.texts[1].text, "ま");
  EXPECT_EQ(ruby.ruby.texts[1].begin_ms, 2400);
  EXPECT_EQ(ruby.ruby.texts[1].end_ms, 2800);

  EXPECT_EQ(l2.items[1].kind, amll::ItemKind::Word);
  EXPECT_EQ(l2.items[1].text, "、");

  const amll::Item &bg = l2.items[2];
  EXPECT_EQ(bg.kind, amll::ItemKind::Background);
  ASSERT_TRUE(bg.begin_ms.has_value());
  EXPECT_EQ(*bg.begin_ms, 3400);
  ASSERT_TRUE(bg.end_ms.has_value());
  EXPECT_EQ(*bg.end_ms, 4000);
  ASSERT_EQ(bg.children.size(), 3u);
  EXPECT_EQ(bg.children[0].kind, amll::ItemKind::Word);
  EXPECT_EQ(bg.children[0].text, "ドキ");
  ASSERT_TRUE(bg.children[0].begin_ms.has_value());
  EXPECT_EQ(*bg.children[0].begin_ms, 3400);
  EXPECT_EQ(bg.children[1].kind, amll::ItemKind::Word);
  EXPECT_EQ(bg.children[1].text, "ドキ");
  ASSERT_TRUE(bg.children[1].end_ms.has_value());
  EXPECT_EQ(*bg.children[1].end_ms, 4000);
  EXPECT_EQ(bg.children[2].kind, amll::ItemKind::Translation);
  EXPECT_EQ(bg.children[2].text, "心跳");
  EXPECT_EQ(bg.children[2].lang.value_or(""), "zh-CN");

  // L4：整行纯文本（逐行形态）。
  const amll::Section &chorus = doc.sections[1];
  ASSERT_EQ(chorus.lines.size(), 2u);
  const amll::Line &l4 = chorus.lines[1];
  EXPECT_EQ(l4.key, "L4");
  EXPECT_EQ(l4.agent, "v2");
  ASSERT_EQ(l4.items.size(), 1u);
  EXPECT_EQ(l4.items[0].kind, amll::ItemKind::Text);
  EXPECT_EQ(l4.items[0].text, "溶けてしまいそう");
}

} // namespace

TEST(TimestampsTest, ParsesSupportedForms) {
  expectTimestamp("2", 2000);
  expectTimestamp("2.5", 2500);
  expectTimestamp("1:02.345", 62345);
  expectTimestamp("01:02:03.004", 3723004);
  expectTimestamp("00:00", 0);
}

TEST(TimestampsTest, RejectsMalformedInput) {
  EXPECT_FALSE(amll::parse_timestamp("abc").has_value());
  EXPECT_FALSE(amll::parse_timestamp("1s").has_value());
  EXPECT_FALSE(amll::parse_timestamp("").has_value());
}

TEST(TimestampsTest, FormatsAndRoundTrips) {
  EXPECT_EQ(amll::format_timestamp(62345), "01:02.345");
  EXPECT_EQ(amll::format_timestamp(3723004), "01:02:03.004");
  EXPECT_EQ(amll::format_timestamp(0), "00:00.000");

  for (const std::int64_t ms :
       {0LL, 999LL, 1000LL, 59999LL, 60000LL, 3723004LL}) {
    SCOPED_TRACE(ms);
    expectTimestamp(amll::format_timestamp(ms), ms);
  }
}

class AmllDocumentTest : public ::testing::Test {
protected:
  void SetUp() override {
    std::string error;
    std::optional<amll::Document> parsed = amll::parse(kSample, &error);
    ASSERT_TRUE(parsed.has_value()) << "样例应能解析: " << error;
    doc_ = std::move(*parsed);
  }

  amll::Document doc_;
};

TEST_F(AmllDocumentTest, ParsesDocumentMetadata) {
  expectMetadata(doc_);
}

TEST_F(AmllDocumentTest, ParsesAgents) {
  expectAgents(doc_);
}

TEST_F(AmllDocumentTest, ParsesAmllMeta) {
  expectMetas(doc_);
}

TEST_F(AmllDocumentTest, ParsesHeadTranslations) {
  expectTranslations(doc_);
}

TEST_F(AmllDocumentTest, ParsesHeadTransliterations) {
  expectTransliterations(doc_);
}

TEST_F(AmllDocumentTest, ParsesSongwriters) {
  expectSongwriters(doc_);
}

TEST_F(AmllDocumentTest, ParsesSections) {
  expectSections(doc_);
}

TEST_F(AmllDocumentTest, ParsesLines) {
  expectLines(doc_);
}

TEST_F(AmllDocumentTest, SerializesExpectedStructure) {
  const std::string once = amll::serialize(doc_);
  EXPECT_FALSE(once.empty());
  EXPECT_NE(once.find("<tt "), std::string::npos);
  EXPECT_NE(
      once.find(
          R"(xmlns:itunes="http://music.apple.com/lyric-ttml-internal")"),
      std::string::npos);
  EXPECT_NE(once.find("ttm:agent"), std::string::npos);
  EXPECT_NE(once.find("<iTunesMetadata"), std::string::npos);
  EXPECT_NE(once.find(R"(tts:ruby="container")"), std::string::npos);
  EXPECT_NE(once.find(R"(ttm:role="x-bg")"), std::string::npos);
}

TEST_F(AmllDocumentTest, RoundTripsWithoutLosingFields) {
  const std::string once = amll::serialize(doc_);

  std::string error;
  const std::optional<amll::Document> reparsed = amll::parse(once, &error);
  ASSERT_TRUE(reparsed.has_value()) << "序列化结果应可再次解析: " << error;

  const std::string twice = amll::serialize(*reparsed);
  EXPECT_EQ(twice, once);

  // 防止序列化丢信息：重解析后所有域都必须与样例一致。
  expectMetadata(*reparsed);
  expectAgents(*reparsed);
  expectMetas(*reparsed);
  expectTranslations(*reparsed);
  expectTransliterations(*reparsed);
  expectSongwriters(*reparsed);
  expectSections(*reparsed);
  expectLines(*reparsed);
}

TEST(AmllErrorTest, RejectsInvalidInput) {
  std::string error;
  EXPECT_FALSE(amll::parse("<tt><body>", &error).has_value());
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(amll::parse("<html/>", nullptr).has_value());
}

TEST(AmllErrorTest, SerializesEmptyDocument) {
  const amll::Document empty;
  const std::string text = amll::serialize(empty);

  std::string error;
  const std::optional<amll::Document> back = amll::parse(text, &error);
  ASSERT_TRUE(back.has_value()) << "空文档序列化结果应可解析: " << error;
  EXPECT_TRUE(back->sections.empty());
  EXPECT_TRUE(back->agents.empty());
}