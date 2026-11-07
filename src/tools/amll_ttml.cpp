#include "tools/amll_ttml.h"

#include "pugixml.hpp"

#include <cctype>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace amll {
namespace {

// ---------------------------------------------------------------------------
// 文本小工具
// ---------------------------------------------------------------------------

bool is_space(char c) {
  return std::isspace(static_cast<unsigned char>(c)) != 0;
}

std::string trim(const std::string &text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && is_space(text[begin])) {
    ++begin;
  }
  while (end > begin && is_space(text[end - 1])) {
    --end;
  }
  return text.substr(begin, end - begin);
}

// pugixml 不解析命名空间：`ttm:agent` 这种限定名是逐字保存的。这里只在
// 明确知道带前缀的地方做"去前缀"比较（Apple 的 iTunesMetadata 子元素）。
std::string local_name(const std::string &qualified) {
  const std::size_t colon = qualified.rfind(':');
  if (colon == std::string::npos) {
    return qualified;
  }
  return qualified.substr(colon + 1);
}

// ---------------------------------------------------------------------------
// 属性读取。pugixml 的 as_string() 对缺失属性返回空串，无法区分"没有该属性"
// 和"属性值为空"，所以统一走 optional。
// ---------------------------------------------------------------------------

std::optional<std::string> opt_attr(const pugi::xml_node &node,
                                    const char *name) {
  const pugi::xml_attribute attribute = node.attribute(name);
  if (attribute.empty()) {
    return std::nullopt;
  }
  return std::string(attribute.value());
}

std::string attr(const pugi::xml_node &node, const char *name) {
  const std::optional<std::string> value = opt_attr(node, name);
  return value.has_value() ? *value : std::string();
}

std::optional<std::int64_t> time_attr(const pugi::xml_node &node,
                                      const char *name) {
  const std::optional<std::string> value = opt_attr(node, name);
  if (!value.has_value()) {
    return std::nullopt;
  }
  return parse_timestamp(*value);
}

// <div itunes:songPart="Verse">；旧写法 itunes:song-part 仅读时兼容。
std::string song_part_of(const pugi::xml_node &node) {
  const std::optional<std::string> current = opt_attr(node, "itunes:songPart");
  if (current.has_value()) {
    return *current;
  }
  const std::optional<std::string> legacy = opt_attr(node, "itunes:song-part");
  return legacy.has_value() ? *legacy : std::string();
}

bool is_text_node(const pugi::xml_node &node) {
  return node.type() == pugi::node_pcdata || node.type() == pugi::node_cdata;
}

// ---------------------------------------------------------------------------
// 内容解析：<p>、<span ttm:role="x-bg">、<text for> 都是同一套混合内容规则。
// ---------------------------------------------------------------------------

class ContentParser {
public:
  // trim_direct_text：该层直接出现的 PCDATA 是否要 trim。
  // 行级文本（<p> 直文、<text for> 直文、x-bg 内的直文、无时间的 span）来自
  // 格式化后的文件，首尾空白要清掉；带时间轴的音节文本不能 trim，音节之间
  // 的空格就是原文。
  std::vector<Item> parse_content(const pugi::xml_node &parent,
                                  bool trim_direct_text) {
    std::vector<Item> items;
    for (pugi::xml_node child = parent.first_child(); child;
         child = child.next_sibling()) {
      if (is_text_node(child)) {
        append_text(child.value(), trim_direct_text, items);
        continue;
      }
      if (child.type() != pugi::node_element) {
        continue;
      }
      append_span(child, items);
    }
    return items;
  }

private:
  void append_text(const std::string &raw, bool trim_direct_text,
                   std::vector<Item> &items) {
    const std::string text = trim_direct_text ? trim(raw) : raw;
    if (text.empty()) {
      return;
    }
    Item item;
    item.kind = ItemKind::Text;
    item.text = text;
    items.push_back(std::move(item));
  }

