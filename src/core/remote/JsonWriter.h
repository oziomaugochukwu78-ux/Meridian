#pragma once

#include <charconv>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace meridian::remote
{

/** Appends `text` to `out` as a quoted, escaped JSON string. */
inline void appendJsonString (std::string& out, std::string_view text)
{
    static constexpr char hex[] = "0123456789abcdef";
    out += '"';
    for (const char ch : text)
    {
        const auto c = static_cast<unsigned char> (ch);
        switch (c)
        {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20)
                {
                    out += "\\u00";
                    out += hex[c >> 4];
                    out += hex[c & 15];
                }
                else
                {
                    out += ch;
                }
        }
    }
    out += '"';
}

/** Minimal builder for flat JSON objects. The method names are distinct on
    purpose: an overloaded add() would silently pick the bool overload for
    string literals. */
class JsonObject
{
public:
    JsonObject& addString (std::string_view key, std::string_view value)
    {
        appendKey (key);
        appendJsonString (body, value);
        return *this;
    }

    JsonObject& addBool (std::string_view key, bool value)
    {
        appendKey (key);
        body += value ? "true" : "false";
        return *this;
    }

    JsonObject& addInt (std::string_view key, int64_t value)
    {
        appendKey (key);
        body += std::to_string (value);
        return *this;
    }

    /** Non-finite numbers are not valid JSON; they are written as null. */
    JsonObject& addNumber (std::string_view key, double value, int decimals = 2)
    {
        appendKey (key);
        if (! std::isfinite (value))
        {
            body += "null";
            return *this;
        }
        char buffer[64];
        const auto result = std::to_chars (buffer, buffer + sizeof (buffer), value, std::chars_format::fixed, decimals);
        body.append (buffer, result.ptr);
        return *this;
    }

    /** Inserts pre-serialised JSON (an array or nested object) verbatim. */
    JsonObject& addRaw (std::string_view key, std::string_view json)
    {
        appendKey (key);
        body += json;
        return *this;
    }

    std::string toString() const { return "{" + body + "}"; }

private:
    void appendKey (std::string_view key)
    {
        if (! body.empty())
            body += ',';
        appendJsonString (body, key);
        body += ':';
    }

    std::string body;
};

} // namespace meridian::remote
