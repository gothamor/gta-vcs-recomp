// [gta-recomp] ps2recomp::toml (toml_lite.h), written from the TOML 1.0 specification.
#include "ps2recomp/toml_lite.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>

namespace ps2recomp::toml
{
    namespace
    {
        [[noreturn]] void typeError(const char *wanted)
        {
            throw std::runtime_error(std::string("toml: the value is not ") + wanted);
        }
    }

    bool Value::as_boolean() const
    {
        if (!is_boolean())
            typeError("a boolean");
        return m_boolean;
    }

    int64_t Value::as_integer() const
    {
        if (!is_integer())
            typeError("an integer");
        return m_integer;
    }

    double Value::as_floating() const
    {
        if (!is_floating())
            typeError("a float");
        return m_float;
    }

    const std::string &Value::as_string() const
    {
        if (!is_string())
            typeError("a string");
        return m_string;
    }

    const Value::Array &Value::as_array() const
    {
        if (!is_array())
            typeError("an array");
        return m_array;
    }

    const Value::Table &Value::as_table() const
    {
        if (!is_table())
            typeError("a table");
        return m_table;
    }

    const Value &Value::at(const std::string &key) const
    {
        if (!is_table())
            typeError("a table");
        const auto it = m_table.find(key);
        if (it == m_table.end())
            throw std::runtime_error("toml: no key \"" + key + "\"");
        return it->second;
    }

    const Value &find(const Value &table, const std::string &key)
    {
        return table.at(key);
    }

    namespace
    {
        class Parser
        {
        public:
            Parser(const std::string &text, const std::string &name) : m_text(text), m_name(name) {}

            Value document()
            {
                Value root(Value::Type::Table);
                root.m_defined = true;
                Value *current = &root;
                if (m_text.compare(0, 3, "\xEF\xBB\xBF") == 0)
                    m_pos = 3;
                for (;;)
                {
                    skipBlank(true);
                    if (atEnd())
                        break;
                    if (peek() == '[')
                    {
                        const bool arrayOfTables = peek(1) == '[';
                        m_pos += arrayOfTables ? 2 : 1;
                        skipBlank(false);
                        const std::vector<std::string> path = keyPath();
                        skipBlank(false);
                        expect(']');
                        if (arrayOfTables)
                            expect(']');
                        endOfLine();
                        current = arrayOfTables ? &appendTable(root, path) : &defineTable(root, path);
                        continue;
                    }
                    keyValue(*current);
                    endOfLine();
                }
                return root;
            }

        private:
            const std::string &m_text;
            const std::string &m_name;
            size_t m_pos = 0;

            bool atEnd() const { return m_pos >= m_text.size(); }
            char peek(size_t ahead = 0) const { return m_pos + ahead < m_text.size() ? m_text[m_pos + ahead] : '\0'; }

            [[noreturn]] void fail(const std::string &what) const
            {
                size_t line = 1;
                for (size_t i = 0; i < m_pos && i < m_text.size(); ++i)
                    line += m_text[i] == '\n';
                throw std::runtime_error(m_name + ":" + std::to_string(line) + ": " + what);
            }

            void expect(char c)
            {
                if (peek() != c)
                    fail(std::string("expected '") + c + "'");
                ++m_pos;
            }

            // spaces and tabs; with newlines also line ends and comments
            void skipBlank(bool newlines)
            {
                while (!atEnd())
                {
                    const char c = peek();
                    if (c == ' ' || c == '\t')
                        ++m_pos;
                    else if (newlines && (c == '\n' || (c == '\r' && peek(1) == '\n')))
                        m_pos += c == '\r' ? 2 : 1;
                    else if (newlines && c == '#')
                        skipComment();
                    else
                        break;
                }
            }

            void skipComment()
            {
                while (!atEnd() && peek() != '\n')
                {
                    const unsigned char c = static_cast<unsigned char>(peek());
                    if ((c < 0x20 && c != '\t' && c != '\r') || c == 0x7F)
                        fail("a control character in a comment");
                    ++m_pos;
                }
            }

            void endOfLine()
            {
                skipBlank(false);
                if (peek() == '#')
                    skipComment();
                if (atEnd())
                    return;
                if (peek() == '\r' && peek(1) == '\n')
                    m_pos += 2;
                else if (peek() == '\n')
                    ++m_pos;
                else
                    fail("expected the end of the line");
            }

            static bool isBare(char c)
            {
                return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
            }

            std::string simpleKey()
            {
                if (peek() == '"')
                    return basicString();
                if (peek() == '\'')
                    return literalString();
                const size_t start = m_pos;
                while (isBare(peek()))
                    ++m_pos;
                if (m_pos == start)
                    fail("expected a key");
                return m_text.substr(start, m_pos - start);
            }

            std::vector<std::string> keyPath()
            {
                std::vector<std::string> path{simpleKey()};
                for (;;)
                {
                    skipBlank(false);
                    if (peek() != '.')
                        return path;
                    ++m_pos;
                    skipBlank(false);
                    path.push_back(simpleKey());
                }
            }

