#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

// Matches UTF-8 log messages against a substring or a PCRE2 regular expression.
// A single message must not fail the whole request: invalid UTF-8 sequences in a message can't be matched,
// and a message that exceeds PCRE2 resource limits is treated as not matching.
class TPatternMatcher
{
public:
    // Throws std::runtime_error if the pattern is invalid
    TPatternMatcher(const std::string& pattern, bool regex, bool caseSensitive);

    bool Matches(std::string_view msg);

private:
    static constexpr size_t JIT_STACK_START_SIZE = 32 * 1024;
    // Enough for a repeated capturing group over a message of journald's default LineMax (48 KiB)
    static constexpr size_t JIT_STACK_MAX_SIZE = 4 * 1024 * 1024;

    std::string Pattern;
    bool ResourceLimitReported = false;
    std::unique_ptr<pcre2_code, decltype(&pcre2_code_free)> Code{nullptr, &pcre2_code_free};
    std::unique_ptr<pcre2_jit_stack, decltype(&pcre2_jit_stack_free)> JitStack{nullptr, &pcre2_jit_stack_free};
    std::unique_ptr<pcre2_match_context, decltype(&pcre2_match_context_free)> MatchContext{nullptr,
                                                                                           &pcre2_match_context_free};
    std::unique_ptr<pcre2_match_data, decltype(&pcre2_match_data_free)> MatchData{nullptr, &pcre2_match_data_free};
};
