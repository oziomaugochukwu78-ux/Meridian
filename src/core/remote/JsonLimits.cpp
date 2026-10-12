#include "JsonLimits.h"

#include "WebSocketProtocol.h"

namespace meridian::remote
{
namespace
{
class Validator
{
public:
    Validator (std::string_view t, int depth) : text (t), maxDepth (depth) {}

    bool document()
    {
        skipWhitespace();
        if (! value (0))
            return false;
        skipWhitespace();
        return pos == text.size();
    }

private:
    bool atEnd() const noexcept { return pos >= text.size(); }
    char peek() const noexcept  { return atEnd() ? '\0' : text[pos]; }

    void skipWhitespace() noexcept
    {
        while (! atEnd() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r'))
            ++pos;
    }

    bool literal (std::string_view word) noexcept
    {
        if (text.substr (pos, word.size()) != word)
            return false;
        pos += word.size();
        return true;
    }

    bool value (int depth)
    {
        switch (peek())
        {
            case '{': return object (depth + 1);
            case '[': return array (depth + 1);
            case '"': return string();
            case 't': return literal ("true");
            case 'f': return literal ("false");
            case 'n': return literal ("null");
            default:  return number();
        }
    }

    bool object (int depth)
    {
        if (depth > maxDepth)
            return false;
        ++pos; // '{'
        skipWhitespace();
        if (peek() == '}')
        {
            ++pos;
            return true;
        }
        for (;;)
        {
            skipWhitespace();
            if (peek() != '"' || ! string())
                return false;
            skipWhitespace();
            if (peek() != ':')
                return false;
            ++pos;
            skipWhitespace();
            if (! value (depth))
                return false;
            skipWhitespace();
            if (peek() == ',') { ++pos; continue; }
            if (peek() == '}') { ++pos; return true; }
            return false;
        }
    }

    bool array (int depth)
    {
        if (depth > maxDepth)
            return false;
        ++pos; // '['
        skipWhitespace();
        if (peek() == ']')
        {
            ++pos;
            return true;
        }
        for (;;)
        {
            skipWhitespace();
            if (! value (depth))
                return false;
            skipWhitespace();
            if (peek() == ',') { ++pos; continue; }
            if (peek() == ']') { ++pos; return true; }
            return false;
        }
    }

    static bool isHex (char c) noexcept
    {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }

    bool string()
    {
        const auto start = ++pos; // after '"'
        while (! atEnd())
        {
            const auto c = static_cast<unsigned char> (text[pos]);
            if (c == '"')
            {
                const bool utf8 = ws::isValidUtf8 (text.substr (start, pos - start));
                ++pos;
                return utf8;
            }
            if (c < 0x20)
                return false; // raw control characters must be escaped
            if (c == '\\')
            {
                ++pos;
                switch (peek())
                {
                    case '"': case '\\': case '/': case 'b': case 'f': case 'n': case 'r': case 't':
                        ++pos;
                        break;
                    case 'u':
                        ++pos;
                        for (int i = 0; i < 4; ++i, ++pos)
                            if (! isHex (peek()))
                                return false;
                        break;
                    default:
                        return false;
                }
                continue;
            }
            ++pos;
        }
        return false; // unterminated
    }

    bool digits() noexcept
    {
        const auto start = pos;
        while (peek() >= '0' && peek() <= '9')
            ++pos;
        return pos > start;
    }

    // number = [ "-" ] ( "0" / 1-9 *DIGIT ) [ "." 1*DIGIT ] [ ( "e" / "E" ) [ "+" / "-" ] 1*DIGIT ]
    bool number() noexcept
    {
        if (peek() == '-')
            ++pos;
        if (peek() == '0')
            ++pos;
        else if (! (peek() >= '1' && peek() <= '9') || ! digits())
            return false;

        if (peek() == '.')
        {
            ++pos;
            if (! digits())
                return false;
        }
        if (peek() == 'e' || peek() == 'E')
        {
            ++pos;
            if (peek() == '+' || peek() == '-')
                ++pos;
            if (! digits())
                return false;
        }
        return true;
    }

    std::string_view text;
    int maxDepth;
    size_t pos = 0;
};
} // namespace

bool isValidJson (std::string_view text, int maxDepth) noexcept
{
    return Validator (text, maxDepth).document();
}

} // namespace meridian::remote
