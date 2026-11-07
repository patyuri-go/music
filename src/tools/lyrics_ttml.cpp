#include "tools/lyrics_ttml.h"

#include "tools/amll_ttml.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace media_library {
namespace {

// 规范要求每行都有 ttm:agent，即使是单人演唱也要写；领域模型没有演唱者，
// 统一用一个匿名 agent。
constexpr const char *kDefaultAgent = "v1";

std::string line_key(std::size_t index) {
  return "L" + std::to_string(index + 1);
}

bool any_line_has_words(const Lyrics &lyrics) {
  return std::any_of(
      lyrics.lines.begin(), lyrics.lines.end(),
      [](const LyricLine &line) { return !line.words.empty(); });
}

bool aux_has_words(const Lyrics &lyrics, LyricAuxKind kind) {
  for (const LyricLine &line : lyrics.lines) {
    for (const LyricAux &aux : line.aux) {
      if (aux.kind == kind && !aux.words.empty()) {
        return true;
      }
    }
  }
  return false;
}

// 行结束时间：显式 end → 下一行 start → 整首时长。纯文本行（begin/end
// 都没有）不补时间。
std::optional<std::int64_t> resolved_line_end(const Lyrics &lyrics,
                                              const LyricsTtmlMeta &meta,
                                              std::size_t line_index) {
  const LyricLine &line = lyrics.lines[line_index];
  if (!line.start_ms.has_value() && !line.end_ms.has_value()) {
    return std::nullopt;
  }
  const std::int64_t start = line.start_ms.value_or(0);
  if (line.end_ms.has_value() && *line.end_ms > start) {
    return line.end_ms;
  }
  for (std::size_t next = line_index + 1; next < lyrics.lines.size(); ++next) {
    const std::optional<std::int64_t> next_start = lyrics.lines[next].start_ms;
    if (next_start.has_value() && *next_start > start) {
      return next_start;
    }
  }
  if (meta.duration_ms.has_value() && *meta.duration_ms > start) {
    return meta.duration_ms;
  }
  return std::nullopt;
}

// 一个音节的结束时间：优先用词自己的 end；其次下一个词的 start；再次
// 行 end（含回退链）；最后整首时长。都拿不到就不写 end。
std::optional<std::int64_t>
word_end_ms(const Lyrics &lyrics, const LyricsTtmlMeta &meta,
            std::size_t line_index, const std::vector<LyricWord> &words,
            std::size_t word_index) {
  const LyricWord &word = words[word_index];
  if (word.end_ms.has_value() && *word.end_ms > word.start_ms) {
    return word.end_ms;
  }
  if (word_index + 1 < words.size()) {
    const std::int64_t next = words[word_index + 1].start_ms;
    if (next > word.start_ms) {
      return next;
    }
  }
  if (const std::optional<std::int64_t> line_end =
          resolved_line_end(lyrics, meta, line_index);
      line_end.has_value() && *line_end > word.start_ms) {
    return line_end;
  }
  if (meta.duration_ms.has_value() && *meta.duration_ms > word.start_ms) {
    return meta.duration_ms;
  }
  return std::nullopt;
}

std::vector<amll::Item> make_word_items(const Lyrics &lyrics,
                                        const LyricsTtmlMeta &meta,
                                        std::size_t line_index,
                                        const std::vector<LyricWord> &words) {
  std::vector<amll::Item> items;
  items.reserve(words.size());
  for (std::size_t i = 0; i < words.size(); ++i) {
    amll::Item item;
    item.kind = amll::ItemKind::Word;
    item.text = words[i].text;
    item.begin_ms = words[i].start_ms;
    item.end_ms = word_end_ms(lyrics, meta, line_index, words, i);
    items.push_back(std::move(item));
  }
  return items;
}

amll::Item make_text_item(std::string text) {
  amll::Item item;
  item.kind = amll::ItemKind::Text;
  item.text = std::move(text);
  return item;
}

// 行内辅助轨：整行文本挂在 <p> 里（x-translation / x-roman）。
void append_inline_aux(std::vector<amll::Item> &items, const LyricAux &aux) {
  amll::Item item;
  item.kind = aux.kind == LyricAuxKind::Translation
                  ? amll::ItemKind::Translation
                  : amll::ItemKind::Roman;
  if (!aux.words.empty()) {
    // 行内 span 只放整行文本，词级时间由 head 的逐字轨承载；这里兜底把
    // 音节拼回整行，避免丢内容。
    std::string text;
    for (const LyricWord &word : aux.words) {
      text += word.text;
    }
    item.text = std::move(text);
  } else {
    item.text = aux.text;
  }
  if (item.text.empty()) {
    return;
  }
  items.push_back(std::move(item));
}

amll::AuxTrack build_aux_track(const Lyrics &lyrics,
                               const LyricsTtmlMeta &meta, LyricAuxKind kind) {
  amll::AuxTrack track;
  if (kind == LyricAuxKind::Translation) {
    track.type = "subtitle";
  }
  for (std::size_t i = 0; i < lyrics.lines.size(); ++i) {
    const LyricLine &line = lyrics.lines[i];
    for (const LyricAux &aux : line.aux) {
      if (aux.kind != kind) {
        continue;
      }
      amll::AuxText text;
      text.ref = line_key(i);
      if (!aux.words.empty()) {
        text.items = make_word_items(lyrics, meta, i, aux.words);
      } else if (!aux.text.empty()) {
        text.items.push_back(make_text_item(aux.text));
      }
      if (!text.items.empty()) {
        track.texts.push_back(std::move(text));
      }
    }
  }
  return track;
}

amll::Line convert_line(const Lyrics &lyrics, const LyricsTtmlMeta &meta,
                        std::size_t index, bool translation_inline,
                        bool roman_inline) {
  const LyricLine &line = lyrics.lines[index];

  amll::Line out;
  out.key = line_key(index);
  out.agent = kDefaultAgent;
  out.begin_ms = line.start_ms;
  out.end_ms = resolved_line_end(lyrics, meta, index);

  if (!line.words.empty()) {
    out.items = make_word_items(lyrics, meta, index, line.words);
  } else if (!line.text.empty()) {
    out.items.push_back(make_text_item(line.text));
  }

  // 规范建议辅助轨放在主唱之后。
  for (const LyricAux &aux : line.aux) {
    const bool inline_track =
        aux.kind == LyricAuxKind::Translation ? translation_inline : roman_inline;
    if (inline_track) {
      append_inline_aux(out.items, aux);
    }
  }
  return out;
}

std::optional<std::int64_t> body_duration(const Lyrics &lyrics,
                                          const LyricsTtmlMeta &meta) {
  std::optional<std::int64_t> max_end = meta.duration_ms;
  const auto record = [&max_end](std::int64_t value) {
    if (!max_end.has_value() || value > *max_end) {
      max_end = value;
    }
  };
  for (const LyricLine &line : lyrics.lines) {
    if (line.end_ms.has_value()) {
      record(*line.end_ms);
    }
    for (const LyricWord &word : line.words) {
      if (word.end_ms.has_value()) {
        record(*word.end_ms);
      }
    }
    for (const LyricAux &aux : line.aux) {
      for (const LyricWord &word : aux.words) {
        if (word.end_ms.has_value()) {
          record(*word.end_ms);
        }
      }
    }
  }
  return max_end;
}

} // namespace

