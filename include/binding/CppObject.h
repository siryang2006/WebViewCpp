#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <map>
#include <vector>
#include <functional>
#include <memory>
#include <atomic>
#include <stdexcept>
#include <tuple>
#include <type_traits>

class JsCallback;
class WebViewWrapper;

using json = nlohmann::json;

// ============================================================
// 错误码
// ============================================================
enum class ErrorCode {
    OK                 = 0,
    OBJECT_NOT_FOUND   = -1,
    METHOD_NOT_FOUND   = -2,
    PROPERTY_NOT_FOUND = -3,
    INVALID_ARGUMENTS  = -4,
    OBJECT_DESTROYED   = -5,
    INTERNAL_ERROR     = -10,
    USER_ERROR         = -100,
};

class BindingException : public std::runtime_error {
public:
    explicit BindingException(ErrorCode code, const std::string& msg)
        : std::runtime_error(msg), m_code(code) {}
    ErrorCode code() const { return m_code; }
    // 结构化错误：所有 binding 错误统一返回此格式，
    // JS 端可直接检查 result.ok === false 并读取 code/message。
    json to_json() const {
        return { {"ok", false}, {"code", static_cast<int>(m_code)}, {"message", what()} };
    }
    // 工厂方法：将任意异常转换为 BindingException。
    // 若已是 BindingException 直接返回，否则包装为 INTERNAL_ERROR。
    static BindingException from(const std::exception& e) {
        if (auto* be = dynamic_cast<const BindingException*>(&e)) return *be;
        return BindingException(ErrorCode::INTERNAL_ERROR, e.what());
    }
private:
    ErrorCode m_code;
};

struct ErrorResult {
    ErrorCode code = ErrorCode::OK;
    std::string message;
    ErrorResult() = default;
    ErrorResult(ErrorCode c, std::string m) : code(c), message(std::move(m)) {}
    bool ok() const { return code == ErrorCode::OK; }
    json to_json() const {
        return { {"ok", false}, {"code", static_cast<int>(code)}, {"message", message} };
    }
};

// ============================================================
// JsCallback
// ============================================================
class JsCallback {
public:
    JsCallback() = default;
    JsCallback(WebViewWrapper* wv, const std::string& callback_id)
        : m_wv(wv), m_id(callback_id) {}
    void resolve(const json& result);
    void reject(const std::string& error);
    bool valid() const { return m_wv != nullptr && !m_id.empty(); }
    const std::string& id() const { return m_id; }
private:
    WebViewWrapper* m_wv = nullptr;
    std::string m_id;
};

// ============================================================
// CppObject 基类
// ============================================================
class CppObject : public std::enable_shared_from_this<CppObject> {
public:
    CppObject() : m_id(next_id()) {}
    virtual ~CppObject() { mark_destroyed(); }

    CppObject(const CppObject&) = delete;
    CppObject& operator=(const CppObject&) = delete;

    virtual std::string object_name() const = 0;
    virtual void on_created() {}
    virtual void on_destroyed() {}

    int64_t instance_id() const { return m_id; }
    bool is_destroyed() const { return m_destroyed.load(); }

    using SyncMethod  = std::function<json(const json& args)>;
    using AsyncMethod = std::function<void(const std::string& id, const json& args, WebViewWrapper* wv)>;

    // ============================================================
    // 同步方法绑定
    // 用法: bind_sync("add", [](int a, int b) { return a + b; });
    // ============================================================
    template<typename Fn>
    void bind_sync(const std::string& name, Fn fn) {
        m_sync_methods[name] = [fn = std::move(fn), name](const json& args) -> json {
            return fn_impl(fn, args, name);
        };
    }

    // ============================================================
    // 异步方法绑定
    // 用法: bind_async("fetch", [](const std::string& id, const json& args, WebViewWrapper* wv) {
    //         wv->dispatch_task([id, args, wv]() { wv->resolve(id, "ok"); });
    //       });
    // ============================================================
    template<typename Fn>
    void bind_async(const std::string& name, Fn fn) {
        m_async_methods[name] = [fn = std::move(fn)](const std::string& id, const json& args, WebViewWrapper* wv) {
            fn(id, args, wv);
        };
    }

