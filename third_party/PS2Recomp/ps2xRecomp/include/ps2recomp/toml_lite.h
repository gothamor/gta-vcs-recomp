#ifndef PS2RECOMP_TOML_LITE_H
#define PS2RECOMP_TOML_LITE_H

// [gta-recomp] A TOML 1.0 reader for the recompiler's config, in place of the toml11 library (DECISIONS.md
// 2026-11-27). It reads what a config holds: comments, [tables], [[arrays of tables]], bare / quoted / dotted keys,
// basic and literal strings, integers (decimal, 0x, 0o, 0b, with _), floats, booleans, arrays (over several lines,
// trailing comma) and inline tables. Not read: multi-line strings and dates - an error naming the file and line, as
// is anything malformed or defined twice. The API is the part of toml11's the config manager used.

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace ps2recomp::toml
{
    class Value
    {
    public:
        enum class Type : uint8_t
        {
            Boolean,
            Integer,
            Float,
            String,
            Array,
            Table
        };
        using Array = std::vector<Value>;
        using Table = std::map<std::string, Value>;

        Value() : m_type(Type::Table) {}
        explicit Value(Type type) : m_type(type) {}

        Type type() const { return m_type; }
        bool is_boolean() const { return m_type == Type::Boolean; }
        bool is_integer() const { return m_type == Type::Integer; }
        bool is_floating() const { return m_type == Type::Float; }
        bool is_string() const { return m_type == Type::String; }
        bool is_array() const { return m_type == Type::Array; }
        bool is_table() const { return m_type == Type::Table; }

        bool as_boolean() const;
        int64_t as_integer() const;
        double as_floating() const;
        const std::string &as_string() const;
        const Array &as_array() const;
        const Table &as_table() const;

        bool contains(const std::string &key) const { return is_table() && m_table.count(key) != 0; }
        const Value &at(const std::string &key) const; // throws when not a table or no such key

        // for the parser
        bool m_boolean = false;
        int64_t m_integer = 0;
        double m_float = 0.0;
        std::string m_string;
        Array m_array;
        Table m_table;
        bool m_defined = false;      // a table given by a [header] or a key path (redefining it is an error)
        bool m_tableArray = false;   // an array made by [[headers]]
        bool m_inline = false;       // an inline table or a value array: closed to later keys

    private:
        Type m_type;
    };

    using value = Value; // toml11's name
    using table = Value::Table;
    using array = Value::Array;

    Value parse(const std::string &path);
    Value parseText(const std::string &text, const std::string &name = "<text>");

    const Value &find(const Value &table, const std::string &key);

    template <typename T>
    T get(const Value &value);

    template <>
    inline bool get<bool>(const Value &value) { return value.as_boolean(); }
    template <>
    inline int64_t get<int64_t>(const Value &value) { return value.as_integer(); }
    template <>
    inline double get<double>(const Value &value) { return value.as_floating(); }
    template <>
    inline std::string get<std::string>(const Value &value) { return value.as_string(); }
    template <>
    inline std::vector<std::string> get<std::vector<std::string>>(const Value &value)
    {
        std::vector<std::string> out;
        for (const Value &item : value.as_array())
            out.push_back(item.as_string());
        return out;
    }

    template <typename T>
    T find(const Value &table, const std::string &key)
    {
        return get<T>(find(table, key));
    }

    // the value, or fallback when the key is missing or holds another type (as toml11's find_or)
    template <typename T>
    T find_or(const Value &table, const std::string &key, const T &fallback)
    {
        if (!table.contains(key))
            return fallback;
        try
        {
            return get<T>(table.at(key));
        }
        catch (const std::runtime_error &)
        {
            return fallback;
        }
    }

    inline std::string find_or(const Value &table, const std::string &key, const char *fallback)
    {
        return find_or<std::string>(table, key, std::string(fallback));
    }
}

#endif // PS2RECOMP_TOML_LITE_H
