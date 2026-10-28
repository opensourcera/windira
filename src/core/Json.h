// SPDX-License-Identifier: MIT
// Minimal JSON value, parser and writer - just enough for Windira's state file.
#pragma once

#include <string>
#include <vector>

namespace windira::json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;
    Value(bool b) : type_(Type::Bool), bool_(b) {}
    Value(double d) : type_(Type::Number), number_(d) {}
    Value(int i) : type_(Type::Number), number_(i) {}
    Value(std::string s) : type_(Type::String), string_(std::move(s)) {}
    Value(const char* s) : type_(Type::String), string_(s) {}

    static Value MakeArray() { Value v; v.type_ = Type::Array; return v; }
    static Value MakeObject() { Value v; v.type_ = Type::Object; return v; }

    Type type() const { return type_; }
    bool IsNull() const { return type_ == Type::Null; }
    bool IsArray() const { return type_ == Type::Array; }
    bool IsObject() const { return type_ == Type::Object; }

    bool AsBool(bool def = false) const { return type_ == Type::Bool ? bool_ : def; }
    double AsNumber(double def = 0) const { return type_ == Type::Number ? number_ : def; }
    const std::string& AsString() const { static const std::string empty; return type_ == Type::String ? string_ : empty; }

    // Array access
    std::vector<Value>& Items() { return array_; }
    const std::vector<Value>& Items() const { return array_; }
    void Push(Value v) { type_ = Type::Array; array_.push_back(std::move(v)); }

    // Object access (keeps insertion order for stable, diff-friendly output)
    const Value* Find(const std::string& key) const;
    void Set(const std::string& key, Value v);
    const std::vector<std::pair<std::string, Value>>& Members() const { return object_; }

    std::string Serialize(int indent = 2) const;
    static bool Parse(const std::string& text, Value& out, std::string* error = nullptr);

private:
    void SerializeTo(std::string& out, int indent, int depth) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0;
    std::string string_;
    std::vector<Value> array_;
    std::vector<std::pair<std::string, Value>> object_;
};

}  // namespace windira::json
