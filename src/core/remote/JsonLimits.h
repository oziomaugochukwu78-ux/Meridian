#pragma once

#include <cstddef>
#include <string_view>

namespace meridian::remote
{

/** Deepest nesting the remote layer accepts. Real tool calls use 3 or 4
    levels; the limit exists because recursive-descent JSON parsers use one
    stack frame per level, so a message of 64 KB of '[' could otherwise
    overflow the stack and crash the application. */
inline constexpr int maxJsonNesting = 64;

/** True if `text` nests arrays/objects deeper than `limit`. Brackets inside
    strings (including escaped quotes) are ignored. Linear time, no allocation,
    stops at the first level past the limit. */
constexpr bool exceedsJsonNesting (std::string_view text, int limit = maxJsonNesting) noexcept
{
    int depth = 0;
    bool inString = false, escaped = false;
    for (const char c : text)
    {
        if (inString)
        {
            if (escaped)        escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"')  inString = false;
            continue;
        }

        if (c == '"')
            inString = true;
        else if (c == '[' || c == '{')
        {
            if (++depth > limit)
                return true;
        }
        else if ((c == ']' || c == '}') && depth > 0)
            --depth;
    }
    return false;
}

/** Strict RFC 8259 syntax check of one complete JSON document (any value,
    surrounded by optional whitespace), nested at most `maxDepth` deep.

    Network input is checked with this before any general-purpose parser sees
    it: lenient parsers accept malformed text in surprising ways (and some
    assert on it), so only well-formed JSON gets through. Strings must be
    valid UTF-8 with no raw control characters; escapes must be one of the
    standard ones, with \u followed by four hex digits. Recursion depth is
    bounded by maxDepth. */
bool isValidJson (std::string_view text, int maxDepth = maxJsonNesting) noexcept;

} // namespace meridian::remote
