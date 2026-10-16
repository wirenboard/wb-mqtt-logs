#include "pattern_matcher.h"
#include <gtest/gtest.h>
#include <stdexcept>

TEST(TPatternMatcherTest, substring)
{
    TPatternMatcher m("Error", false, true);
    ASSERT_TRUE(m.Matches("Some Error happened"));
    ASSERT_FALSE(m.Matches("some error happened"));
    ASSERT_FALSE(m.Matches(""));

    // Regex metacharacters are literal in substring mode
    TPatternMatcher dot("a.b", false, true);
    ASSERT_TRUE(dot.Matches("x a.b y"));
    ASSERT_FALSE(dot.Matches("axb"));
}

TEST(TPatternMatcherTest, substring_case_insensitive)
{
    TPatternMatcher m("error", false, false);
    ASSERT_TRUE(m.Matches("Some ERROR happened"));

    TPatternMatcher cyrillic("ошибка", false, false);
    ASSERT_TRUE(cyrillic.Matches("ОШИБКА чтения"));
}

TEST(TPatternMatcherTest, regex)
{
    TPatternMatcher m("^port \\d+ (opened|closed)$", true, true);
    ASSERT_TRUE(m.Matches("port 502 opened"));
    ASSERT_FALSE(m.Matches("port x opened"));

    // \w is Unicode-aware
    TPatternMatcher word("^\\w+$", true, true);
    ASSERT_TRUE(word.Matches("привет"));
}

TEST(TPatternMatcherTest, substring_case_insensitive_unicode)
{
    // Same results as ICU foldCase()
    TPatternMatcher yo("Ё", false, false);
    ASSERT_TRUE(yo.Matches("ёлка"));

    TPatternMatcher sigma("ΣΟΦΙΑ", false, false);
    ASSERT_TRUE(sigma.Matches("σοφια"));

    TPatternMatcher finalSigma("ς", false, false);
    ASSERT_TRUE(finalSigma.Matches("Σ"));
}

TEST(TPatternMatcherTest, regex_line_terminators)
{
    // Like ICU, '.' doesn't match any Unicode line terminator: LF, CR, FF, NEL, LS
    TPatternMatcher dot("a.b", true, true);
    ASSERT_TRUE(dot.Matches("a b"));
    ASSERT_FALSE(dot.Matches("a\nb"));
    ASSERT_FALSE(dot.Matches("a\rb"));
    ASSERT_FALSE(dot.Matches("a\fb"));
    ASSERT_FALSE(dot.Matches("a\u0085b"));
    ASSERT_FALSE(dot.Matches("a\u2028b"));

    TPatternMatcher dotAll("(?s)a.b", true, true);
    ASSERT_TRUE(dotAll.Matches("a\nb"));
}

TEST(TPatternMatcherTest, regex_anchors)
{
    // Like ICU, '$' matches at the end and before a final line terminator, but not in the middle
    TPatternMatcher end("b$", true, true);
    ASSERT_TRUE(end.Matches("ab"));
    ASSERT_TRUE(end.Matches("ab\n"));
    ASSERT_TRUE(end.Matches("ab\r\n"));
    ASSERT_FALSE(end.Matches("ab\nc"));

    // '^' matches only at the start unless multiline mode is on
    TPatternMatcher start("^b", true, true);
    ASSERT_FALSE(start.Matches("a\nb"));

    TPatternMatcher multiline("(?m)^b", true, true);
    ASSERT_TRUE(multiline.Matches("a\nb"));
}

TEST(TPatternMatcherTest, regex_case_insensitive)
{
    TPatternMatcher m("ОШИБКА", true, false);
    ASSERT_TRUE(m.Matches("ошибка чтения"));

    TPatternMatcher inlineFlag("(?i)ERROR", true, true);
    ASSERT_TRUE(inlineFlag.Matches("error"));
}

TEST(TPatternMatcherTest, regex_unicode_classes)
{
    // Like ICU, \d, \s, \b and POSIX classes are Unicode-aware
    TPatternMatcher digits("^\\d+$", true, true);
    ASSERT_TRUE(digits.Matches("٣")); // ARABIC-INDIC DIGIT THREE

    TPatternMatcher space("a\\sb", true, true);
    ASSERT_TRUE(space.Matches("a b")); // NO-BREAK SPACE

    TPatternMatcher boundary("\\bмир\\b", true, true);
    ASSERT_TRUE(boundary.Matches("привет мир!"));
    ASSERT_FALSE(boundary.Matches("примирение"));

    TPatternMatcher alpha("^[[:alpha:]]+$", true, true);
    ASSERT_TRUE(alpha.Matches("мир"));

    TPatternMatcher script("^\\p{Cyrillic}+$", true, true);
    ASSERT_TRUE(script.Matches("мир"));
    ASSERT_FALSE(script.Matches("mir"));
}

TEST(TPatternMatcherTest, invalid_pattern)
{
    ASSERT_THROW(TPatternMatcher("(unclosed", true, true), std::runtime_error);
}

TEST(TPatternMatcherTest, invalid_utf8_in_message)
{
    // Invalid bytes can't be matched, but the valid parts of the message still can
    TPatternMatcher m("def", false, true);
    ASSERT_TRUE(m.Matches("abc\xff def"));
    ASSERT_FALSE(m.Matches("abc\xff"));
}

TEST(TPatternMatcherTest, only_view_is_matched)
{
    // The message is not NUL-terminated, bytes after its end must be ignored
    std::string buffer = "foo bar";
    TPatternMatcher m("bar", false, true);
    ASSERT_FALSE(m.Matches(std::string_view(buffer).substr(0, 3)));
}

TEST(TPatternMatcherTest, long_message)
{
    // A repeated group over a 48 KiB message needs about 2 MiB of JIT stack, much more than the default 32 KiB
    std::string msg;
    while (msg.size() < 48 * 1024) {
        msg += "ab ";
    }
    msg += "x";
    TPatternMatcher m("(a|b|\\s)*x", true, true);
    ASSERT_TRUE(m.Matches(msg));
}

TEST(TPatternMatcherTest, resource_limit)
{
    // Catastrophic backtracking hits the match limit, the message is treated as not matching
    TPatternMatcher m("(\\w+\\s?)+$", true, true);
    std::string msg(40, 'a');
    msg += "!";
    ASSERT_FALSE(m.Matches(msg));
    ASSERT_TRUE(m.Matches("aaa"));
}