  // 一个元素（<span>）的直接文本，不递归。
  static std::string direct_text(const pugi::xml_node &node) {
    std::string text;
    for (pugi::xml_node child = node.first_child(); child;
         child = child.next_sibling()) {
      if (is_text_node(child)) {
        text += child.value();
      }
    }
    return text;
  }

  void append_span(const pugi::xml_node &span, std::vector<Item> &items) {
    const std::string ruby_kind = attr(span, "tts:ruby");
    if (ruby_kind == "container") {
      items.push_back(parse_ruby(span));
      return;
    }

    const std::string role = attr(span, "ttm:role");
    if (role == "x-bg") {
      Item item;
      item.kind = ItemKind::Background;
      item.begin_ms = time_attr(span, "begin");
      item.end_ms = time_attr(span, "end");
      // 背景人声内部可能是逐行直文，也可能是带时间轴的 span。
      item.children = parse_content(span, true);
      items.push_back(std::move(item));
      return;
    }

    if (role == "x-translation" || role == "x-roman") {
      Item item;
      item.kind = role == "x-translation" ? ItemKind::Translation : ItemKind::Roman;
      // 规范里这两种 span 的 begin/end 语义被忽略，但仍原样保留，避免序列化
      // 时悄悄丢掉作者写下的时间。
      item.begin_ms = time_attr(span, "begin");
      item.end_ms = time_attr(span, "end");
      if (const std::optional<std::string> lang = opt_attr(span, "xml:lang");
          lang.has_value() && !lang->empty()) {
        item.lang = *lang;
      }
      // 逐字翻译/标音会在 x-translation / x-roman 里再嵌带时间的 span；
      // 有元素子项时按混合内容递归解析，纯文本才放 text。
      bool has_element = false;
      for (pugi::xml_node child = span.first_child(); child;
           child = child.next_sibling()) {
        if (child.type() == pugi::node_element) {
          has_element = true;
          break;
        }
      }
      if (has_element) {
        item.children = parse_content(span, true);
      } else {
        item.text = trim(direct_text(span));
      }
      items.push_back(std::move(item));
      return;
    }

    // 普通 span：带时间轴就是逐字音节，否则是行级文本容器。
    const std::optional<std::int64_t> begin = time_attr(span, "begin");
    const std::optional<std::int64_t> end = time_attr(span, "end");
    if (begin.has_value() || end.has_value()) {
      Item item;
      item.kind = ItemKind::Word;
      item.begin_ms = begin;
      item.end_ms = end;
      item.text = direct_text(span);
      items.push_back(std::move(item));
      return;
    }

    // 无时间的 span 只是包装：把里面的内容摊平到当前层。
    const std::vector<Item> children = parse_content(span, true);
    for (const Item &child : children) {
      items.push_back(child);
    }
  }

  // <span tts:ruby="container">：
  //   <span tts:ruby="base">漢字</span>
  //   <span tts:ruby="textContainer"><span tts:ruby="text" begin end>かん</span>…</span>
  static Item parse_ruby(const pugi::xml_node &container) {
    Item item;
    item.kind = ItemKind::Ruby;

    pugi::xml_node text_container;
    for (pugi::xml_node child = container.first_child(); child;
         child = child.next_sibling()) {
      if (child.type() != pugi::node_element) {
        continue;
      }
      const std::string kind = attr(child, "tts:ruby");
      if (kind == "base") {
        item.ruby.base = trim(direct_text(child));
      } else if (kind == "textContainer") {
        text_container = child;
      }
    }

    // 个别文件省掉 textContainer 直接把 text 挂在 container 下，两种都认。
    const pugi::xml_node source =
        text_container ? text_container : container;
    for (pugi::xml_node child = source.first_child(); child;
         child = child.next_sibling()) {
      if (child.type() != pugi::node_element) {
        continue;
      }
      if (attr(child, "tts:ruby") != "text") {
        continue;
      }
      RubyText text;
      text.text = direct_text(child);
      text.begin_ms = time_attr(child, "begin").value_or(0);
      text.end_ms = time_attr(child, "end").value_or(0);
      item.ruby.texts.push_back(std::move(text));
    }
    return item;
  }
};

