#include "serve/tool_call_parser.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <string_view>

namespace ninfer::serve {
namespace {

using Json = nlohmann::json;

constexpr std::string_view kToolOpen    = "<tool_call>";
constexpr std::string_view kToolClose   = "</tool_call>";
constexpr std::string_view kFunctionOpen  = "<function=";
constexpr std::string_view kFunctionClose = "</function>";

std::string trim_ascii(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) { --end; }
    return std::string(text.substr(begin, end - begin));
}

std::string rtrim_ascii(std::string_view text) {
    std::size_t end = text.size();
    while (end != 0 && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) { --end; }
    return std::string(text.substr(0, end));
}

void skip_ws(std::string_view text, std::size_t& pos) {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) { ++pos; }
}

bool starts_with_at(std::string_view text, std::size_t pos, std::string_view prefix) {
    return pos <= text.size() && text.substr(pos, prefix.size()) == prefix;
}

std::size_t longest_suffix_prefix(std::string_view text, std::string_view marker) {
    const std::size_t maximum = std::min(text.size(), marker.size() - 1);
    for (std::size_t size = maximum; size != 0; --size) {
        if (text.substr(text.size() - size) == marker.substr(0, size)) { return size; }
    }
    return 0;
}

bool valid_function_name(std::string_view name, std::size_t max_name_length) {
    if (name.empty() || name.size() > max_name_length) { return false; }
    for (const unsigned char c : name) {
        if (std::isalnum(c) == 0 && c != '_' && c != '-') { return false; }
    }
    return true;
}

std::string new_tool_call_id() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<std::uint64_t> dist;
    std::array<char, 32> buf{};
    std::snprintf(buf.data(), buf.size(), "call_%016llx",
                  static_cast<unsigned long long>(dist(rng)));
    return std::string(buf.data());
}

bool parse_parameter(std::string_view inner, std::size_t& pos, Json& args) {
    constexpr std::string_view kParamOpen  = "<parameter=";
    constexpr std::string_view kParamClose = "</parameter>";
    if (!starts_with_at(inner, pos, kParamOpen)) { return false; }
    const std::size_t name_begin = pos + kParamOpen.size();
    const std::size_t name_end   = inner.find('>', name_begin);
    if (name_end == std::string_view::npos || name_end == name_begin) { return false; }
    const std::string key       = std::string(inner.substr(name_begin, name_end - name_begin));
    pos                         = name_end + 1;
    const std::size_t value_end = inner.find(kParamClose, pos);
    if (value_end == std::string_view::npos) { return false; }
    const std::string raw_value = trim_ascii(inner.substr(pos, value_end - pos));
    Json parsed                 = Json::parse(raw_value, nullptr, false);
    args[key]                   = parsed.is_discarded() ? Json(raw_value) : parsed;
    pos                         = value_end + kParamClose.size();
    return true;
}

bool parse_one_tool_call(std::string_view block, std::size_t max_name_length, ToolCall& out) {
    std::size_t pos = 0;
    skip_ws(block, pos);
    if (!starts_with_at(block, pos, kFunctionOpen)) { return false; }
    const std::size_t name_begin = pos + kFunctionOpen.size();
    const std::size_t name_end   = block.find('>', name_begin);
    if (name_end == std::string_view::npos || name_end == name_begin) { return false; }
    const std::string name = std::string(block.substr(name_begin, name_end - name_begin));
    if (!valid_function_name(name, max_name_length)) { return false; }
    pos = name_end + 1;

    const std::size_t function_end = block.find(kFunctionClose, pos);
    if (function_end == std::string_view::npos) { return false; }
    const std::string_view params = block.substr(pos, function_end - pos);
    Json args                     = Json::object();
    std::size_t param_pos         = 0;
    for (;;) {
        skip_ws(params, param_pos);
        if (param_pos >= params.size()) { break; }
        if (!parse_parameter(params, param_pos, args)) { return false; }
    }

    pos = function_end + kFunctionClose.size();
    skip_ws(block, pos);
    if (pos != block.size()) { return false; }

    out.id             = new_tool_call_id();
    out.name           = name;
    out.arguments_json = args.dump();
    return true;
}

// Salvage path for "bare function-call" drift: the model omits the canonical
// <tool_call> wrapper and emits <function=...>...</function> blocks directly.
// This is a known failure mode when the surrounding agent transcript contains
// foreign tool-call formats the model imitates. The block shape is still
// unambiguous, so a strict structural check lets us rescue the call instead of
// returning prose that silently ends the agent turn.
//
// Strictness contract (any violation -> no salvage, verbatim text):
//   * lead-in prose before the first block is short (<400 chars) and carries
//     no code-fence / indented-code cues (an echo of this format in ordinary
//     content almost always sits inside a fenced or indented code sample);
//   * every block is a fully well-formed <function=...><parameter=...>
//     </parameter>...</function> construct (parse_one_tool_call, reused);
//   * between and after blocks only whitespace and stray "</tool_call>"
//     wrappers are tolerated (the drift sometimes keeps the closer).
bool lead_in_may_precede_salvaged_call(std::string_view lead) {
    while (!lead.empty() && std::isspace(static_cast<unsigned char>(lead.back())) != 0) {
        lead.remove_suffix(1);
    }
    if (lead.size() >= 400) { return false; }
    if (lead.find("```") != std::string_view::npos) { return false; }
    if (lead.find("\n\t") != std::string_view::npos) { return false; }
    if (lead.find("\n    ") != std::string_view::npos) { return false; }
    return true;
}

