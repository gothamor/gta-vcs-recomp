#ifndef PS2RECOMP_FORMAT_H
#define PS2RECOMP_FORMAT_H

// [gta-recomp] The recompiler's own string formatting in place of the fmt library (DECISIONS.md 2026-11-27): the
// `{}` replacement fields the code generator uses, with fmt's output for them, so the generated C++ is byte for byte
// what fmt wrote. Supported: `{}`, `{{` / `}}`, and `{:[[fill]align][#][0][width][.precision][type]}` with the types
// d, x, X, b, c, s, g, e, f. A field or argument it doesn't handle throws std::runtime_error naming the format
// string, so the recompile stops instead of writing different code. Floats need a precision or a type: fmt's
// shortest form for a bare `{}` isn't reproduced.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace ps2recomp
{
    namespace format_detail
    {
        struct Arg
        {
            enum class Kind : uint8_t
            {
                Signed,
                Unsigned,
                Bool,
                Char,
                String,
                Float,
                Pointer
            };
            Kind kind;
            union
            {
                int64_t i;
                uint64_t u;
                bool b;
                char c;
                double d;
                const void *p;
                struct
                {
                    const char *data;
                    size_t size;
                } s;
            };
        };

        template <typename T>
        Arg makeArg(const T &value)
        {
            Arg a{};
            using D = std::decay_t<T>;
            if constexpr (std::is_same_v<D, bool>)
            {
                a.kind = Arg::Kind::Bool;
                a.b = value;
            }
            else if constexpr (std::is_same_v<D, char>)
            {
                a.kind = Arg::Kind::Char;
                a.c = value;
            }
            else if constexpr (std::is_enum_v<D>)
            {
                static_assert(!std::is_enum_v<D>, "ps2recomp::format: cast enums to their underlying type");
            }
            else if constexpr (std::is_integral_v<D> && std::is_signed_v<D>)
            {
                a.kind = Arg::Kind::Signed;
                a.i = static_cast<int64_t>(value);
            }
            else if constexpr (std::is_integral_v<D>)
            {
                a.kind = Arg::Kind::Unsigned;
                a.u = static_cast<uint64_t>(value);
            }
            else if constexpr (std::is_floating_point_v<D>)
            {
                a.kind = Arg::Kind::Float;
                a.d = static_cast<double>(value);
            }
            else if constexpr (std::is_same_v<D, const char *> || std::is_same_v<D, char *>)
            {
                a.kind = Arg::Kind::String;
                a.s.data = value;
                a.s.size = std::char_traits<char>::length(value);
            }
            else if constexpr (std::is_convertible_v<const T &, std::string_view>)
            {
                const std::string_view view(value);
                a.kind = Arg::Kind::String;
                a.s.data = view.data();
                a.s.size = view.size();
            }
            else if constexpr (std::is_pointer_v<D> || std::is_null_pointer_v<D>)
            {
                a.kind = Arg::Kind::Pointer;
                a.p = static_cast<const void *>(value);
            }
            else
            {
                static_assert(sizeof(T) == 0, "ps2recomp::format: unsupported argument type");
            }
            return a;
        }

        std::string vformat(std::string_view fmt, const Arg *args, size_t count);
    }

    template <typename... Args>
    std::string format(std::string_view fmt, const Args &...args)
    {
        if constexpr (sizeof...(Args) == 0)
        {
            return format_detail::vformat(fmt, nullptr, 0);
        }
        else
        {
            const format_detail::Arg list[] = {format_detail::makeArg(args)...};
            return format_detail::vformat(fmt, list, sizeof...(Args));
        }
    }
}

#endif // PS2RECOMP_FORMAT_H