// ---------------------------------------------------------------------------
// head/metadata
// ---------------------------------------------------------------------------

void parse_aux_track(const pugi::xml_node &node, bool with_type,
                     std::vector<AuxTrack> &out) {
  for (pugi::xml_node child = node.first_child(); child;
       child = child.next_sibling()) {
    if (child.type() != pugi::node_element ||
        local_name(child.name()) != (with_type ? "translation" : "transliteration")) {
      continue;
    }
    AuxTrack track;
    if (with_type) {
      track.type = attr(child, "type");
    }
    if (const std::optional<std::string> lang = opt_attr(child, "xml:lang");
        lang.has_value() && !lang->empty()) {
      track.lang = *lang;
    }
    ContentParser parser;
    for (pugi::xml_node text = child.first_child(); text;
         text = text.next_sibling()) {
      if (text.type() != pugi::node_element ||
          local_name(text.name()) != "text") {
        continue;
      }
      AuxText item;
      item.ref = attr(text, "for");
      item.items = parser.parse_content(text, true);
      track.texts.push_back(std::move(item));
    }
    out.push_back(std::move(track));
  }
}

void parse_itunes_metadata(const pugi::xml_node &node, Document &document) {
  for (pugi::xml_node child = node.first_child(); child;
       child = child.next_sibling()) {
    if (child.type() != pugi::node_element) {
      continue;
    }
    const std::string name = local_name(child.name());
    if (name == "translations") {
      parse_aux_track(child, true, document.translations);
    } else if (name == "transliterations") {
      parse_aux_track(child, false, document.transliterations);
    } else if (name == "songwriters") {
      for (pugi::xml_node writer = child.first_child(); writer;
           writer = writer.next_sibling()) {
        if (writer.type() != pugi::node_element ||
            local_name(writer.name()) != "songwriter") {
          continue;
        }
        std::string text;
        for (pugi::xml_node part = writer.first_child(); part;
             part = part.next_sibling()) {
          if (is_text_node(part)) {
            text += part.value();
          }
        }
        document.songwriters.push_back(trim(text));
      }
    }
  }
}

void parse_metadata(const pugi::xml_node &metadata, Document &document) {
  for (pugi::xml_node child = metadata.first_child(); child;
       child = child.next_sibling()) {
    if (child.type() != pugi::node_element) {
      continue;
    }
    const std::string name = child.name();
    if (name == "ttm:title") {
      document.title = trim(child.text().as_string());
      continue;
    }
    if (name == "ttm:agent") {
      Agent agent;
      agent.id = attr(child, "xml:id");
      agent.type = attr(child, "type");
      // <ttm:name type="full"> 优先；只有其它 type 时退而取第一个。
      for (pugi::xml_node entry = child.first_child(); entry;
           entry = entry.next_sibling()) {
        if (entry.type() != pugi::node_element ||
            entry.name() != std::string("ttm:name")) {
          continue;
        }
        const std::string text = trim(entry.text().as_string());
        if (agent.name.empty() || attr(entry, "type") == "full") {
          agent.name = text;
        }
        if (attr(entry, "type") == "full") {
          break;
        }
      }
      document.agents.push_back(std::move(agent));
      continue;
    }
    if (name == "amll:meta") {
      Meta meta;
      meta.key = attr(child, "key");
      meta.value = attr(child, "value");
      document.metas.push_back(std::move(meta));
      continue;
    }
    if (local_name(name) == "iTunesMetadata") {
      parse_itunes_metadata(child, document);
    }
  }
}

// ---------------------------------------------------------------------------
// body
// ---------------------------------------------------------------------------

