//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: минимальный разбор и запись JSON (DOM) без внешних зависимостей.
//
#pragma once

#include "crossrender/core/Base.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace crossrender {

class JsonValue;
using JsonArray = std::vector<JsonValue>;
using JsonObject = std::map<std::string, JsonValue>;

class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    JsonValue() = default;
    JsonValue(std::nullptr_t) {}
    JsonValue(bool b) : type_(Type::Bool), bool_(b) {}
    JsonValue(double n) : type_(Type::Number), num_(n) {}
    JsonValue(int n) : type_(Type::Number), num_(static_cast<double>(n)) {}
    JsonValue(i64 n) : type_(Type::Number), num_(static_cast<double>(n)) {}
    JsonValue(u32 n) : type_(Type::Number), num_(static_cast<double>(n)) {}
    JsonValue(const char* s) : type_(Type::String), str_(s ? s : "") {}
    JsonValue(std::string s) : type_(Type::String), str_(std::move(s)) {}
    JsonValue(JsonArray a) : type_(Type::Array), arr_(std::make_shared<JsonArray>(std::move(a))) {}
    JsonValue(JsonObject o) : type_(Type::Object), obj_(std::make_shared<JsonObject>(std::move(o))) {}

    [[nodiscard]] Type GetType() const { return type_; }
    [[nodiscard]] bool IsNull() const { return type_ == Type::Null; }
    [[nodiscard]] bool IsBool() const { return type_ == Type::Bool; }
    [[nodiscard]] bool IsNumber() const { return type_ == Type::Number; }
    [[nodiscard]] bool IsString() const { return type_ == Type::String; }
    [[nodiscard]] bool IsArray() const { return type_ == Type::Array; }
    [[nodiscard]] bool IsObject() const { return type_ == Type::Object; }

    [[nodiscard]] bool AsBool(bool def = false) const { return type_ == Type::Bool ? bool_ : def; }
    [[nodiscard]] double AsNumber(double def = 0) const { return type_ == Type::Number ? num_ : def; }
    [[nodiscard]] f32 AsFloat(f32 def = 0) const { return type_ == Type::Number ? static_cast<f32>(num_) : def; }
    [[nodiscard]] i32 AsInt(i32 def = 0) const { return type_ == Type::Number ? static_cast<i32>(num_) : def; }
    [[nodiscard]] const std::string& AsString() const { return str_; }
    [[nodiscard]] std::string AsString(const std::string& def) const {
        return type_ == Type::String ? str_ : def;
    }

    // Доступ к массиву
    [[nodiscard]] usize Size() const {
        if (type_ == Type::Array) return arr_->size();
        if (type_ == Type::Object) return obj_->size();
        return 0;
    }
    [[nodiscard]] bool Empty() const { return Size() == 0; }
    const JsonValue& operator[](usize i) const;
    JsonValue& operator[](usize i);

    // Доступ к объекту
    [[nodiscard]] bool Has(const std::string& key) const {
        return type_ == Type::Object && obj_ && obj_->count(key) != 0;
    }
    const JsonValue& operator[](const std::string& key) const;
    JsonValue& operator[](const std::string& key);
    [[nodiscard]] f32 GetFloat(const std::string& key, f32 def = 0) const {
        auto* v = Find(key);
        return v ? v->AsFloat(def) : def;
    }
    [[nodiscard]] i32 GetInt(const std::string& key, i32 def = 0) const {
        auto* v = Find(key);
        return v ? v->AsInt(def) : def;
    }
    [[nodiscard]] bool GetBool(const std::string& key, bool def = false) const {
        auto* v = Find(key);
        return v ? v->AsBool(def) : def;
    }
    [[nodiscard]] std::string GetString(const std::string& key, const std::string& def = "") const {
        auto* v = Find(key);
        return (v && v->IsString()) ? v->AsString() : def;
    }
    [[nodiscard]] const JsonValue* Find(const std::string& key) const {
        if (type_ != Type::Object || !obj_) return nullptr;
        auto it = obj_->find(key);
        return it == obj_->end() ? nullptr : &it->second;
    }
    [[nodiscard]] const JsonObject& Object() const;
    [[nodiscard]] const JsonArray& Array() const;

    // Изменение
    void Push(JsonValue v);
    void Set(const std::string& key, JsonValue v);

    // Сериализация
    [[nodiscard]] std::string Dump(int indent = -1) const;

    static JsonValue Parse(const std::string& text, std::string* error = nullptr);
    static bool ParseFile(const std::string& path, JsonValue* out, std::string* error = nullptr);

private:
    void DumpTo(std::string& out, int indent, int depth) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0;
    std::string str_;
    std::shared_ptr<JsonArray> arr_;
    std::shared_ptr<JsonObject> obj_;
};

// Удобные конструкторы.
JsonValue JsonMakeArray();
JsonValue JsonMakeObject();

}  // namespace crossrender
