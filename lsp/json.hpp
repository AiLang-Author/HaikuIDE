/* Minimal JSON for LSP messages.
 *
 * Copyright (c) 2026 Sean Collins, 2 Paws Machine and Engineering. MIT.
 */
#ifndef HAIKUIDE_JSON_HPP
#define HAIKUIDE_JSON_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace haikuide {

struct Value {
    enum class Kind { Null, Bool, Number, String, Array, Object };

    Kind kind = Kind::Null;
    bool b = false;
    bool is_int = false;
    int64_t i = 0;
    double d = 0;
    std::string s;
    std::vector<Value> a;
    std::vector<std::pair<std::string, Value>> o;

    const Value* find(std::string_view key) const;
};

Value JsonNull();
Value JsonBool(bool v);
Value JsonInt(int64_t v);
Value JsonString(std::string v);
Value JsonArray();
Value JsonObject();
void Push(Value& array, Value v);
void Put(Value& object, std::string key, Value v);

bool Parse(std::string_view text, Value* out, std::string* err);
std::string Stringify(const Value& value);

}  // namespace haikuide

#endif