Line parse_line(const pugi::xml_node &node, ContentParser &parser) {
  Line line;
  line.begin_ms = time_attr(node, "begin");
  line.end_ms = time_attr(node, "end");
  line.key = attr(node, "itunes:key");
  line.agent = attr(node, "ttm:agent");
  line.items = parser.parse_content(node, true);
  return line;
}

void parse_div(const pugi::xml_node &node, ContentParser &parser,
               std::vector<Section> &sections) {
  Section section;
  section.begin_ms = time_attr(node, "begin");
  section.end_ms = time_attr(node, "end");
  section.song_part = song_part_of(node);
  for (pugi::xml_node child = node.first_child(); child;
       child = child.next_sibling()) {
    if (child.type() != pugi::node_element || child.name() != std::string("p")) {
      continue;
    }
    section.lines.push_back(parse_line(child, parser));
  }
  sections.push_back(std::move(section));
}

void parse_body(const pugi::xml_node &node, Document &document) {
  document.body_dur_ms = time_attr(node, "dur");

  ContentParser parser;
  for (pugi::xml_node child = node.first_child(); child;
       child = child.next_sibling()) {
    if (child.type() != pugi::node_element) {
      continue;
    }
    const std::string name = child.name();
    if (name == "div") {
      parse_div(child, parser, document.sections);
      continue;
    }
    // 规范要求 <p> 放在 <div> 里，但直接挂在 <body> 下的写法也接受：
    // 折成一段"无段落名、无时间"的 section，序列化时不会再补 <div>。
    if (name == "p") {
      if (document.sections.empty() ||
          !document.sections.back().song_part.empty() ||
          document.sections.back().begin_ms.has_value() ||
          document.sections.back().end_ms.has_value()) {
        document.sections.emplace_back();
      }
      document.sections.back().lines.push_back(parse_line(child, parser));
    }
  }
}

// ---------------------------------------------------------------------------
// 序列化
// ---------------------------------------------------------------------------

void escape_text(std::string &out, const std::string &text) {
  for (const char c : text) {
    switch (c) {
    case '&':
      out += "&amp;";
      break;
    case '<':
      out += "&lt;";
      break;
    case '>':
      out += "&gt;";
      break;
    default:
      out += c;
    }
  }
}

void escape_attr(std::string &out, const std::string &text) {
  for (const char c : text) {
    switch (c) {
    case '&':
      out += "&amp;";
      break;
    case '<':
      out += "&lt;";
      break;
    case '>':
      out += "&gt;";
      break;
    case '"':
      out += "&quot;";
      break;
    case '\n':
      out += "&#10;";
      break;
    case '\r':
      out += "&#13;";
      break;
    case '\t':
      out += "&#9;";
      break;
    default:
      out += c;
    }
  }
}

void append_attr(std::string &out, const char *name, const std::string &value) {
  out += ' ';
  out += name;
  out += "=\"";
  escape_attr(out, value);
  out += '"';
}

void append_time_attr(std::string &out, const char *name,
                      const std::optional<std::int64_t> &ms) {
  if (!ms.has_value()) {
    return;
  }
  append_attr(out, name, format_timestamp(*ms));
}

void append_items(std::string &out, const std::vector<Item> &items);

