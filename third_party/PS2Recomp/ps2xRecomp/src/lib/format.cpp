// [gta-recomp] ps2recomp::format (format.h): fmt's output for the replacement fields the recompiler uses.
#include "ps2recomp/format.h"

#include <cstdio>
#include <stdexcept>

namespace ps2recomp::format_detail
{
    namespace
    {
        struct Spec
        {
            char fill = ' ';
            char align = 0; // '<', '>', '^' or 0 (the type's default)
            bool alternate = false;
            bool zero = false;
            int width = 0;
            int precision = -1;
            char type = 0;
        };

        [[noreturn]] void fail(std::string_view fmt, const char *what)
        {
            throw std::runtime_error(std::string("ps2recomp::format: ") + what + " in \"" + std::string(fmt) + "\"");
        }

        bool isAlign(char c)
        {
            return c == '<' || c == '>' || c == '^';
        }

        // the text between ':' and '}'
        Spec parseSpec(std::string_view fmt, std::string_view text)
        {
            Spec spec;
            size_t i = 0;
            if (text.size() >= 2 && isAlign(text[1]))
            {
                spec.fill = text[0];
                spec.align = text[1];
                i = 2;
            }
            else if (!text.empty() && isAlign(text[0]))
            {
                spec.align = text[0];
                i = 1;
            }
            if (i < text.size() && (text[i] == '+' || text[i] == '-' || text[i] == ' '))
                fail(fmt, "sign flags are not supported");
            if (i < text.size() && text[i] == '#')
            {
                spec.alternate = true;
                ++i;
            }
            if (i < text.size() && text[i] == '0')
            {
                spec.zero = true;
                ++i;
            }
            while (i < text.size() && text[i] >= '0' && text[i] <= '9')
                spec.width = spec.width * 10 + (text[i++] - '0');
            if (i < text.size() && text[i] == '.')
            {
                ++i;
                if (i >= text.size() || text[i] < '0' || text[i] > '9')
                    fail(fmt, "a precision without digits");
                spec.precision = 0;
                while (i < text.size() && text[i] >= '0' && text[i] <= '9')
                    spec.precision = spec.precision * 10 + (text[i++] - '0');
            }
            if (i < text.size())
                spec.type = text[i++];
            if (i != text.size())
                fail(fmt, "an unsupported format specification");
            return spec;
        }

        // body: the digits / text; prefix: sign and base prefix (zero padding goes between the two)
        void pad(std::string &out, const Spec &spec, std::string_view prefix, std::string_view body, bool numeric)
        {
            const size_t length = prefix.size() + body.size();
            const size_t width = spec.width > 0 ? static_cast<size_t>(spec.width) : 0;
            if (length >= width)
            {
                out += prefix;
                out += body;
                return;
            }
            const size_t fillCount = width - length;
            if (numeric && spec.zero && !spec.align)
            {
                out += prefix;
                out.append(fillCount, '0');
                out += body;
                return;
            }
            const char align = spec.align ? spec.align : (numeric ? '>' : '<');
            const size_t before = align == '>' ? fillCount : align == '^' ? fillCount / 2 : 0;
            out.append(before, spec.fill);
            out += prefix;
            out += body;
            out.append(fillCount - before, spec.fill);
        }

        void formatInteger(std::string &out, std::string_view fmt, const Spec &spec, bool negative, uint64_t magnitude)
        {
            char digits[72];
            size_t n = 0;
            const char type = spec.type ? spec.type : 'd';
            unsigned base = 10;
            const char *alphabet = "0123456789abcdef";
            std::string_view basePrefix;
            switch (type)
            {
            case 'd':
                break;
            case 'x':
                base = 16;
                basePrefix = "0x";
                break;
            case 'X':
                base = 16;
                alphabet = "0123456789ABCDEF";
                basePrefix = "0X";
                break;
            case 'b':
                base = 2;
                basePrefix = "0b";
                break;
            default:
                fail(fmt, "an unsupported integer presentation type");
            }
            if (spec.precision >= 0)
                fail(fmt, "a precision on an integer");
            do
            {
                digits[n++] = alphabet[magnitude % base];
                magnitude /= base;
            } while (magnitude);
            std::string body(digits, n);
            for (size_t a = 0, b = n - 1; a < b; ++a, --b)
                std::swap(body[a], body[b]);
            std::string prefix = negative ? "-" : "";
            if (spec.alternate)
                prefix += basePrefix;
            pad(out, spec, prefix, body, true);
        }