bool try_salvage_bare_function_calls(const std::string& text, std::size_t max_tool_name_length,
                                     ParsedToolCallOutput& out) {
    const std::size_t first = text.find(kFunctionOpen);
    if (first == std::string::npos) { return false; }
    if (!lead_in_may_precede_salvaged_call(std::string_view(text).substr(0, first))) { return false; }

    ParsedToolCallOutput candidate;
    std::size_t pos = first;
    while (pos < text.size()) {
        skip_ws(text, pos);
        while (starts_with_at(text, pos, kToolClose)) {
            pos += kToolClose.size();
            skip_ws(text, pos);
        }
        if (pos >= text.size()) { break; }
        if (!starts_with_at(text, pos, kFunctionOpen)) { return false; }
        const std::size_t block_end = text.find(kFunctionClose, pos);
        if (block_end == std::string_view::npos) { return false; }
        ToolCall call;
        if (!parse_one_tool_call(std::string_view(text).substr(pos, block_end + kFunctionClose.size() - pos),
                                 max_tool_name_length, call)) {
            return false;
        }
        candidate.tool_calls.push_back(std::move(call));
        pos = block_end + kFunctionClose.size();
    }

    if (candidate.tool_calls.empty()) { return false; }
    candidate.is_tool_call_response = true;
    std::string content = rtrim_ascii(std::string_view(text).substr(0, first));
    // Stray "</tool_call>" wrappers sometimes leak into the lead-in; they are
    // drift debris, not content.
    while (content.size() >= kToolClose.size() && content.compare(
               content.size() - kToolClose.size(), kToolClose.size(), kToolClose) == 0) {
        content.resize(content.size() - kToolClose.size());
        content = rtrim_ascii(content);
    }
    candidate.content = std::move(content);
    out               = std::move(candidate);
    return true;
}

// Lax parameter extraction for malformed (unclosed) blocks. Finds
// "<parameter=name>" markers, takes the value up to the next marker or
// closing tag, trims, JSON-parses or keeps string. Does NOT require
// </parameter> closers. Returns false if nothing valid found.
bool salvage_parameters(std::string_view inner, Json& args) {
    constexpr std::string_view kParamOpen = "<parameter=";
    std::size_t pos = 0;
    bool found_any = false;
    while (pos < inner.size()) {
        const std::size_t open = inner.find(kParamOpen, pos);
        if (open == std::string_view::npos) { break; }
        const std::size_t name_begin = open + kParamOpen.size();
        const std::size_t name_end   = inner.find('>', name_begin);
        if (name_end == std::string_view::npos || name_end == name_begin) { return false; }
        const std::string key = std::string(inner.substr(name_begin, name_end - name_begin));

        // Value runs to the next parameter marker, or any closing tag, or
        // end of block — whichever comes first.
        std::size_t value_end = inner.size();
        const std::size_t next_param = inner.find(kParamOpen, name_end + 1);
        if (next_param != std::string_view::npos && next_param < value_end) { value_end = next_param; }
        const std::size_t f_close = inner.find("</function>", name_end + 1);
        if (f_close != std::string_view::npos && f_close < value_end) { value_end = f_close; }
        const std::size_t t_close = inner.find("</tool_call>", name_end + 1);
        if (t_close != std::string_view::npos && t_close < value_end) { value_end = t_close; }
        const std::size_t p_close = inner.find("</parameter>", name_end + 1);
        if (p_close != std::string_view::npos && p_close < value_end) { value_end = p_close; }

        const std::string raw_value = trim_ascii(inner.substr(name_end + 1, value_end - name_end - 1));
        Json parsed = Json::parse(raw_value, nullptr, false);
        args[key]   = parsed.is_discarded() ? Json(raw_value) : parsed;
        found_any = true;
        pos = value_end;
    }
    return found_any;
}

