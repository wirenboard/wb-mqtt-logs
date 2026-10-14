#include "pattern_matcher.h"

#include "log.h"

#include <array>
#include <cstdint>
#include <stdexcept>

#define LOG(logger) ::logger.Log() << "[logs] "

namespace
{
    bool IsResourceLimitError(int errorCode)
    {
        return errorCode == PCRE2_ERROR_MATCHLIMIT || errorCode == PCRE2_ERROR_DEPTHLIMIT ||
               errorCode == PCRE2_ERROR_HEAPLIMIT || errorCode == PCRE2_ERROR_JIT_STACKLIMIT;
    }

    std::string GetErrorMessage(int errorCode)
    {
        std::array<PCRE2_UCHAR, 256> buffer;
        if (pcre2_get_error_message(errorCode, buffer.data(), buffer.size()) < 0) {
            return "error " + std::to_string(errorCode);
        }
        return reinterpret_cast<const char*>(buffer.data());
    }
}

TPatternMatcher::TPatternMatcher(const std::string& pattern, bool regex, bool caseSensitive): Pattern(pattern)
{
    // PCRE2_UCP makes \w, \d, \b, \s and POSIX classes Unicode-aware, but is not allowed with
    // PCRE2_LITERAL. Caseless matching uses Unicode case folding in UTF mode anyway.
    uint32_t options = PCRE2_UTF | PCRE2_MATCH_INVALID_UTF | (regex ? PCRE2_UCP : PCRE2_LITERAL);
    if (!caseSensitive) {
        options |= PCRE2_CASELESS;
    }
    std::unique_ptr<pcre2_compile_context, decltype(&pcre2_compile_context_free)> context(
        pcre2_compile_context_create(nullptr),
        &pcre2_compile_context_free);
    if (!context) {
        throw std::runtime_error("Failed to allocate compile context for pattern '" + pattern + "'");
    }
    // Any Unicode line terminator (CR, LF, CRLF, VT, FF, NEL, LS, PS) ends a line for '.', '^' and '$'
    pcre2_set_newline(context.get(), PCRE2_NEWLINE_ANY);
    int errorCode;
    PCRE2_SIZE errorOffset;
    Code.reset(pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.data()),
                             pattern.size(),
                             options,
                             &errorCode,
                             &errorOffset,
                             context.get()));
    if (!Code) {
        throw std::runtime_error("Invalid pattern '" + pattern + "': " + GetErrorMessage(errorCode) + " at offset " +
                                 std::to_string(errorOffset));
    }
    // If JIT is unavailable, pcre2_match() falls back to the interpreter
    if (pcre2_jit_compile(Code.get(), PCRE2_JIT_COMPLETE) == 0) {
        // The default 32 KiB JIT stack is too small for patterns with repeated groups on long messages
        JitStack.reset(pcre2_jit_stack_create(JIT_STACK_START_SIZE, JIT_STACK_MAX_SIZE, nullptr));
    }
    MatchContext.reset(pcre2_match_context_create(nullptr));
    if (!MatchContext) {
        throw std::runtime_error("Failed to allocate match context for pattern '" + pattern + "'");
    }
    if (JitStack) {
        pcre2_jit_stack_assign(MatchContext.get(), nullptr, JitStack.get());
    }
    // Only the fact of a match is needed, not the captured groups
    MatchData.reset(pcre2_match_data_create(1, nullptr));
    if (!MatchData) {
        throw std::runtime_error("Failed to allocate match data for pattern '" + pattern + "'");
    }
}

bool TPatternMatcher::Matches(std::string_view msg)
{
    int rc = pcre2_match(Code.get(),
                         reinterpret_cast<PCRE2_SPTR>(msg.data()),
                         msg.size(),
                         0,
                         0,
                         MatchData.get(),
                         MatchContext.get());
    if (rc == PCRE2_ERROR_NOMATCH) {
        return false;
    }
    if (IsResourceLimitError(rc)) {
        if (!ResourceLimitReported) {
            LOG(Warn) << "Pattern '" << Pattern << "' exceeded a resource limit (" << GetErrorMessage(rc)
                      << "), such messages are treated as not matching";
            ResourceLimitReported = true;
        }
        return false;
    }
    if (rc < 0) {
        throw std::runtime_error("Error searching for pattern '" + Pattern + "': " + GetErrorMessage(rc));
    }
    return true;
}