            // a [header] or key path step: descend into (or create) a table; an array of tables gives its last
            Value &descend(Value &table, const std::string &key, bool fromHeader)
            {
                auto it = table.m_table.find(key);
                if (it == table.m_table.end())
                {
                    Value child(Value::Type::Table);
                    it = table.m_table.emplace(key, std::move(child)).first;
                }
                Value &child = it->second;
                if (child.is_array() && child.m_tableArray && fromHeader)
                    return child.m_array.back();
                if (!child.is_table() || child.m_inline)
                    fail("\"" + key + "\" is already a value");
                return child;
            }

            Value &defineTable(Value &root, const std::vector<std::string> &path)
            {
                Value *table = &root;
                for (size_t i = 0; i + 1 < path.size(); ++i)
                    table = &descend(*table, path[i], true);
                const bool existed = table->m_table.count(path.back()) != 0;
                Value &target = descend(*table, path.back(), false);
                if (existed && target.m_defined)
                    fail("table [" + path.back() + "] defined twice");
                target.m_defined = true;
                return target;
            }

            Value &appendTable(Value &root, const std::vector<std::string> &path)
            {
                Value *table = &root;
                for (size_t i = 0; i + 1 < path.size(); ++i)
                    table = &descend(*table, path[i], true);
                auto it = table->m_table.find(path.back());
                if (it == table->m_table.end())
                {
                    Value array(Value::Type::Array);
                    array.m_tableArray = true;
                    it = table->m_table.emplace(path.back(), std::move(array)).first;
                }
                Value &array = it->second;
                if (!array.is_array() || !array.m_tableArray)
                    fail("\"" + path.back() + "\" is not an array of tables");
                Value element(Value::Type::Table);
                element.m_defined = true;
                array.m_array.push_back(std::move(element));
                return array.m_array.back();
            }

            void keyValue(Value &table)
            {
                const std::vector<std::string> path = keyPath();
                skipBlank(false);
                expect('=');
                skipBlank(false);
                Value *target = &table;
                for (size_t i = 0; i + 1 < path.size(); ++i)
                    target = &descend(*target, path[i], false);
                if (target->m_table.count(path.back()))
                    fail("key \"" + path.back() + "\" defined twice");
                target->m_table.emplace(path.back(), value());
            }

            Value value()
            {
                const char c = peek();
                if (c == '"')
                {
                    if (peek(1) == '"' && peek(2) == '"')
                        fail("multi-line strings are not supported");
                    Value v(Value::Type::String);
                    v.m_string = basicString();
                    return v;
                }
                if (c == '\'')
                {
                    if (peek(1) == '\'' && peek(2) == '\'')
                        fail("multi-line strings are not supported");
                    Value v(Value::Type::String);
                    v.m_string = literalString();
                    return v;
                }
                if (c == '[')
                    return arrayValue();
                if (c == '{')
                    return inlineTable();
                return scalar();
            }

            Value arrayValue()
            {
                expect('[');
                Value v(Value::Type::Array);
                v.m_inline = true;
                for (;;)
                {
                    skipBlank(true);
                    if (peek() == ']')
                    {
                        ++m_pos;
                        return v;
                    }
                    v.m_array.push_back(value());
                    skipBlank(true);
                    if (peek() == ',')
                    {
                        ++m_pos;
                        continue;
                    }
                    expect(']');
                    return v;
                }
            }

            Value inlineTable()
            {
                expect('{');
                Value v(Value::Type::Table);
                v.m_defined = true;
                skipBlank(false);
                if (peek() == '}')
                {
                    ++m_pos;
                    v.m_inline = true;
                    return v;
                }
                for (;;)
                {
                    skipBlank(false);
                    keyValue(v);
                    skipBlank(false);
                    if (peek() == ',')
                    {
                        ++m_pos;
                        continue;
                    }
                    expect('}');
                    v.m_inline = true;
                    return v;
                }
            }