void append_item(std::string &out, const Item &item) {
  switch (item.kind) {
  case ItemKind::Text:
    escape_text(out, item.text);
    return;

  case ItemKind::Word:
    out += "<span";
    append_time_attr(out, "begin", item.begin_ms);
    append_time_attr(out, "end", item.end_ms);
    out += '>';
    escape_text(out, item.text);
    out += "</span>";
    return;

  case ItemKind::Ruby:
    out += "<span tts:ruby=\"container\"><span tts:ruby=\"base\">";
    escape_text(out, item.ruby.base);
    out += "</span><span tts:ruby=\"textContainer\">";
    for (const RubyText &text : item.ruby.texts) {
      out += "<span tts:ruby=\"text\"";
      append_attr(out, "begin", format_timestamp(text.begin_ms));
      append_attr(out, "end", format_timestamp(text.end_ms));
      out += '>';
      escape_text(out, text.text);
      out += "</span>";
    }
    out += "</span></span>";
    return;

  case ItemKind::Background:
    out += "<span ttm:role=\"x-bg\"";
    append_time_attr(out, "begin", item.begin_ms);
    append_time_attr(out, "end", item.end_ms);
    out += '>';
    append_items(out, item.children);
    out += "</span>";
    return;

  case ItemKind::Translation:
  case ItemKind::Roman:
    out += item.kind == ItemKind::Translation
               ? "<span ttm:role=\"x-translation\""
               : "<span ttm:role=\"x-roman\"";
    if (item.lang.has_value()) {
      append_attr(out, "xml:lang", *item.lang);
    }
    append_time_attr(out, "begin", item.begin_ms);
    append_time_attr(out, "end", item.end_ms);
    out += '>';
    if (!item.children.empty()) {
      append_items(out, item.children);
    } else {
      escape_text(out, item.text);
    }
    out += "</span>";
    return;
  }
}

void append_items(std::string &out, const std::vector<Item> &items) {
  for (const Item &item : items) {
    append_item(out, item);
  }
}

void append_lines(std::string &out, const std::vector<Line> &lines,
                  const std::string &indent) {
  for (const Line &line : lines) {
    out += indent;
    out += "<p";
    append_time_attr(out, "begin", line.begin_ms);
    append_time_attr(out, "end", line.end_ms);
    if (!line.key.empty()) {
      append_attr(out, "itunes:key", line.key);
    }
    if (!line.agent.empty()) {
      append_attr(out, "ttm:agent", line.agent);
    }
    out += '>';
    append_items(out, line.items);
    out += "</p>\n";
  }
}

void append_aux_tracks(std::string &out, const char *wrapper,
                       const char *element, bool with_type,
                       const std::vector<AuxTrack> &tracks) {
  if (tracks.empty()) {
    return;
  }
  out += "      <";
  out += wrapper;
  out += ">\n";
  for (const AuxTrack &track : tracks) {
    out += "        <";
    out += element;
    if (with_type && !track.type.empty()) {
      append_attr(out, "type", track.type);
    }
    if (track.lang.has_value()) {
      append_attr(out, "xml:lang", *track.lang);
    }
    out += ">\n";
    for (const AuxText &text : track.texts) {
      out += "          <text";
      if (!text.ref.empty()) {
        append_attr(out, "for", text.ref);
      }
      out += '>';
      append_items(out, text.items);
      out += "</text>\n";
    }
    out += "        </";
    out += element;
    out += ">\n";
  }
  out += "      </";
  out += wrapper;
  out += ">\n";
}

} // namespace

// ---------------------------------------------------------------------------
// 时间戳
// ---------------------------------------------------------------------------

std::optional<std::int64_t> parse_timestamp(const std::string &text) {
  const std::string value = trim(text);
  if (value.empty()) {
    return std::nullopt;
  }

  // 规范只允许半角冒号、最多两个：ss / mm:ss / hh:mm:ss，末段可带小数。
  std::vector<std::string> parts;
  std::string current;
  for (const char c : value) {
    if (c == ':') {
      parts.push_back(current);
      current.clear();
      continue;
    }
    current += c;
  }
  parts.push_back(current);
  if (parts.size() > 3) {
    return std::nullopt;
  }

  std::int64_t seconds = 0;
  for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
    const std::string &part = parts[i];
    if (part.empty()) {
      return std::nullopt;
    }
    std::int64_t number = 0;
    for (const char c : part) {
      if (!std::isdigit(static_cast<unsigned char>(c))) {
        return std::nullopt;
      }
      number = number * 10 + (c - '0');
    }
    seconds = seconds * 60 + number;
  }

  // 末段：整数秒 + 可选小数（小数位直接按毫秒解释，最多三位）。
  const std::string &tail = parts.back();
  if (tail.empty()) {
    return std::nullopt;
  }
  const std::size_t dot = tail.find('.');
  const std::string whole = dot == std::string::npos ? tail : tail.substr(0, dot);
  if (whole.empty()) {
    return std::nullopt;
  }
  std::int64_t last_seconds = 0;
  for (const char c : whole) {
    if (!std::isdigit(static_cast<unsigned char>(c))) {
      return std::nullopt;
    }
    last_seconds = last_seconds * 10 + (c - '0');
  }
  seconds = seconds * 60 + last_seconds;

  std::int64_t millis = 0;
  if (dot != std::string::npos) {
    std::string fraction;
    for (std::size_t i = dot + 1; i < tail.size(); ++i) {
      const char c = tail[i];
      if (!std::isdigit(static_cast<unsigned char>(c))) {
        return std::nullopt;
      }
      if (fraction.size() < 3) {
        fraction += c;
      }
    }
    while (fraction.size() < 3) {
      fraction += '0';
    }
    for (const char c : fraction) {
      millis = millis * 10 + (c - '0');
    }
  }
  return seconds * 1000 + millis;
}