    // 属性绑定
    template<typename Getter>
    void bind_property(const std::string& name, Getter get) {
        m_property_names.push_back(name);
        m_properties[name] = [get = std::move(get)]() -> json {
            using RetType = std::invoke_result_t<Getter>;
            if constexpr (std::is_same_v<RetType, void>) { get(); return nullptr; }
            else if constexpr (std::is_same_v<RetType, json>) { return get(); }
            else { return json(get()); }
        };
    }

    template<typename Getter, typename Setter>
    void bind_property(const std::string& name, Getter get, Setter set) {
        m_property_names.push_back(name);
        m_properties[name] = [get = std::move(get)]() -> json { return json(get()); };
        m_prop_setters[name] = [set = std::move(set)](const json& val) { set(val.get<std::invoke_result_t<Getter>>()); };
    }

    // 内部调用接口
    json invoke_sync(const std::string& method, const json& args) {
        if (m_destroyed.load())
            throw BindingException(ErrorCode::OBJECT_DESTROYED, "Object destroyed");
        auto it = m_sync_methods.find(method);
        if (it == m_sync_methods.end())
            throw BindingException(ErrorCode::METHOD_NOT_FOUND, "Method not found: " + method);
        try { return it->second(args); }
        catch (const BindingException&) { throw; }
        catch (const std::exception& e) { throw BindingException(ErrorCode::INTERNAL_ERROR, e.what()); }
    }

    void invoke_async(const std::string& method, const std::string& id,
                      const json& args, WebViewWrapper* wv);

    json get_property(const std::string& name) {
        if (m_destroyed.load()) throw BindingException(ErrorCode::OBJECT_DESTROYED, "Object destroyed");
        auto it = m_properties.find(name);
        if (it == m_properties.end()) throw BindingException(ErrorCode::PROPERTY_NOT_FOUND, name);
        return it->second();
    }

    void set_property(const std::string& name, const json& val) {
        if (m_destroyed.load()) throw BindingException(ErrorCode::OBJECT_DESTROYED, "Object destroyed");
        auto it = m_prop_setters.find(name);
        if (it == m_prop_setters.end()) throw BindingException(ErrorCode::PROPERTY_NOT_FOUND, name);
        it->second(val);
    }

    const std::map<std::string, SyncMethod>& sync_methods() const { return m_sync_methods; }
    const std::map<std::string, AsyncMethod>& async_methods() const { return m_async_methods; }
    const std::vector<std::string>& property_names() const { return m_property_names; }
    bool has_setter(const std::string& name) const { return m_prop_setters.count(name) > 0; }

    static json ok_result(const json& data = nullptr) { return {{"ok", true}, {"data", data}}; }
    static json error_result(ErrorCode code, const std::string& message) { return ErrorResult(code, message).to_json(); }
    static std::string js_escape(const std::string& s) {
        std::string out;
        for (char c : s) {
            switch (c) {
                case '\'': out += "\\'"; break; case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break; case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break; case '"':  out += "\\\""; break;
                default:   out += c; break;
            }
        }
        return out;
    }

protected:
    // 幂等：on_destroyed() 只会在首次标记销毁时调用一次。
    // 既可由 JS 端显式 __destroy__ 触发，也可由 ~CppObject 触发，避免重复回调。
    void mark_destroyed() {
        if (!m_destroyed.exchange(true)) on_destroyed();
    }

private:
    static int64_t next_id() { static std::atomic<int64_t> id{0}; return ++id; }

    // 类型名称映射（用于错误信息）
    template<typename T>
    static constexpr const char* type_name() {
        if constexpr (std::is_same_v<T, int>) return "int";
        else if constexpr (std::is_same_v<T, bool>) return "bool";
        else if constexpr (std::is_same_v<T, double>) return "double";
        else if constexpr (std::is_same_v<T, std::string>) return "string";
        else if constexpr (std::is_same_v<T, json>) return "json";
        else return "unknown";
    }

    // 编译期检查：禁止不合理类型
    template<typename T>
    constexpr void check_arg_type() {
        static_assert(!std::is_pointer_v<T>,
            "bind_sync/async: pointer types not allowed, use value/reference types");
        static_assert(!std::is_void_v<T>,
            "bind_sync/async: void not allowed as argument type");
        static_assert(!std::is_reference_v<T>,
            "bind_sync/async: reference types not allowed, use value types");
        static_assert(std::is_default_constructible_v<T> || std::is_same_v<T, json>,
            "bind_sync/async: type must be default constructible or json");
    }

