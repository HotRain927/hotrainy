// tbg/json.h — 迷你 JSON（无第三方依赖）
// 支持：object / array / string / number / bool / null，UTF-8，\u 转义。
// 只为配置文件与 IPC 设计，不追求完整规范（如 NaN、超深嵌套）。
#pragma once

#include <string>
#include <vector>
#include <utility>

namespace tbg {
namespace json {

struct Value {
    enum Type { NUL = 0, BOOL, NUM, STR, ARR, OBJ };
    Type type = NUL;

    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    // ── 构造 ──
    static Value makeNull();
    static Value makeBool(bool v);
    static Value makeNum(double v);
    static Value makeStr(const std::string& v);
    static Value makeArr();
    static Value makeObj();

    // ── 查询 ──
    bool isObj() const { return type == OBJ; }
    bool isArr() const { return type == ARR; }
    bool isStr() const { return type == STR; }
    bool isNum() const { return type == NUM; }
    bool isBool() const { return type == BOOL; }

    const Value* find(const std::string& key) const;
    bool has(const std::string& key) const { return find(key) != nullptr; }

    bool getBool(const std::string& key, bool def) const;
    double getNum(const std::string& key, double def) const;
    std::string getStr(const std::string& key, const std::string& def) const;

    // ── 写入（obj 时按 key 覆盖或追加） ──
    Value& set(const std::string& key, const Value& v);
    Value& setBool(const std::string& key, bool v);
    Value& setNum(const std::string& key, double v);
    Value& setStr(const std::string& key, const std::string& v);
    void push(const Value& v);
};

bool parse(const std::string& text, Value& out, std::string& err);
std::string dump(const Value& v, bool pretty = true);

}  // namespace json
}  // namespace tbg