std::string format_timestamp(std::int64_t ms) {
  if (ms < 0) {
    ms = 0;
  }
  const std::int64_t total_seconds = ms / 1000;
  const std::int64_t millis = ms % 1000;
  const std::int64_t hours = total_seconds / 3600;
  const std::int64_t minutes = (total_seconds % 3600) / 60;
  const std::int64_t seconds = total_seconds % 60;

  char buffer[32];
  if (hours > 0) {
    std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld:%02lld.%03lld",
                  static_cast<long long>(hours), static_cast<long long>(minutes),
                  static_cast<long long>(seconds),
                  static_cast<long long>(millis));
  } else {
    std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld.%03lld",
                  static_cast<long long>(total_seconds / 60),
                  static_cast<long long>(seconds),
                  static_cast<long long>(millis));
  }
  return std::string(buffer);
}

// ---------------------------------------------------------------------------
// Document 辅助
// ---------------------------------------------------------------------------

std::optional<std::string> Document::meta(const std::string &key) const {
  for (const Meta &item : metas) {
    if (item.key == key) {
      return item.value;
    }
  }
  return std::nullopt;
}

void Document::set_meta(const std::string &key, const std::string &value) {
  for (Meta &item : metas) {
    if (item.key == key) {
      item.value = value;
      return;
    }
  }
  metas.push_back(Meta{key, value});
}

// ---------------------------------------------------------------------------
// 解析入口
// ---------------------------------------------------------------------------

std::optional<Document> parse(const std::string &xml, std::string *error) {
  pugi::xml_document tree;
  // 默认 flags 会丢弃纯空白 PCDATA，正好让格式化文件的换行缩进不混进歌词；
  // 有内容的文本节点仍然保留，行级文本各自 trim。
  const pugi::xml_parse_result result = tree.load_string(xml.c_str());
  if (!result) {
    if (error != nullptr) {
      *error = std::string("TTML XML 解析失败: ") + result.description() +
               "（偏移 " + std::to_string(result.offset) + "）";
    }
    return std::nullopt;
  }

  const pugi::xml_node root = tree.document_element();
  if (!root) {
    if (error != nullptr) {
      *error = "TTML 缺少根元素";
    }
    return std::nullopt;
  }
  if (local_name(root.name()) != "tt") {
    if (error != nullptr) {
      *error = std::string("TTML 根元素必须是 <tt>，实际是 <") + root.name() + ">";
    }
    return std::nullopt;
  }

  Document document;
  if (const std::optional<std::string> lang = opt_attr(root, "xml:lang");
      lang.has_value() && !lang->empty()) {
    document.lang = *lang;
  }
  document.timing = attr(root, "itunes:timing");

  for (pugi::xml_node child = root.first_child(); child;
       child = child.next_sibling()) {
    if (child.type() != pugi::node_element) {
      continue;
    }
    const std::string name = child.name();
    if (name == "head") {
      for (pugi::xml_node metadata = child.first_child(); metadata;
           metadata = metadata.next_sibling()) {
        if (metadata.type() == pugi::node_element &&
            metadata.name() == std::string("metadata")) {
          parse_metadata(metadata, document);
        }
      }
      continue;
    }
    if (name == "body") {
      parse_body(child, document);
    }
  }

  if (error != nullptr) {
    error->clear();
  }
  return document;
}

