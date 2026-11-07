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

// 行结束时间：显式 end -> 下一行第一个更晚的 start -> meta.duration_ms。
// 纯文本行（begin/end 都没有）不补时间。
std::optional<std::int64_t>
resolved_line_end(const Lyrics &lyrics, const LyricsTtmlMeta &meta,
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
  // 到这里说明所有回退都不可用：AMLL 要求 begin < end，行只能给最小时长。
  return start + 1;
}

// 同一个 items 列表里，从 index 之后找第一个带时间的 Word 的 start。
std::optional<std::int64_t>
next_word_start(const std::vector<LyricItem> &items, std::size_t index,
                std::int64_t after_ms) {
  for (std::size_t i = index + 1; i < items.size(); ++i) {
    if (items[i].kind != LyricItemKind::Word ||
        !items[i].start_ms.has_value()) {
      continue;
    }
    if (*items[i].start_ms > after_ms) {
      return items[i].start_ms;
    }
  }
  return std::nullopt;
}

std::int64_t word_end_ms(const std::vector<LyricItem> &items,
                         std::size_t index,
                         std::optional<std::int64_t> line_end) {
  const LyricItem &item = items[index];
  const std::int64_t start = item.start_ms.value_or(0);
  if (item.end_ms.has_value() && *item.end_ms > start) {
    return *item.end_ms;
  }
  if (const std::optional<std::int64_t> next = next_word_start(items, index, start);
      next.has_value()) {
    return *next;
  }
  if (line_end.has_value() && *line_end > start) {
    return *line_end;
  }
  // 没有任何可用回退时也必须给 end：极端的 0 时长词退化成 1ms。
  return start + 1;
}

std::int64_t ruby_text_end_ms(const std::vector<LyricWord> &texts,
                              std::size_t index,
                              std::optional<std::int64_t> line_end) {
  const LyricWord &text = texts[index];
  if (text.end_ms.has_value() && *text.end_ms > text.start_ms) {
    return *text.end_ms;
  }
  for (std::size_t next = index + 1; next < texts.size(); ++next) {
    if (texts[next].start_ms > text.start_ms) {
      return texts[next].start_ms;
    }
  }
  if (line_end.has_value() && *line_end > text.start_ms) {
    return *line_end;
  }
  return text.start_ms + 1;
}

amll::Item make_text_item(const std::string &text) {
  amll::Item item;
  item.kind = amll::ItemKind::Text;
  item.text = text;
  return item;
}

void record_end(std::optional<std::int64_t> &max_end, std::int64_t value);
void scan_rendered_end(const amll::Item &item,
                       std::optional<std::int64_t> &max_end);

std::vector<amll::Item>
convert_items(const Lyrics &lyrics, const LyricsTtmlMeta &meta,
              std::size_t line_index, const std::vector<LyricItem> &items,
              std::optional<std::int64_t> parent_end) {
  const std::optional<std::int64_t> line_end =
      resolved_line_end(lyrics, meta, line_index);
  const std::optional<std::int64_t> fallback_end =
      parent_end.has_value() ? parent_end : line_end;

  std::vector<amll::Item> out;
  out.reserve(items.size());
  for (std::size_t i = 0; i < items.size(); ++i) {
    const LyricItem &item = items[i];
    switch (item.kind) {
    case LyricItemKind::Text:
      out.push_back(make_text_item(item.text));
      break;

    case LyricItemKind::Word: {
      amll::Item word;
      word.kind = amll::ItemKind::Word;
      word.text = item.text;
      word.begin_ms = item.start_ms;
      word.end_ms = word_end_ms(items, i, fallback_end);
      out.push_back(std::move(word));
      break;
    }

    case LyricItemKind::Ruby: {
      amll::Item ruby;
      ruby.kind = amll::ItemKind::Ruby;
      ruby.ruby.base = item.ruby.base;
      ruby.ruby.texts.reserve(item.ruby.texts.size());
      for (std::size_t t = 0; t < item.ruby.texts.size(); ++t) {
        const LyricWord &source = item.ruby.texts[t];
        amll::RubyText text;
        text.text = source.text;
        text.begin_ms = source.start_ms;
        text.end_ms = ruby_text_end_ms(item.ruby.texts, t, fallback_end);
        ruby.ruby.texts.push_back(std::move(text));
      }
      out.push_back(std::move(ruby));
      break;
    }

    case LyricItemKind::Background: {
      amll::Item background;
      background.kind = amll::ItemKind::Background;
      background.begin_ms = item.start_ms;
      std::optional<std::int64_t> background_end = item.end_ms;
      // x-bg 一旦写了 begin，也必须有一个更晚的 end，否则同样是非法 span。
      if (item.start_ms.has_value() &&
          (!background_end.has_value() ||
           *background_end <= *item.start_ms)) {
        background_end = fallback_end.has_value() &&
                                 *fallback_end > *item.start_ms
                             ? fallback_end
                             : std::optional<std::int64_t>(
                                   *item.start_ms + 1);
      }
      background.end_ms = background_end;
      background.children = convert_items(lyrics, meta, line_index,
                                          item.children, background_end);
      std::optional<std::int64_t> child_end;
      for (const amll::Item &child : background.children) {
        scan_rendered_end(child, child_end);
      }
      if (child_end.has_value() &&
          (background.begin_ms.has_value() ||
           background.end_ms.has_value()) &&
          (!background.end_ms.has_value() ||
           *background.end_ms < *child_end)) {
        background.end_ms = child_end;
      }
      out.push_back(std::move(background));
      break;
    }

    case LyricItemKind::Translation:
    case LyricItemKind::Romanization: {
      amll::Item aux;
      aux.kind = item.kind == LyricItemKind::Translation
                     ? amll::ItemKind::Translation
                     : amll::ItemKind::Roman;
      aux.text = item.text;
      aux.begin_ms = item.start_ms;
      aux.end_ms = item.end_ms;
      if (aux.begin_ms.has_value() &&
          (!aux.end_ms.has_value() || *aux.end_ms <= *aux.begin_ms)) {
        aux.end_ms = fallback_end.has_value() && *fallback_end > *aux.begin_ms
                         ? fallback_end
                         : std::optional<std::int64_t>(*aux.begin_ms + 1);
      }
      aux.lang = item.lang;
      if (!item.children.empty()) {
        // 逐字辅助轨：文本与子项都可能存在，文本按原顺序放最前。
        if (!item.text.empty()) {
          aux.children.push_back(make_text_item(item.text));
        }
        const std::vector<amll::Item> children =
            convert_items(lyrics, meta, line_index, item.children, parent_end);
        for (const amll::Item &child : children) {
          aux.children.push_back(child);
        }
        std::optional<std::int64_t> child_end;
        for (const amll::Item &child : aux.children) {
          scan_rendered_end(child, child_end);
        }
        if (child_end.has_value() &&
            (aux.begin_ms.has_value() || aux.end_ms.has_value()) &&
            (!aux.end_ms.has_value() || *aux.end_ms < *child_end)) {
          aux.end_ms = child_end;
        }
      }
      out.push_back(std::move(aux));
      break;
    }
    }
  }
  return out;
}