    // 参数类型提取（从 lambda operator() 签名）
    template<typename> struct fn_sig;
    template<typename R, typename C, typename... Args>
    struct fn_sig<R (C::*)(Args...) const> {
        using ret_type = R;
        using args_tuple = std::tuple<std::remove_cvref_t<Args>...>;
        static constexpr size_t arg_count = sizeof...(Args);
        constexpr static void validate() { validate_all_types<std::remove_cvref_t<Args>...>(); }
    };
    template<typename R, typename C, typename... Args>
    struct fn_sig<R (C::*)(Args...)> {
        using ret_type = R;
        using args_tuple = std::tuple<std::remove_cvref_t<Args>...>;
        static constexpr size_t arg_count = sizeof...(Args);
        constexpr static void validate() { validate_all_types<std::remove_cvref_t<Args>...>(); }
    };

    // 带详细错误信息的参数提取
    template<typename T>
    static T extract_one(const json& args, size_t idx, const std::string& name) {
        try {
            return args[idx].get<T>();
        } catch (const json::exception&) {
            std::string actual_type;
            if (args[idx].is_null()) actual_type = "null";
            else if (args[idx].is_boolean()) actual_type = "boolean";
            else if (args[idx].is_number_integer()) actual_type = "integer";
            else if (args[idx].is_number_float()) actual_type = "float";
            else if (args[idx].is_string()) actual_type = "string";
            else if (args[idx].is_array()) actual_type = "array";
            else if (args[idx].is_object()) actual_type = "object";
            else actual_type = "unknown";
            throw std::runtime_error("[" + name + "] Argument " + std::to_string(idx) +
                " type mismatch: expected " + type_name<T>() +
                ", got " + actual_type);
        }
    }

    template<typename Tuple, size_t... Is>
    static Tuple extract_args(const json& args, std::index_sequence<Is...>, const std::string& name) {
        return Tuple{extract_one<std::tuple_element_t<Is, Tuple>>(args, Is, name)...};
    }

    // 用 tuple 解包调用函数
    template<typename Fn, typename Tuple, size_t... Is>
    static json apply_with_index(Fn& fn, Tuple& tup, std::index_sequence<Is...>) {
        using sig = fn_sig<decltype(&Fn::operator())>;
        if constexpr (std::is_same_v<typename sig::ret_type, void>) {
            fn(std::get<Is>(tup)...);
            return nullptr;
        } else {
            return json(fn(std::get<Is>(tup)...));
        }
    }

    // 类型擦除模式: 直接传递 json
    template<typename Fn>
    static json fn_impl_json(Fn& fn, const json& args, const std::string&) {
        return fn(args);
    }

    // 类型安全模式: 提取参数并调用
    template<typename Fn>
    static json fn_impl_typed(Fn& fn, const json& args, const std::string& name) {
        using sig = fn_sig<decltype(&Fn::operator())>;
        sig::validate();
        if (args.size() != sig::arg_count)
            throw std::runtime_error("[" + name + "] Argument count mismatch: expected " +
                std::to_string(sig::arg_count) + ", got " + std::to_string(args.size()));
        auto tup = extract_args<typename sig::args_tuple>(args,
            std::make_index_sequence<sig::arg_count>{}, name);
        return apply_with_index(fn, tup, std::make_index_sequence<sig::arg_count>{});
    }

    template<typename Fn>
    static json fn_impl(Fn& fn, const json& args, const std::string& name) {
        using sig = fn_sig<decltype(&Fn::operator())>;
        constexpr bool is_json = (sig::arg_count == 1 &&
            std::is_same_v<std::remove_cvref_t<std::tuple_element_t<0, typename sig::args_tuple>>, json>);
        if constexpr (is_json) {
            return fn_impl_json(fn, args, name);
        } else {
            return fn_impl_typed(fn, args, name);
        }
    }

    template<typename... Ts>
    constexpr static void validate_all_types() { (check_arg_type<Ts>(), ...); }

    int64_t m_id;
    std::atomic<bool> m_destroyed{false};
    std::map<std::string, SyncMethod> m_sync_methods;
    std::map<std::string, AsyncMethod> m_async_methods;
    std::map<std::string, std::function<json()>> m_properties;
    std::map<std::string, std::function<void(const json&)>> m_prop_setters;
    std::vector<std::string> m_property_names;

    friend class WebViewWrapper;
};