// ---------------------------------------------------------------------------
// 序列化入口
// ---------------------------------------------------------------------------

std::string serialize(const Document &document) {
  std::string out;

  out += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  out += "<tt xmlns=\"http://www.w3.org/ns/ttml\""
         " xmlns:ttm=\"http://www.w3.org/ns/ttml#metadata\""
         " xmlns:tts=\"http://www.w3.org/ns/ttml#styling\""
         " xmlns:itunes=\"http://music.apple.com/lyric-ttml-internal\""
         " xmlns:amll=\"http://www.example.com/ns/amll\"";
  if (document.lang.has_value() && !document.lang->empty()) {
    append_attr(out, "xml:lang", *document.lang);
  }
  if (!document.timing.empty()) {
    append_attr(out, "itunes:timing", document.timing);
  }
  out += ">\n";

  const bool has_itunes = !document.translations.empty() ||
                          !document.transliterations.empty() ||
                          !document.songwriters.empty();
  if (!document.title.empty() || !document.agents.empty() ||
      !document.metas.empty() || has_itunes) {
    out += "  <head>\n    <metadata>\n";

    if (!document.title.empty()) {
      out += "      <ttm:title>";
      escape_text(out, document.title);
      out += "</ttm:title>\n";
    }

    for (const Agent &agent : document.agents) {
      out += "      <ttm:agent";
      if (!agent.type.empty()) {
        append_attr(out, "type", agent.type);
      }
      if (!agent.id.empty()) {
        append_attr(out, "xml:id", agent.id);
      }
      if (agent.name.empty()) {
        out += " />\n";
        continue;
      }
      out += "><ttm:name type=\"full\">";
      escape_text(out, agent.name);
      out += "</ttm:name></ttm:agent>\n";
    }

    for (const Meta &meta : document.metas) {
      out += "      <amll:meta";
      append_attr(out, "key", meta.key);
      append_attr(out, "value", meta.value);
      out += " />\n";
    }

    if (has_itunes) {
      out += "      <iTunesMetadata"
             " xmlns=\"http://music.apple.com/lyric-ttml-internal\">\n";
      append_aux_tracks(out, "translations", "translation", true,
                        document.translations);
      append_aux_tracks(out, "transliterations", "transliteration", false,
                        document.transliterations);
      if (!document.songwriters.empty()) {
        out += "        <songwriters>\n";
        for (const std::string &writer : document.songwriters) {
          out += "          <songwriter>";
          escape_text(out, writer);
          out += "</songwriter>\n";
        }
        out += "        </songwriters>\n";
      }
      out += "      </iTunesMetadata>\n";
    }

    out += "    </metadata>\n  </head>\n";
  }

  out += "  <body";
  append_time_attr(out, "dur", document.body_dur_ms);
  out += ">\n";

  for (const Section &section : document.sections) {
    // AMLL TTML DB 的实际文件统一用 <div> 包住 <p>，即使没有 songPart；
    // 这里也始终输出 div，保持和标准库一致的形状。
    out += "    <div";
    append_time_attr(out, "begin", section.begin_ms);
    append_time_attr(out, "end", section.end_ms);
    if (!section.song_part.empty()) {
      append_attr(out, "itunes:songPart", section.song_part);
    }
    out += ">\n";
    append_lines(out, section.lines, "      ");
    out += "    </div>\n";
  }

  out += "  </body>\n</tt>\n";
  return out;
}

} // namespace amll