            static void appendUtf8(std::string &out, uint32_t cp)
            {
                if (cp < 0x80)
                    out += static_cast<char>(cp);
                else if (cp < 0x800)
                {
                    out += static_cast<char>(0xC0 | (cp >> 6));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
                else if (cp < 0x10000)
                {
                    out += static_cast<char>(0xE0 | (cp >> 12));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
                else
                {
                    out += static_cast<char>(0xF0 | (cp >> 18));
                    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
            }

            std::string basicString()
            {
                expect('"');
                std::string out;
                for (;;)
                {
                    if (atEnd() || peek() == '\n')
                        fail("an unterminated string");
                    const char c = m_text[m_pos++];
                    if (c == '"')
                        return out;
                    if (c != '\\')
                    {
                        const unsigned char u = static_cast<unsigned char>(c);
                        if ((u < 0x20 && c != '\t') || u == 0x7F)
                            fail("a control character in a string");
                        out += c;
                        continue;
                    }
                    const char e = m_text[m_pos++];
                    switch (e)
                    {
                    case 'b': out += '\b'; break;
                    case 't': out += '\t'; break;
                    case 'n': out += '\n'; break;
                    case 'f': out += '\f'; break;
                    case 'r': out += '\r'; break;
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case 'u':
                    case 'U':
                    {
                        const size_t digits = e == 'u' ? 4 : 8;
                        uint32_t cp = 0;
                        for (size_t i = 0; i < digits; ++i)
                        {
                            const char h = peek();
                            int d = (h >= '0' && h <= '9') ? h - '0' : (h >= 'a' && h <= 'f') ? h - 'a' + 10 : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
                            if (d < 0)
                                fail("a bad \\u escape");
                            cp = cp * 16 + static_cast<uint32_t>(d);
                            ++m_pos;
                        }
                        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
                            fail("a \\u escape that is no Unicode scalar value");
                        appendUtf8(out, cp);
                        break;
                    }
                    default:
                        fail(std::string("an unknown escape \\") + e);
                    }
                }
            }

            std::string literalString()
            {
                expect('\'');
                const size_t start = m_pos;
                while (!atEnd() && peek() != '\'' && peek() != '\n')
                    ++m_pos;
                if (peek() != '\'')
                    fail("an unterminated literal string");
                std::string out = m_text.substr(start, m_pos - start);
                ++m_pos;
                return out;
            }

            Value scalar()
            {
                const size_t start = m_pos;
                while (!atEnd())
                {
                    const char c = peek();
                    if (c == ',' || c == ']' || c == '}' || c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '#')
                        break;
                    ++m_pos;
                }
                const std::string token = m_text.substr(start, m_pos - start);
                if (token.empty())
                    fail("expected a value");
                if (token == "true" || token == "false")
                {
                    Value v(Value::Type::Boolean);
                    v.m_boolean = token == "true";
                    return v;
                }
                if (token.find_first_of(":") != std::string::npos ||
                    (token.size() >= 5 && token[4] == '-' && token[0] >= '0' && token[0] <= '9'))
                    fail("dates and times are not supported");

                std::string body = token;
                bool negative = false;
                if (body[0] == '+' || body[0] == '-')
                {
                    negative = body[0] == '-';
                    body.erase(0, 1);
                }
                if (body == "inf" || body == "nan")
                {
                    Value v(Value::Type::Float);
                    v.m_float = body == "inf" ? std::numeric_limits<double>::infinity() : std::numeric_limits<double>::quiet_NaN();
                    if (negative)
                        v.m_float = -v.m_float;
                    return v;
                }
                // underscores only between digits
                std::string digits;
                for (size_t i = 0; i < body.size(); ++i)
                {
                    if (body[i] == '_')
                    {
                        auto digit = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };
                        if (i == 0 || i + 1 >= body.size() || !digit(body[i - 1]) || !digit(body[i + 1]))
                            fail("a misplaced '_' in a number");
                        continue;
                    }
                    digits += body[i];
                }
                if (digits.empty())
                    fail("expected a value");

                int base = 10;
                if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'o' || digits[1] == 'b'))
                {
                    if (token[0] == '+' || token[0] == '-')
                        fail("a sign on a hexadecimal, octal or binary integer");
                    base = digits[1] == 'x' ? 16 : digits[1] == 'o' ? 8 : 2;
                    digits.erase(0, 2);
                }
                const bool isFloat = base == 10 && digits.find_first_of(".eE") != std::string::npos;
                if (isFloat)
                {
                    char *end = nullptr;
                    const double d = std::strtod(digits.c_str(), &end);
                    if (end != digits.c_str() + digits.size() || digits[0] == '.' || digits.back() == '.')
                        fail("a malformed float \"" + token + "\"");
                    Value v(Value::Type::Float);
                    v.m_float = negative ? -d : d;
                    return v;
                }
                if (base == 10 && digits.size() > 1 && digits[0] == '0')
                    fail("a leading zero in \"" + token + "\"");
                uint64_t magnitude = 0;
                for (const char c : digits)
                {
                    int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 99;
                    if (d >= base)
                        fail("a malformed value \"" + token + "\"");
                    if (magnitude > (std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(d)) / static_cast<uint64_t>(base))
                        fail("an integer out of range \"" + token + "\"");
                    magnitude = magnitude * static_cast<uint64_t>(base) + static_cast<uint64_t>(d);
                }
                const uint64_t limit = negative ? uint64_t(1) << 63 : (uint64_t(1) << 63) - 1;
                if (magnitude > limit)
                    fail("an integer out of range \"" + token + "\"");
                Value v(Value::Type::Integer);
                v.m_integer = negative ? static_cast<int64_t>(0u - magnitude) : static_cast<int64_t>(magnitude);
                return v;
            }
        };
    }

    Value parseText(const std::string &text, const std::string &name)
    {
        return Parser(text, name).document();
    }

    Value parse(const std::string &path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            throw std::runtime_error("toml: cannot open " + path);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return parseText(text, path);
    }
}