        void formatFloat(std::string &out, std::string_view fmt, const Spec &spec, double value)
        {
            char type = spec.type;
            if (!type)
            {
                if (spec.precision < 0)
                    fail(fmt, "a float in a bare {} (fmt's shortest form is not reproduced)");
                type = 'g';
            }
            if (type != 'g' && type != 'e' && type != 'f')
                fail(fmt, "an unsupported float presentation type");
            if (spec.alternate)
                fail(fmt, "'#' on a float");
            const int precision = spec.precision >= 0 ? spec.precision : 6;
            char pattern[8] = {'%', '.', '*', type, 0};
            char buffer[512];
            const int n = std::snprintf(buffer, sizeof(buffer), pattern, precision, value);
            if (n < 0 || n >= static_cast<int>(sizeof(buffer)))
                fail(fmt, "a float too long to format");
            std::string_view text(buffer, static_cast<size_t>(n));
            std::string_view sign;
            if (!text.empty() && text[0] == '-')
            {
                sign = text.substr(0, 1);
                text.remove_prefix(1);
            }
            pad(out, spec, sign, text, true);
        }

        void formatArg(std::string &out, std::string_view fmt, const Spec &spec, const Arg &arg)
        {
            switch (arg.kind)
            {
            case Arg::Kind::Signed:
                if (spec.type == 'c')
                    fail(fmt, "'c' on an integer");
                formatInteger(out, fmt, spec, arg.i < 0, arg.i < 0 ? 0u - static_cast<uint64_t>(arg.i) : static_cast<uint64_t>(arg.i));
                return;
            case Arg::Kind::Unsigned:
                if (spec.type == 'c')
                    fail(fmt, "'c' on an integer");
                formatInteger(out, fmt, spec, false, arg.u);
                return;
            case Arg::Kind::Bool:
                if (spec.type && spec.type != 's')
                {
                    formatInteger(out, fmt, spec, false, arg.b ? 1u : 0u);
                    return;
                }
                pad(out, spec, {}, arg.b ? "true" : "false", false);
                return;
            case Arg::Kind::Char:
                if (spec.type && spec.type != 'c')
                {
                    formatInteger(out, fmt, spec, arg.c < 0, arg.c < 0 ? 0u - static_cast<uint64_t>(arg.c) : static_cast<uint64_t>(arg.c));
                    return;
                }
                pad(out, spec, {}, std::string_view(&arg.c, 1), false);
                return;
            case Arg::Kind::String:
            {
                if (spec.type && spec.type != 's')
                    fail(fmt, "an unsupported string presentation type");
                std::string_view text(arg.s.data, arg.s.size);
                if (spec.precision >= 0 && static_cast<size_t>(spec.precision) < text.size())
                    text = text.substr(0, static_cast<size_t>(spec.precision));
                pad(out, spec, {}, text, false);
                return;
            }
            case Arg::Kind::Float:
                formatFloat(out, fmt, spec, arg.d);
                return;
            case Arg::Kind::Pointer:
            {
                if (spec.type && spec.type != 'p')
                    fail(fmt, "an unsupported pointer presentation type");
                Spec hex = spec;
                hex.type = 'x';
                hex.alternate = true;
                formatInteger(out, fmt, hex, false, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(arg.p)));
                return;
            }
            }
        }
    }

    std::string vformat(std::string_view fmt, const Arg *args, size_t count)
    {
        std::string out;
        out.reserve(fmt.size() + count * 8);
        size_t next = 0;
        for (size_t i = 0; i < fmt.size(); ++i)
        {
            const char c = fmt[i];
            if (c == '}')
            {
                if (i + 1 < fmt.size() && fmt[i + 1] == '}')
                {
                    out += '}';
                    ++i;
                    continue;
                }
                fail(fmt, "an unmatched '}'");
            }
            if (c != '{')
            {
                out += c;
                continue;
            }
            if (i + 1 < fmt.size() && fmt[i + 1] == '{')
            {
                out += '{';
                ++i;
                continue;
            }
            const size_t close = fmt.find('}', i + 1);
            if (close == std::string_view::npos)
                fail(fmt, "an unclosed '{'");
            std::string_view field = fmt.substr(i + 1, close - i - 1);
            if (!field.empty() && field[0] != ':')
                fail(fmt, "argument indices or names are not supported");
            if (field.find('{') != std::string_view::npos)
                fail(fmt, "nested replacement fields are not supported");
            if (next >= count)
                fail(fmt, "more replacement fields than arguments");
            const Spec spec = field.empty() ? Spec{} : parseSpec(fmt, field.substr(1));
            formatArg(out, fmt, spec, args[next++]);
            i = close;
        }
        if (next != count)
            fail(fmt, "more arguments than replacement fields");
        return out;
    }
}
