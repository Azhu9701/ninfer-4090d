#include "serve/tool_call_parser.h"

#include <nlohmann/json.hpp>

#include <iostream>
#include <string>

namespace {

using Json = nlohmann::json;

int fail(const std::string& message) {
    std::cerr << "FAIL: " << message << '\n';
    return 1;
}

int check(bool condition, const std::string& message) { return condition ? 0 : fail(message); }

int test_single_call() {
    const ninfer::serve::ParsedToolCallOutput parsed =
        ninfer::serve::parse_qwen_tool_call_output("Calling weather.\n"
                                                   "<tool_call>\n"
                                                   "<function=get_weather>\n"
                                                   "<parameter=city>\nParis\n</parameter>\n"
                                                   "<parameter=days>\n2\n</parameter>\n"
                                                   "</function>\n"
                                                   "</tool_call>",
                                                   64);

    int failures = 0;
    failures += check(parsed.is_tool_call_response, "single call parsed as tool response");
    failures += check(parsed.content == "Calling weather.", "content prefix trimmed");
    failures += check(parsed.tool_calls.size() == 1, "one parsed call");
    failures += check(parsed.tool_calls[0].id.rfind("call_", 0) == 0, "generated call id prefix");
    failures += check(parsed.tool_calls[0].name == "get_weather", "function name parsed");
    const Json args = Json::parse(parsed.tool_calls[0].arguments_json);
    failures += check(args.at("city") == "Paris", "string parameter parsed");
    failures += check(args.at("days") == 2, "number parameter parsed");
    return failures;
}

int test_multiple_calls_and_json_values() {
    const ninfer::serve::ParsedToolCallOutput parsed = ninfer::serve::parse_qwen_tool_call_output(
        "<tool_call>\n"
        "<function=first>\n"
        "<parameter=payload>\n{\"ok\":true,\"items\":[1,2]}\n</parameter>\n"
        "</function>\n"
        "</tool_call>\n"
        "<tool_call>\n"
        "<function=second>\n"
        "<parameter=value>\nplain text\n</parameter>\n"
        "</function>\n"
        "</tool_call>",
        64);

    int failures = 0;
    failures += check(parsed.is_tool_call_response, "multiple calls parsed as tool response");
    failures += check(parsed.tool_calls.size() == 2, "two parsed calls");
    failures += check(parsed.tool_calls[0].name == "first", "first call name");
    failures += check(parsed.tool_calls[1].name == "second", "second call name");
    const Json payload = Json::parse(parsed.tool_calls[0].arguments_json).at("payload");
    failures += check(payload.at("ok") == true, "json object parameter parsed");
    return failures;
}

int test_malformed_falls_back_to_text() {
    const std::string text = "<tool_call>\n<function=broken>";
    const ninfer::serve::ParsedToolCallOutput parsed =
        ninfer::serve::parse_qwen_tool_call_output(text, 64);
    int failures = 0;
    failures += check(!parsed.is_tool_call_response, "malformed tool call falls back to text");
    failures += check(parsed.content == text, "malformed tool call preserves original text");
    return failures;
}

int test_suffix_after_tool_falls_back_to_text() {
    const std::string text = "<tool_call>\n<function=ok>\n</function>\n</tool_call>\ntrailing";
    const ninfer::serve::ParsedToolCallOutput parsed =
        ninfer::serve::parse_qwen_tool_call_output(text, 64);
    int failures = 0;
    failures += check(!parsed.is_tool_call_response, "non-whitespace suffix falls back to text");
    failures += check(parsed.content == text, "suffix fallback preserves text");
    return failures;
}

int test_configured_name_limit() {
    const std::string name(128, 'a');
    const std::string text = "<tool_call>\n<function=" + name + ">\n</function>\n</tool_call>";

    const ninfer::serve::ParsedToolCallOutput anthropic =
        ninfer::serve::parse_qwen_tool_call_output(text, 128);
    const ninfer::serve::ParsedToolCallOutput openai =
        ninfer::serve::parse_qwen_tool_call_output(text, 64);
    const std::string too_long_text =
        "<tool_call>\n<function=" + std::string(129, 'a') + ">\n</function>\n</tool_call>";
    const ninfer::serve::ParsedToolCallOutput too_long =
        ninfer::serve::parse_qwen_tool_call_output(too_long_text, 128);

    int failures = 0;
    failures += check(anthropic.is_tool_call_response && anthropic.tool_calls.size() == 1 &&
                          anthropic.tool_calls[0].name == name,
                      "128-character name accepted with Anthropic limit");
    failures +=
        check(!openai.is_tool_call_response, "128-character name rejected with OpenAI limit");
    failures +=
        check(!too_long.is_tool_call_response, "129-character name rejected with Anthropic limit");
    return failures;
}

int test_incremental_filter_valid_tool() {
    ninfer::serve::ToolCallStreamFilter filter;
    std::string visible;
    visible += filter.feed("Calling weather.  \n<tool_");
    visible += filter.feed("call>\n<function=get_weather>");
    visible += filter.feed("\n</function>\n</tool_call>");
    visible += filter.finish(true);
    int failures = 0;
    failures += check(visible == "Calling weather.",
                      "valid tool filter did not stream the trimmed content prefix");
    failures +=
        check(filter.emitted_bytes() == visible.size(), "valid tool filter byte count mismatch");
    return failures;
}

int test_incremental_filter_fallback() {
    const std::string original = "prefix  \n<tool_call>\n<function=broken>";
    ninfer::serve::ToolCallStreamFilter malformed;
    std::string restored;
    restored += malformed.feed(original.substr(0, 10));
    restored += malformed.feed(original.substr(10));
    restored += malformed.finish(false);

    ninfer::serve::ToolCallStreamFilter normal;
    std::string ordinary;
    ordinary += normal.feed("ordinary text  ");
    ordinary += normal.finish(false);

    int failures = 0;
    failures += check(restored == original, "malformed tool filter fallback lost raw bytes");
    failures +=
        check(ordinary == "ordinary text  ", "ordinary filtered output lost trailing whitespace");
    return failures;
}

// --- bare (wrapper-less) function-call drift salvage ---

int test_bare_block_salvaged() {
    const std::string text =
        "<function=Bash>\n"
        "<parameter=file_path>\n/Users/mac/emibot/emibot-pi/dist/core/sdk.d.ts\n</parameter>\n"
        "</function>";
    const ninfer::serve::ParsedToolCallOutput parsed =
        ninfer::serve::parse_qwen_tool_call_output(text, 128);
    int failures = 0;
    failures += check(parsed.is_tool_call_response, "bare block salvaged as tool response");
    failures += check(parsed.tool_calls.size() == 1, "bare block yields one call");
    failures += check(parsed.tool_calls[0].name == "Bash", "bare block name parsed");
    const Json args = Json::parse(parsed.tool_calls[0].arguments_json);
    failures += check(args.at("file_path") == "/Users/mac/emibot/emibot-pi/dist/core/sdk.d.ts",
                      "bare block path parameter parsed");
    failures += check(parsed.content.empty(), "no lead-in means empty content");
    return failures;
}

int test_bare_block_with_lead_in_and_stray_closer() {
    const std::string text =
        "Let me read that file.\n"
        "</tool_call>\n"
        "<function=Read>\n"
        "<parameter=file_path>\n/tmp/x.txt\n</parameter>\n"
        "</function>\n"
        "</tool_call>\n";
    const ninfer::serve::ParsedToolCallOutput parsed =
        ninfer::serve::parse_qwen_tool_call_output(text, 64);
    int failures = 0;
    failures += check(parsed.is_tool_call_response,
                      "short lead-in + stray closers still salvaged");
    failures += check(parsed.tool_calls.size() == 1, "salvaged one call");
    failures += check(parsed.tool_calls[0].name == "Read", "salvaged call name");
    failures += check(parsed.content == "Let me read that file.", "lead-in kept as content");
    return failures;
}

int test_bare_block_long_lead_in_falls_back() {
    const std::string lead(500, 'x');
    const std::string text =
        lead + "\n<function=Read>\n<parameter=file_path>\n/tmp/x\n</parameter>\n</function>";
    const ninfer::serve::ParsedToolCallOutput parsed =
        ninfer::serve::parse_qwen_tool_call_output(text, 64);
    int failures = 0;
    failures += check(!parsed.is_tool_call_response, "long lead-in refuses salvage");
    failures += check(parsed.content == text, "long lead-in fallback preserves text");
    return failures;
}

int test_bare_block_code_fence_lead_falls_back() {
    const std::string text =
        "Example:\n```xml\n<function=Read>\n<parameter=file_path>\n/tmp/x\n</parameter>\n"
        "</function>\n```\n";
    const ninfer::serve::ParsedToolCallOutput parsed =
        ninfer::serve::parse_qwen_tool_call_output(text, 64);
    int failures = 0;
    failures += check(!parsed.is_tool_call_response, "fenced example refuses salvage");
    failures += check(parsed.content == text, "fenced example fallback preserves text");
    return failures;
}

int test_bare_block_malformed_params_fall_back() {
    const std::string text = "<function=Read>\njunk instead of parameters\n</function>";
    const ninfer::serve::ParsedToolCallOutput parsed =
        ninfer::serve::parse_qwen_tool_call_output(text, 64);
    int failures = 0;
    failures += check(!parsed.is_tool_call_response, "malformed bare block refuses salvage");
    failures += check(parsed.content == text, "malformed bare block preserves text");
    return failures;
}

int test_bare_block_trailing_prose_falls_back() {
    const std::string text =
        "<function=Read>\n<parameter=file_path>\n/tmp/x\n</parameter>\n</function>\nDone.";
    const ninfer::serve::ParsedToolCallOutput parsed =
        ninfer::serve::parse_qwen_tool_call_output(text, 64);
    int failures = 0;
    failures += check(!parsed.is_tool_call_response, "trailing prose refuses salvage");
    failures += check(parsed.content == text, "trailing prose fallback preserves text");
    return failures;
}

int test_incremental_filter_holds_bare_block() {
    const std::string original =
        "Working.  \n<function=Bash>\n<parameter=cmd>\nls\n</parameter>\n</function>";
    ninfer::serve::ToolCallStreamFilter salvage;
    std::string visible;
    visible += salvage.feed(original.substr(0, 12));
    visible += salvage.feed(original.substr(12, 14));
    visible += salvage.feed(original.substr(26));
    visible += salvage.finish(true);
    int failures = 0;
    failures += check(visible == "Working.", "bare-block filter did not stream held region");
    failures += check(salvage.emitted_bytes() == visible.size(), "bare-block byte count mismatch");

    ninfer::serve::ToolCallStreamFilter restore;
    std::string restored;
    restored += restore.feed(original.substr(0, 7));
    restored += restore.feed(original.substr(7));
    restored += restore.finish(false);
    failures += check(restored == original, "bare-block non-tool finish restores raw bytes");
    return failures;
}

} // namespace

int main() {
    int failures = 0;
    failures += test_single_call();
    failures += test_multiple_calls_and_json_values();
    failures += test_malformed_falls_back_to_text();
    failures += test_suffix_after_tool_falls_back_to_text();
    failures += test_configured_name_limit();
    failures += test_incremental_filter_valid_tool();
    failures += test_incremental_filter_fallback();
    failures += test_bare_block_salvaged();
    failures += test_bare_block_with_lead_in_and_stray_closer();
    failures += test_bare_block_long_lead_in_falls_back();
    failures += test_bare_block_code_fence_lead_falls_back();
    failures += test_bare_block_malformed_params_fall_back();
    failures += test_bare_block_trailing_prose_falls_back();
    failures += test_incremental_filter_holds_bare_block();
    if (failures == 0) { std::cout << "ok\n"; }
    return failures == 0 ? 0 : 1;
}