std::string lyrics_to_ttml(const Lyrics &lyrics, const LyricsTtmlMeta &meta) {
  amll::Document document;

  if (lyrics.language.has_value() && !lyrics.language->empty()) {
    document.lang = lyrics.language;
  }
  document.timing = any_line_has_words(lyrics) ? "Word" : "Line";
  document.body_dur_ms = body_duration(lyrics, meta);

  if (!lyrics.lines.empty()) {
    document.agents.push_back(amll::Agent{kDefaultAgent, "person", ""});
  }

  // 元数据按规范的多值写法：artists 有几个就写几个 amll:meta。
  if (!meta.music_name.empty()) {
    document.metas.push_back(amll::Meta{"musicName", meta.music_name});
  }
  for (const std::string &artist : meta.artists) {
    if (!artist.empty()) {
      document.metas.push_back(amll::Meta{"artists", artist});
    }
  }
  if (!meta.album.empty()) {
    document.metas.push_back(amll::Meta{"album", meta.album});
  }

  // 词级辅助轨放 head，行级辅助轨留在行内；同一类辅助轨只选一种形态，
  // 避免规范警告的「head 翻译 + 行内翻译」双重叠加。
  bool translation_inline = true;
  if (aux_has_words(lyrics, LyricAuxKind::Translation)) {
    amll::AuxTrack track =
        build_aux_track(lyrics, meta, LyricAuxKind::Translation);
    if (!track.texts.empty()) {
      document.translations.push_back(std::move(track));
      translation_inline = false;
    }
  }
  bool roman_inline = true;
  if (aux_has_words(lyrics, LyricAuxKind::Romanization)) {
    amll::AuxTrack track =
        build_aux_track(lyrics, meta, LyricAuxKind::Romanization);
    if (!track.texts.empty()) {
      document.transliterations.push_back(std::move(track));
      roman_inline = false;
    }
  }

  amll::Section section;
  section.lines.reserve(lyrics.lines.size());
  for (std::size_t i = 0; i < lyrics.lines.size(); ++i) {
    section.lines.push_back(
        convert_line(lyrics, meta, i, translation_inline, roman_inline));
  }
  if (!lyrics.lines.empty()) {
    section.begin_ms = lyrics.lines.front().start_ms;
    section.end_ms =
        resolved_line_end(lyrics, meta, lyrics.lines.size() - 1);
  }
  if (!section.lines.empty()) {
    document.sections.push_back(std::move(section));
  }

  return amll::serialize(document);
}

} // namespace media_library