void record_end(std::optional<std::int64_t> &max_end, std::int64_t value) {
  if (!max_end.has_value() || value > *max_end) {
    max_end = value;
  }
}

void scan_rendered_end(const amll::Item &item,
                       std::optional<std::int64_t> &max_end) {
  if (item.end_ms.has_value()) {
    record_end(max_end, *item.end_ms);
  }
  // 兜底：任何带 begin 的 span 都必须有比 begin 更晚的 end。
  if (item.begin_ms.has_value() &&
      (!item.end_ms.has_value() || *item.end_ms <= *item.begin_ms)) {
    record_end(max_end, *item.begin_ms + 1);
  }
  for (const amll::RubyText &text : item.ruby.texts) {
    record_end(max_end, text.end_ms);
    if (text.end_ms <= text.begin_ms) {
      record_end(max_end, text.begin_ms + 1);
    }
  }
  for (const amll::Item &child : item.children) {
    scan_rendered_end(child, max_end);
  }
}

} // namespace

std::string lyrics_to_ttml(const Lyrics &lyrics,
                           const LyricsTtmlMeta &meta) {
  amll::Document document;

  if (lyrics.language.has_value() && !lyrics.language->empty()) {
    document.lang = lyrics.language;
  }
  document.timing =
      has_mode(lyrics, LyricsMode::WordSynced) ? "Word" : "Line";

  if (!lyrics.lines.empty()) {
    document.agents.push_back(amll::Agent{kDefaultAgent, "person", ""});
  }

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

  amll::Section section;
  std::optional<std::int64_t> section_end;
  std::optional<std::int64_t> content_end = meta.duration_ms;
  if (!lyrics.lines.empty()) {
    section.begin_ms = lyrics.lines.front().start_ms;
  }
  section.lines.reserve(lyrics.lines.size());
  for (std::size_t i = 0; i < lyrics.lines.size(); ++i) {
    const LyricLine &line = lyrics.lines[i];
    amll::Line out;
    out.key = line_key(i);
    out.agent = kDefaultAgent;
    out.begin_ms = line.start_ms;
    out.end_ms = resolved_line_end(lyrics, meta, i);
    out.items = convert_items(lyrics, meta, i, line.items, out.end_ms);
    // 行 end 必须覆盖所有子项：零时长词被补成 1ms 后可能越过行尾。
    std::optional<std::int64_t> item_end;
    for (const amll::Item &item : out.items) {
      scan_rendered_end(item, item_end);
    }
    if (item_end.has_value() &&
        (!out.end_ms.has_value() || *out.end_ms < *item_end)) {
      out.end_ms = item_end;
    }
    if (out.end_ms.has_value()) {
      record_end(section_end, *out.end_ms);
      record_end(content_end, *out.end_ms);
    }
    section.lines.push_back(std::move(out));
  }
  if (!section.lines.empty()) {
    section.end_ms = section_end;
    if (!section.end_ms.has_value() && section.begin_ms.has_value()) {
      section.end_ms = *section.begin_ms + 1;
    }
    document.sections.push_back(std::move(section));
  }
  document.body_dur_ms = content_end;

  return amll::serialize(document);
}

} // namespace media_library