// Lax single-block rescue: the model produced a <tool_call> wrapper but the
// interior is malformed (missing </function> / </parameter> closers).
// Strategy: locate "<function=name>", extract name, then salvage all
// "<parameter=name>..." values. Zero tolerance for structural junk —
// if we can't find a valid function name or any parameter, no rescue.
bool try_salvage_malformed_block(std::string_view block, std::size_t max_tool_name_length,
                                 ToolCall& out) {
    constexpr std::string_view kFunctionOpen = "<function=";
    const std::size_t fn_pos = block.find(kFunctionOpen);
    if (fn_pos == std::string_view::npos) { return false; }
    const std::size_t name_begin = fn_pos + kFunctionOpen.size();
    const std::string_view rest  = block.substr(name_begin);
    const std::size_t name_end   = rest.find('>');
    if (name_end == std::string_view::npos || name_end == 0) { return false; }
    const std::string name = std::string(rest.substr(0, name_end));
    if (!valid_function_name(name, max_tool_name_length)) { return false; }

    Json args = Json::object();
    const std::string_view params = rest.substr(name_end + 1);
    if (!salvage_parameters(params, args)) { return false; }

    out.id             = new_tool_call_id();
    out.name           = name;
    out.arguments_json = args.dump();
    return true;
}

ParsedToolCallOutput fallback(const std::string& text) {
    ParsedToolCallOutput out;
    out.content = text;
    return out;
}

} // namespace

ParsedToolCallOutput parse_qwen_tool_call_output(const std::string& text,
                                                 std::size_t max_tool_name_length) {
    const std::size_t first = text.find(kToolOpen);
    if (first == std::string::npos) {
        ParsedToolCallOutput salvaged;
        if (try_salvage_bare_function_calls(text, max_tool_name_length, salvaged)) {
            return salvaged;
        }
        return fallback(text);
    }

    ParsedToolCallOutput out;
    out.content = rtrim_ascii(std::string_view(text).substr(0, first));

    // Lead-in rules: long essays or fenced code preceding the wrapper means
    // this is an example being discussed, not an attempted call — refuse
    // rescue and pass through verbatim.
    const bool lead_ok = lead_in_may_precede_salvaged_call(std::string_view(text).substr(0, first));

    std::size_t pos = first;
    while (pos < text.size()) {
        skip_ws(text, pos);
        if (pos >= text.size()) { break; }
        if (!starts_with_at(text, pos, kToolOpen)) { return fallback(text); }
        const std::size_t inner_begin = pos + kToolOpen.size();
        const std::size_t close       = text.find(kToolClose, inner_begin);
        if (close == std::string::npos) { return fallback(text); }  // truncated: refuse
        const std::string_view inner = std::string_view(text).substr(inner_begin, close - inner_begin);
        ToolCall call;
        if (!parse_one_tool_call(inner, max_tool_name_length, call)) {
            // Strict parse failed. Attempt bounded salvage: valid function
            // name + extractable parameters, short clean lead-in only.
            if (!lead_ok || !try_salvage_malformed_block(inner, max_tool_name_length, call)) {
                return fallback(text);
            }
        }
        out.tool_calls.push_back(std::move(call));
        pos = close + kToolClose.size();
    }

    if (out.tool_calls.empty()) { return fallback(text); }
    out.is_tool_call_response = true;
    return out;
}

std::string ToolCallStreamFilter::feed(std::string_view text) {
    if (finished_) { throw std::logic_error("tool-call stream filter is already finished"); }
    if (text.empty()) { return {}; }
    if (saw_tool_marker_) {
        tool_region_.append(text);
        return {};
    }

    pending_.append(text);
    // Hold from the earliest of the two markers: the canonical <tool_call>
    // wrapper or a drifted bare <function=...> block. Holding on a false
    // positive is harmless: at terminal time the parser classifies the output
    // and finish() flushes a non-tool region verbatim.
    std::size_t marker    = pending_.find(kToolOpen);
    const std::size_t fmarker = pending_.find(kFunctionOpen);
    if (fmarker != std::string::npos && (marker == std::string::npos || fmarker < marker)) {
        marker = fmarker;
    }
    if (marker != std::string::npos) {
        std::size_t safe_end = marker;
        while (safe_end != 0 &&
               std::isspace(static_cast<unsigned char>(pending_[safe_end - 1])) != 0) {
            --safe_end;
        }
        std::string visible = pending_.substr(0, safe_end);
        tool_region_        = pending_.substr(safe_end);
        pending_.clear();
        saw_tool_marker_ = true;
        emitted_bytes_ += visible.size();
        return visible;
    }

    const std::size_t prefix = std::max(longest_suffix_prefix(pending_, kToolOpen),
                                        longest_suffix_prefix(pending_, kFunctionOpen));
    std::size_t safe_end     = pending_.size() - prefix;
    while (safe_end != 0 && std::isspace(static_cast<unsigned char>(pending_[safe_end - 1])) != 0) {
        --safe_end;
    }
    std::string visible = pending_.substr(0, safe_end);
    pending_.erase(0, safe_end);
    emitted_bytes_ += visible.size();
    return visible;
}

std::string ToolCallStreamFilter::finish(bool is_tool_call_response) {
    if (finished_) { throw std::logic_error("tool-call stream filter is already finished"); }
    finished_ = true;
    if (is_tool_call_response) {
        pending_.clear();
        tool_region_.clear();
        return {};
    }
    std::string tail = std::move(pending_);
    tail += tool_region_;
    tool_region_.clear();
    emitted_bytes_ += tail.size();
    return tail;
}

} // namespace ninfer::serve
