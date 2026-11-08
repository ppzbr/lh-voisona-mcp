// LhBridge - an in-process MCP bridge for the native singing editors.
//
// VoiSona and VOCALOID 6 have no scripting surface. Their note model is native
// C++ living inside the editor process, so - unlike CeVIO, whose engine is
// managed and simply refuses to authenticate out-of-process - there is no
// out-of-process route to consider at all. The bridge runs inside the editor.
//
// This is the same shape as the CeVIO bridge and as OpenUtau's: a small DLL in
// the host process, publishing MCP on loopback HTTP. What differs is the middle:
// instead of calling a published API, this bridge exposes the *primitives* for
// reaching an object graph that has no API -
//
//   <engine>_modules             loaded modules with base and size
//   <engine>_scan                signature scan, "48 8B ?? ??" wildcards
//   <engine>_read / _write       raw bytes
//   <engine>_chain               resolve module+RVA -> pointer chain -> typed value
//   <engine>_chain_write         the same, writing
//   <engine>_windows             this process's windows
//   <engine>_status              bridge, app baseline, capability levels
//
// The per-build structure offsets are *data*, not code: a map file in the shape
// docs/mcp-engine-bridge.md describes binds the note vocabulary onto chains. That
// keeps the brittle part (offsets that change when the editor updates) in a file
// that can be re-learned, instead of baked into a binary that then lies.
//
// The DLL is injected with the existing plumbing (lhai/live/inject.py) and is
// unloaded by the existing agent protocol; nothing is hooked, and no code runs
// unless a client asks for a specific command.
//
// Build: bridges/native/build.ps1  ->  lhai_bridge.dll  and  lhai_bridge.exe
//        (the .exe is the same server standalone, so the transport can be tested
//         without injecting anything into anyone's editor)

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <utility>

#ifndef IMAGE_DOS_SIGNATURE
#define IMAGE_DOS_SIGNATURE 0x5A4D
#endif

// ===========================================================================
// 1. a small JSON value model - enough for MCP envelopes, nothing more
// ===========================================================================
namespace json {

struct Value;
using Object = std::vector<std::pair<std::string, Value>>;
using Array = std::vector<Value>;

struct Value {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
    bool boolean = false;
    double number = 0;
    std::string text;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;

    Value() {}
    static Value make_null() { return Value(); }
    static Value make_bool(bool b) { Value v; v.kind = Bool; v.boolean = b; return v; }
    static Value make_num(double d) { Value v; v.kind = Num; v.number = d; return v; }
    static Value make_str(const std::string &s) { Value v; v.kind = Str; v.text = s; return v; }
    static Value make_arr() { Value v; v.kind = Arr; return v; }
    static Value make_obj() { Value v; v.kind = Obj; return v; }

    Value &set(const std::string &key, Value v) {
        if (kind != Obj) { kind = Obj; }
        for (auto &kv : object) {
            if (kv.first == key) { kv.second = std::move(v); return *this; }
        }
        object.emplace_back(key, std::move(v));
        return *this;
    }
    Value &push(Value v) {
        if (kind != Arr) { kind = Arr; }
        array.push_back(std::move(v));
        return *this;
    }
    const Value *find(const std::string &key) const {
        if (kind != Obj) return nullptr;
        for (auto &kv : object) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    std::string as_string(const std::string &fallback = "") const {
        return kind == Str ? text : fallback;
    }
    double as_num(double fallback = 0) const {
        return kind == Num ? number : fallback;
    }
    bool as_bool(bool fallback = false) const {
        return kind == Bool ? boolean : fallback;
    }
    std::string get_str(const std::string &key, const std::string &fallback = "") const {
        const Value *v = find(key);
        return v ? v->as_string(fallback) : fallback;
    }
    double get_num(const std::string &key, double fallback = 0) const {
        const Value *v = find(key);
        return v ? v->as_num(fallback) : fallback;
    }
    const Array *get_arr(const std::string &key) const {
        const Value *v = find(key);
        return (v && v->kind == Arr) ? &v->array : nullptr;
    }
};

// ------------------------------------------------------------------- parser
struct Parser {
    const char *at;
    const char *end;
    std::string error;

    void ws() {
        while (at < end && (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r')) ++at;
    }
    bool fail(const char *why) { if (error.empty()) error = why; return false; }

    bool parse(Value &out) {
        ws();
        if (at >= end) return fail("unexpected end of input");
        char c = *at;
        if (c == '{') return parse_object(out);
        if (c == '[') return parse_array(out);
        if (c == '"') { out.kind = Value::Str; return parse_string(out.text); }
        if (c == 't') { if (end - at >= 4 && !strncmp(at, "true", 4)) { at += 4; out = Value::make_bool(true); return true; } return fail("bad literal"); }
        if (c == 'f') { if (end - at >= 5 && !strncmp(at, "false", 5)) { at += 5; out = Value::make_bool(false); return true; } return fail("bad literal"); }
        if (c == 'n') { if (end - at >= 4 && !strncmp(at, "null", 4)) { at += 4; out = Value::make_null(); return true; } return fail("bad literal"); }
        return parse_number(out);
    }

    bool parse_object(Value &out) {
        ++at;  // {
        out = Value::make_obj();
        ws();
        if (at < end && *at == '}') { ++at; return true; }
        for (;;) {
            ws();
            std::string key;
            if (!parse_string(key)) return false;
            ws();
            if (at >= end || *at != ':') return fail("expected ':'");
            ++at;
            Value child;
            if (!parse(child)) return false;
            out.object.emplace_back(key, std::move(child));
            ws();
            if (at >= end) return fail("unterminated object");
            if (*at == ',') { ++at; continue; }
            if (*at == '}') { ++at; return true; }
            return fail("expected ',' or '}'");
        }
    }

    bool parse_array(Value &out) {
        ++at;  // [
        out = Value::make_arr();
        ws();
        if (at < end && *at == ']') { ++at; return true; }
        for (;;) {
            Value child;
            if (!parse(child)) return false;
            out.array.push_back(std::move(child));
            ws();
            if (at >= end) return fail("unterminated array");
            if (*at == ',') { ++at; continue; }
            if (*at == ']') { ++at; return true; }
            return fail("expected ',' or ']'");
        }
    }

    bool parse_string(std::string &out) {
        if (at >= end || *at != '"') return fail("expected string");
        ++at;
        out.clear();
        while (at < end) {
            char c = *at++;
            if (c == '"') return true;
            if (c != '\\') { out.push_back(c); continue; }
            if (at >= end) return fail("bad escape");
            char e = *at++;
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (end - at < 4) return fail("bad \\u escape");
                    unsigned code = 0;
                    for (int i = 0; i < 4; ++i) {
                        char h = *at++;
                        code <<= 4;
                        if (h >= '0' && h <= '9') code |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') code |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') code |= (unsigned)(h - 'A' + 10);
                        else return fail("bad hex in \\u escape");
                    }
                    // UTF-8 encode.
                    if (code < 0x80) out.push_back((char)code);
                    else if (code < 0x800) {
                        out.push_back((char)(0xC0 | (code >> 6)));
                        out.push_back((char)(0x80 | (code & 0x3F)));
                    } else {
                        out.push_back((char)(0xE0 | (code >> 12)));
                        out.push_back((char)(0x80 | ((code >> 6) & 0x3F)));
                        out.push_back((char)(0x80 | (code & 0x3F)));
                    }
                    break;
                }
                default: return fail("unknown escape");
            }
        }
        return fail("unterminated string");
    }

    bool parse_number(Value &out) {
        const char *start = at;
        if (at < end && (*at == '-' || *at == '+')) ++at;
        bool any = false;
        while (at < end && ((*at >= '0' && *at <= '9') || *at == '.' || *at == 'e' || *at == 'E' || *at == '+' || *at == '-')) { ++at; any = true; }
        if (!any) return fail("expected value");
        std::string token(start, (size_t)(at - start));
        out = Value::make_num(strtod(token.c_str(), nullptr));
        return true;
    }
};

inline bool parse(const std::string &text, Value &out, std::string &error) {
    Parser p;
    p.at = text.data();
    p.end = text.data() + text.size();
    if (!p.parse(out)) { error = p.error.empty() ? "parse error" : p.error; return false; }
    return true;
}

// ---------------------------------------------------------------- serializer
inline void escape_into(const std::string &s, std::string &out) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back((char)c);
                }
        }
    }
    out.push_back('"');
}

inline void write_into(const Value &v, std::string &out, bool pretty, int depth) {
    auto indent = [&](int d) { if (pretty) { out.push_back('\n'); out.append((size_t)d * 2, ' '); } };
    switch (v.kind) {
        case Value::Null: out += "null"; break;
        case Value::Bool: out += v.boolean ? "true" : "false"; break;
        case Value::Num: {
            double d = v.number;
            if (d == (double)(long long)d && std::fabs(d) < 1e15) {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%lld", (long long)d);
                out += buf;
            } else {
                char buf[40];
                std::snprintf(buf, sizeof buf, "%.10g", d);
                out += buf;
            }
            break;
        }
        case Value::Str: escape_into(v.text, out); break;
        case Value::Arr: {
            if (v.array.empty()) { out += "[]"; break; }
            out.push_back('[');
            for (size_t i = 0; i < v.array.size(); ++i) {
                if (i) out.push_back(',');
                indent(depth + 1);
                write_into(v.array[i], out, pretty, depth + 1);
            }
            indent(depth);
            out.push_back(']');
            break;
        }
        case Value::Obj: {
            if (v.object.empty()) { out += "{}"; break; }
            out.push_back('{');
            for (size_t i = 0; i < v.object.size(); ++i) {
                if (i) out.push_back(',');
                indent(depth + 1);
                escape_into(v.object[i].first, out);
                out.push_back(':');
                if (pretty) out.push_back(' ');
                write_into(v.object[i].second, out, pretty, depth + 1);
            }
            indent(depth);
            out.push_back('}');
            break;
        }
    }
}

inline std::string dump(const Value &v, bool pretty = false) {
    std::string out;
    write_into(v, out, pretty, 0);
    return out;
}

}  // namespace json

// ===========================================================================
// 2. shared helpers
// ===========================================================================
static std::string narrow(const std::wstring &s) {
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}

static std::string narrow_ansi(const char *s) { return std::string(s ? s : ""); }

static std::string hex_u64(uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%llx", (unsigned long long)v);
    return std::string(buf);
}

static bool parse_hex(const std::string &text, uint64_t &out) {
    std::string t = text;
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) t = t.substr(2);
    if (t.empty()) return false;
    uint64_t value = 0;
    for (char c : t) {
        int digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        value = (value << 4) | (uint64_t)digit;
    }
    out = value;
    return true;
}

static std::vector<std::string> split(const std::string &text, char sep) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        size_t at = text.find(sep, start);
        if (at == std::string::npos) { parts.push_back(text.substr(start)); return parts; }
        parts.push_back(text.substr(start, at - start));
        start = at + 1;
    }
}

static uint64_t module_base(const std::string &name) {
    if (name.empty() || name == "*") return (uint64_t)(uintptr_t)GetModuleHandleW(nullptr);
    HMODULE m = GetModuleHandleA(name.c_str());
    return (uint64_t)(uintptr_t)m;
}

static uint64_t module_extent(HMODULE module) {
    MODULEINFO info = {0};
    if (GetModuleInformation(GetCurrentProcess(), module, &info, sizeof info) && info.SizeOfImage) {
        return info.SizeOfImage;
    }
    auto *base = (uint8_t *)module;
    if (!base) return 0;
    auto *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    return nt->OptionalHeader.SizeOfImage;
}

static bool readable(uint64_t address, size_t size) {
    uint64_t cursor = address;
    uint64_t end = address + size;
    if (end < address) return false;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((LPCVOID)(uintptr_t)cursor, &mbi, sizeof mbi) == 0) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & PAGE_GUARD) return false;
        DWORD p = mbi.Protect & 0xFF;
        bool ok = p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_WRITECOPY ||
                  p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
        if (!ok) return false;
        uint64_t region_end = (uint64_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (region_end <= cursor) return false;
        cursor = region_end;
    }
    return true;
}

static bool parse_pattern(const std::string &text, std::vector<uint8_t> &bytes, std::vector<uint8_t> &mask) {
    for (const std::string &token : split(text, ' ')) {
        if (token.empty()) continue;
        if (token == "?" || token == "??" || token == "**") { bytes.push_back(0); mask.push_back(0); continue; }
        if (token.size() != 2) return false;
        uint64_t value = 0;
        if (!parse_hex(token, value)) return false;
        bytes.push_back((uint8_t)value);
        mask.push_back(0xFF);
    }
    return !bytes.empty();
}

static std::vector<uint64_t> scan(uint64_t base, uint64_t size, const std::string &pattern, size_t cap) {
    std::vector<uint64_t> hits;
    std::vector<uint8_t> wanted, mask;
    if (!parse_pattern(pattern, wanted, mask) || !base || !size) return hits;
    uint64_t end = base + size;
    uint64_t cursor = base;
    while (cursor < end && hits.size() < cap) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((LPCVOID)(uintptr_t)cursor, &mbi, sizeof mbi) == 0) break;
        uint64_t region_start = (uint64_t)(uintptr_t)mbi.BaseAddress;
        uint64_t region_end = region_start + mbi.RegionSize;
        if (region_end <= cursor) break;
        DWORD p = mbi.Protect & 0xFF;
        bool ok = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
                  (p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_WRITECOPY ||
                   p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY);
        if (ok) {
            uint64_t start = cursor > region_start ? cursor : region_start;
            uint64_t stop = region_end < end ? region_end : end;
            const uint8_t *data = (const uint8_t *)(uintptr_t)start;
            size_t limit = (size_t)(stop - start);
            if (limit >= wanted.size()) {
                size_t last = limit - wanted.size();
                for (size_t i = 0; i <= last && hits.size() < cap; ++i) {
                    size_t j = 0;
                    for (; j < wanted.size(); ++j) if (mask[j] && data[i + j] != wanted[j]) break;
                    if (j == wanted.size()) hits.push_back(start + i);
                }
            }
        }
        cursor = region_end;
    }
    return hits;
}

// ===========================================================================
// 3. engine identity - which editor are we inside?
// ===========================================================================
struct Engine {
    std::string id;        // "voisona" | "vocaloid" | "cevio" | "synthv" | "unknown"
    std::string exe;       // main module file name
    std::string version;   // file version, when available
    std::string path;
};

static std::string lower(std::string s) {
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

static Engine detect_engine() {
    Engine e;
    wchar_t full[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, full, MAX_PATH);
    e.path = narrow(full);
    size_t slash = e.path.find_last_of("\\/");
    e.exe = slash == std::string::npos ? e.path : e.path.substr(slash + 1);

    std::string low = lower(e.exe);
    if (low.find("voisona") != std::string::npos) e.id = "voisona";
    else if (low.find("vocaloid") != std::string::npos) e.id = "vocaloid";
    else if (low.find("cevio") != std::string::npos) e.id = "cevio";
    else if (low.find("synthv") != std::string::npos || low.find("synthesizer") != std::string::npos) e.id = "synthv";
    else e.id = "unknown";

    DWORD dummy = 0;
    DWORD size = GetFileVersionInfoSizeW(full, &dummy);
    if (size) {
        std::vector<char> buffer(size);
        if (GetFileVersionInfoW(full, 0, size, buffer.data())) {
            VS_FIXEDFILEINFO *info = nullptr;
            UINT len = 0;
            if (VerQueryValueW(buffer.data(), L"\\", (LPVOID *)&info, &len) && info) {
                char buf[64];
                std::snprintf(buf, sizeof buf, "%u.%u.%u.%u",
                    HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                    HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
                e.version = buf;
            }
        }
    }
    return e;
}

// ===========================================================================
// 4. tools
// ===========================================================================
static const char *kBridgeVersion = "0.1.0";
static const int kProtocol = 1;
static const char *kBuiltAgainst = "native x64 (VoiSona / VOCALOID6)";
static const int kPortFirst = 43102;
static const int kPortLast = 43111;

static Engine g_engine;
static std::string g_mapPath;
static int g_port = 0;
static std::string g_dir;
static HMODULE g_self = nullptr;

static std::string tool_prefix() { return g_engine.id + "_"; }

static json::Value ok_value() { json::Value v = json::Value::make_obj(); v.set("ok", json::Value::make_bool(true)); return v; }
static json::Value err_value(const std::string &message) {
    json::Value v = json::Value::make_obj();
    v.set("ok", json::Value::make_bool(false));
    v.set("error", json::Value::make_str(message));
    return v;
}

// Read one typed value. `kind` is one of the type names the map files use.
static bool read_typed(uint64_t address, const std::string &kind, json::Value &out, std::string &error, int count) {
    if (count < 1) count = 1;
    if (kind == "u8" || kind == "i8" || kind == "bool") {
        if (!readable(address, (size_t)count)) return (error = "address is not readable", false);
        json::Value arr = json::Value::make_arr();
        const uint8_t *p = (const uint8_t *)(uintptr_t)address;
        for (int i = 0; i < count; ++i) arr.push(json::Value::make_num(p[i]));
        out = count == 1 ? arr.array[0] : arr;
        return true;
    }
    if (kind == "u16" || kind == "i16") {
        size_t bytes = (size_t)count * 2;
        if (!readable(address, bytes)) return (error = "address is not readable", false);
        json::Value arr = json::Value::make_arr();
        for (int i = 0; i < count; ++i) {
            uint16_t v = 0;
            memcpy(&v, (const void *)(uintptr_t)(address + i * 2), 2);
            arr.push(json::Value::make_num(kind == "i16" ? (double)(int16_t)v : (double)v));
        }
        out = count == 1 ? arr.array[0] : arr;
        return true;
    }
    if (kind == "u32" || kind == "i32") {
        size_t bytes = (size_t)count * 4;
        if (!readable(address, bytes)) return (error = "address is not readable", false);
        json::Value arr = json::Value::make_arr();
        for (int i = 0; i < count; ++i) {
            uint32_t v = 0;
            memcpy(&v, (const void *)(uintptr_t)(address + i * 4), 4);
            arr.push(json::Value::make_num(kind == "i32" ? (double)(int32_t)v : (double)v));
        }
        out = count == 1 ? arr.array[0] : arr;
        return true;
    }
    if (kind == "u64" || kind == "i64" || kind == "ptr") {
        size_t bytes = (size_t)count * 8;
        if (!readable(address, bytes)) return (error = "address is not readable", false);
        json::Value arr = json::Value::make_arr();
        for (int i = 0; i < count; ++i) {
            uint64_t v = 0;
            memcpy(&v, (const void *)(uintptr_t)(address + i * 8), 8);
            if (kind == "ptr") {
                arr.push(json::Value::make_str("0x" + hex_u64(v)));
            } else {
                arr.push(json::Value::make_num((double)(int64_t)v));
            }
        }
        out = count == 1 ? arr.array[0] : arr;
        return true;
    }
    if (kind == "f32") {
        size_t bytes = (size_t)count * 4;
        if (!readable(address, bytes)) return (error = "address is not readable", false);
        json::Value arr = json::Value::make_arr();
        for (int i = 0; i < count; ++i) {
            float v = 0;
            memcpy(&v, (const void *)(uintptr_t)(address + i * 4), 4);
            arr.push(json::Value::make_num((double)v));
        }
        out = count == 1 ? arr.array[0] : arr;
        return true;
    }
    if (kind == "f64") {
        size_t bytes = (size_t)count * 8;
        if (!readable(address, bytes)) return (error = "address is not readable", false);
        json::Value arr = json::Value::make_arr();
        for (int i = 0; i < count; ++i) {
            double v = 0;
            memcpy(&v, (const void *)(uintptr_t)(address + i * 8), 8);
            arr.push(json::Value::make_num(v));
        }
        out = count == 1 ? arr.array[0] : arr;
        return true;
    }
    if (kind == "utf16") {
        if (!readable(address, 2)) return (error = "address is not readable", false);
        const wchar_t *p = (const wchar_t *)(uintptr_t)address;
        std::wstring s;
        for (int i = 0; i < 4096; ++i) {
            if (!readable((uint64_t)(uintptr_t)(p + i), 2)) { error = "string is not terminated"; return false; }
            if (p[i] == 0) break;
            s.push_back(p[i]);
        }
        out = json::Value::make_str(narrow(s));
        return true;
    }
    if (kind == "utf8") {
        if (!readable(address, 1)) return (error = "address is not readable", false);
        const char *p = (const char *)(uintptr_t)address;
        std::string s;
        for (int i = 0; i < 8192; ++i) {
            if (!readable((uint64_t)(uintptr_t)(p + i), 1)) { error = "string is not terminated"; return false; }
            if (p[i] == 0) break;
            s.push_back(p[i]);
        }
        out = json::Value::make_str(s);
        return true;
    }
    if (kind == "bytes") {
        if (!readable(address, (size_t)count)) return (error = "address is not readable", false);
        static const char *digits = "0123456789abcdef";
        std::string hex;
        hex.reserve((size_t)count * 2);
        const uint8_t *p = (const uint8_t *)(uintptr_t)address;
        for (int i = 0; i < count; ++i) { hex.push_back(digits[p[i] >> 4]); hex.push_back(digits[p[i] & 0xF]); }
        out = json::Value::make_str(hex);
        return true;
    }
    error = "unknown type: " + kind;
    return false;
}

// Resolve base -> pointer chain -> address. Chain semantics match the map files:
//   p = base; for each offset o: p = *(void**)p + o;   final p is the address.
static bool resolve_chain(const json::Value &args, uint64_t &address, std::string &error) {
    uint64_t base = 0;
    const json::Value *rva = args.find("rva");
    if (rva) {
        uint64_t offset = 0;
        std::string text = rva->kind == json::Value::Str ? rva->text : hex_u64((uint64_t)rva->number);
        if (!parse_hex(text, offset)) return (error = "bad rva", false);
        std::string module = args.get_str("module");
        uint64_t mb = module_base(module);
        if (!mb) return (error = "module not loaded: " + module, false);
        base = mb + offset;
    } else {
        std::string text = args.get_str("address");
        if (text.empty()) return (error = "either address or module+rva is required", false);
        if (!parse_hex(text, base)) return (error = "bad address", false);
    }

    const json::Array *offsets = args.get_arr("offsets");
    uint64_t p = base;
    if (offsets) {
        for (const json::Value &o : *offsets) {
            std::string text = o.kind == json::Value::Str ? o.text : hex_u64((uint64_t)o.number);
            uint64_t offset = 0;
            if (!parse_hex(text, offset)) return (error = "bad offset in chain", false);
            if (!readable(p, sizeof(uint64_t))) return (error = "chain is not readable at 0x" + hex_u64(p), false);
            uint64_t next = 0;
            memcpy(&next, (const void *)(uintptr_t)p, sizeof(uint64_t));
            if (!next) return (error = "chain hit a null pointer at 0x" + hex_u64(p), false);
            p = next + offset;
        }
    }
    address = p;
    return true;
}

static json::Value tool_status() {
    json::Value v = ok_value();
    v.set("bridge", ([] {
        json::Value b = json::Value::make_obj();
        b.set("version", json::Value::make_str(kBridgeVersion));
        b.set("protocol", json::Value::make_num(kProtocol));
        b.set("builtAgainst", json::Value::make_str(kBuiltAgainst));
        b.set("target", json::Value::make_str("x64"));
        return b;
    })());
    json::Value app = json::Value::make_obj();
    app.set("engine", json::Value::make_str(g_engine.id));
    app.set("exe", json::Value::make_str(g_engine.exe));
    app.set("path", json::Value::make_str(g_engine.path));
    app.set("version", json::Value::make_str(g_engine.version.empty() ? "unknown" : g_engine.version));
    v.set("app", app);

    json::Value caps = json::Value::make_obj();
    caps.set("modules", json::Value::make_str("full"));
    caps.set("scan", json::Value::make_str("full"));
    caps.set("read", json::Value::make_str("full"));
    caps.set("write", json::Value::make_str("full"));
    caps.set("chain", json::Value::make_str("full"));
    // The note vocabulary is bound by a map file. Until one is supplied the
    // honest level is "none" - a client then knows to run the discovery step
    // rather than calling a tool that would return nothing.
    caps.set("notes", json::Value::make_str(g_mapPath.empty() ? "none" : "mapped"));
    caps.set("note_map", json::Value::make_str(g_mapPath.empty() ? "none" : "loaded"));

    json::Value compat = json::Value::make_obj();
    compat.set("ok", json::Value::make_bool(true));
    compat.set("level", json::Value::make_str(g_mapPath.empty() ? "partial" : "full"));
    json::Value reasons = json::Value::make_arr();
    if (g_mapPath.empty()) reasons.push(json::Value::make_str(
        "note model is not resolved for build " + g_engine.exe +
        " " + (g_engine.version.empty() ? "unknown" : g_engine.version) +
        "; export the map with the discovery workflow and load it with load_map"));
    compat.set("reasons", reasons);
    compat.set("capabilities", caps);
    v.set("compatibility", compat);
    return v;
}

static json::Value tool_modules() {
    json::Value v = ok_value();
    json::Value arr = json::Value::make_arr();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return err_value("module snapshot failed");
    MODULEENTRY32W entry = {0};
    entry.dwSize = sizeof entry;
    if (Module32FirstW(snap, &entry)) {
        do {
            json::Value m = json::Value::make_obj();
            m.set("base", json::Value::make_str("0x" + hex_u64((uint64_t)(uintptr_t)entry.modBaseAddr)));
            m.set("size", json::Value::make_str("0x" + hex_u64(entry.modBaseSize)));
            m.set("name", json::Value::make_str(narrow(entry.szModule)));
            m.set("path", json::Value::make_str(narrow(entry.szExePath)));
            arr.push(m);
        } while (Module32NextW(snap, &entry));
    }
    CloseHandle(snap);
    v.set("modules", arr);
    return v;
}

static json::Value tool_scan(const json::Value &args) {
    std::string pattern = args.get_str("pattern");
    if (pattern.empty()) return err_value("pattern is required, e.g. \"48 8B ?? ?? E8\"");
    std::string module = args.get_str("module");
    uint64_t base = 0, size = 0;
    if (module.empty() || module == "*") {
        base = (uint64_t)(uintptr_t)GetModuleHandleW(nullptr);
        size = module_extent((HMODULE)(uintptr_t)base);
    } else {
        HMODULE m = GetModuleHandleA(module.c_str());
        if (!m) return err_value("module not loaded: " + module);
        base = (uint64_t)(uintptr_t)m;
        size = module_extent(m);
    }
    if (!base || !size) return err_value("could not size module: " + module);
    double cap = args.get_num("limit", 64);
    if (cap < 1) cap = 1;
    if (cap > 512) cap = 512;
    std::vector<uint64_t> hits = scan(base, size, pattern, (size_t)cap);
    json::Value v = ok_value();
    v.set("module", json::Value::make_str(module.empty() ? "*" : module));
    v.set("count", json::Value::make_num((double)hits.size()));
    json::Value arr = json::Value::make_arr();
    for (uint64_t h : hits) arr.push(json::Value::make_str("0x" + hex_u64(h)));
    v.set("hits", arr);
    return v;
}

static json::Value tool_read(const json::Value &args) {
    std::string text = args.get_str("address");
    uint64_t address = 0;
    if (text.empty()) {
        std::string error;
        json::Value a = args;
        if (!resolve_chain(a, address, error)) return err_value(error);
    } else if (!parse_hex(text, address)) {
        return err_value("bad address");
    }
    double size = args.get_num("size", 64);
    if (size < 1) size = 1;
    if (size > 65536) size = 65536;
    if (!readable(address, (size_t)size)) return err_value("address is not readable");
    json::Value v = ok_value();
    v.set("address", json::Value::make_str("0x" + hex_u64(address)));
    json::Value out;
    std::string error;
    if (!read_typed(address, "bytes", out, error, (int)size)) return err_value(error);
    v.set("hex", out);
    return v;
}

static json::Value tool_write(const json::Value &args) {
    uint64_t address = 0;
    std::string error;
    json::Value a = args;
    std::string text = args.get_str("address");
    if (!text.empty()) {
        if (!parse_hex(text, address)) return err_value("bad address");
    } else if (!resolve_chain(a, address, error)) {
        return err_value(error);
    }
    std::string hex = args.get_str("hex");
    if (hex.empty()) return err_value("hex payload is required");
    if (hex.size() % 2) return err_value("hex payload has odd length");
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < hex.size(); i += 2) {
        uint64_t value = 0;
        if (!parse_hex(hex.substr(i, 2), value)) return err_value("bad hex payload");
        bytes.push_back((uint8_t)value);
    }
    if (!readable(address, bytes.size())) return err_value("address is not readable");
    DWORD old = 0;
    if (!VirtualProtect((LPVOID)(uintptr_t)address, bytes.size(), PAGE_EXECUTE_READWRITE, &old)) {
        return err_value("VirtualProtect failed");
    }
    memcpy((void *)(uintptr_t)address, bytes.data(), bytes.size());
    VirtualProtect((LPVOID)(uintptr_t)address, bytes.size(), old, &old);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)(uintptr_t)address, bytes.size());
    json::Value v = ok_value();
    v.set("address", json::Value::make_str("0x" + hex_u64(address)));
    v.set("bytes", json::Value::make_num((double)bytes.size()));
    return v;
}

static json::Value tool_chain(const json::Value &args) {
    uint64_t address = 0;
    std::string error;
    if (!resolve_chain(args, address, error)) return err_value(error);
    std::string kind = args.get_str("type", "u64");
    double count = args.get_num("count", 1);
    json::Value value;
    if (!read_typed(address, kind, value, error, (int)count)) return err_value(error);
    json::Value v = ok_value();
    v.set("address", json::Value::make_str("0x" + hex_u64(address)));
    v.set("type", json::Value::make_str(kind));
    v.set("value", value);
    return v;
}

static json::Value tool_windows() {
    json::Value v = ok_value();
    json::Value arr = json::Value::make_arr();
    struct Ctx { DWORD pid; json::Value *out; } ctx = { GetCurrentProcessId(), &arr };
    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        auto *ctx = (Ctx *)param;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != ctx->pid) return TRUE;
        wchar_t title[512] = {0};
        wchar_t klass[256] = {0};
        GetWindowTextW(hwnd, title, 512);
        GetClassNameW(hwnd, klass, 256);
        json::Value w = json::Value::make_obj();
        w.set("hwnd", json::Value::make_str("0x" + hex_u64((uint64_t)(uintptr_t)hwnd)));
        w.set("class", json::Value::make_str(narrow(klass)));
        w.set("title", json::Value::make_str(narrow(title)));
        ctx->out->push(w);
        return TRUE;
    }, (LPARAM)&ctx);
    v.set("windows", arr);
    return v;
}

static json::Value tool_load_map(const json::Value &args) {
    std::string path = args.get_str("path");
    if (path.empty()) return err_value("path is required");
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return err_value("cannot open " + path);
    LARGE_INTEGER size = {0};
    GetFileSizeEx(f, &size);
    std::string text((size_t)size.QuadPart, '\0');
    DWORD got = 0;
    ReadFile(f, &text[0], (DWORD)text.size(), &got, nullptr);
    CloseHandle(f);
    text.resize(got);
    json::Value parsed;
    std::string error;
    if (!json::parse(text, parsed, error)) return err_value("map is not valid JSON: " + error);
    g_mapPath = path;
    json::Value v = ok_value();
    v.set("path", json::Value::make_str(path));
    v.set("bytes", json::Value::make_num((double)got));
    return v;
}

// Dispatch a tools/call. Tool names are prefixed with the engine id so one
// client can drive every engine without special cases.
static json::Value dispatch(const std::string &tool, const json::Value &args) {
    std::string prefix = tool_prefix();
    if (tool.compare(0, prefix.size(), prefix) != 0) {
        return err_value("tool " + tool + " does not belong to engine " + g_engine.id);
    }
    std::string name = tool.substr(prefix.size());
    if (name == "status") return tool_status();
    if (name == "modules") return tool_modules();
    if (name == "scan") return tool_scan(args);
    if (name == "read") return tool_read(args);
    if (name == "write") return tool_write(args);
    if (name == "chain") return tool_chain(args);
    if (name == "windows") return tool_windows();
    if (name == "load_map") return tool_load_map(args);
    if (name == "unload") return ok_value().set("note", json::Value::make_str("eject with live_eject"));
    return err_value("unknown tool: " + tool);
}

static json::Value tool_list() {
    json::Value arr = json::Value::make_arr();
    // Each tool carries the arguments it actually reads, so a client can discover
    // them. They are read by name and absence is tolerated, so "required" lists
    // only the ones the tool cannot work without.
    struct Entry { const char *suffix; const char *description; const char *schema; };
    static const Entry entries[] = {
        { "status", "Bridge version, engine identity and per-capability levels.", "" },
        { "modules", "Loaded modules with base, size and path.", "" },
        { "scan", "Signature scan inside a module. \"48 8B ?? ??\" wildcards.",
          "{\"type\":\"object\",\"properties\":{"
          "\"pattern\":{\"type\":\"string\",\"description\":\"hex bytes with ?? wildcards, e.g. 48 8B ?? ?? E8\"},"
          "\"module\":{\"type\":\"string\",\"description\":\"module name; omit or * for the host image\"},"
          "\"limit\":{\"type\":\"number\",\"description\":\"max hits, default 64, capped at 512\"}},"
          "\"required\":[\"pattern\"]}" },
        { "read", "Raw bytes at an address or resolved through a pointer chain.",
          "{\"type\":\"object\",\"properties\":{"
          "\"address\":{\"type\":\"string\",\"description\":\"hex address; omit to use module + rva + offsets\"},"
          "\"module\":{\"type\":\"string\"},"
          "\"rva\":{\"type\":\"string\"},"
          "\"offsets\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
          "\"size\":{\"type\":\"number\",\"description\":\"bytes to read, default 64, capped at 65536\"}}}" },
        { "write", "Patch bytes at an address or resolved through a pointer chain.",
          "{\"type\":\"object\",\"properties\":{"
          "\"address\":{\"type\":\"string\"},"
          "\"module\":{\"type\":\"string\"},"
          "\"rva\":{\"type\":\"string\"},"
          "\"offsets\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
          "\"hex\":{\"type\":\"string\",\"description\":\"payload bytes as hex, even length\"}},"
          "\"required\":[\"hex\"]}" },
        { "chain", "Resolve module+RVA through a pointer chain and read a typed value.",
          "{\"type\":\"object\",\"properties\":{"
          "\"module\":{\"type\":\"string\"},"
          "\"rva\":{\"type\":\"string\"},"
          "\"offsets\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},"
          "\"description\":\"p = base; then for each offset: p = *(void**)p + offset\"},"
          "\"address\":{\"type\":\"string\",\"description\":\"used instead of module + rva\"},"
          "\"type\":{\"type\":\"string\",\"description\":\"u8 u16 u32 u64 i8 i16 i32 i64 f32 f64 ptr utf8 utf16 bytes; default u64\"},"
          "\"count\":{\"type\":\"number\",\"description\":\"element count for utf8, utf16 and bytes\"}}}" },
        { "windows", "This process's top-level windows.", "" },
        { "load_map", "Load a note-map JSON that binds the note vocabulary onto chains.",
          "{\"type\":\"object\",\"properties\":{"
          "\"path\":{\"type\":\"string\",\"description\":\"note-map file to read\"}},"
          "\"required\":[\"path\"]}" },
    };
    for (const Entry &e : entries) {
        json::Value t = json::Value::make_obj();
        t.set("name", json::Value::make_str(tool_prefix() + e.suffix));
        t.set("description", json::Value::make_str(e.description));
        json::Value schema;
        if (*e.schema) {
            std::string error;
            if (!json::parse(e.schema, schema, error) || schema.kind != json::Value::Obj) {
                schema = json::Value::make_obj();
            }
        } else {
            schema = json::Value::make_obj();
        }
        if (!schema.find("type")) schema.set("type", json::Value::make_str("object"));
        if (!schema.find("properties")) schema.set("properties", json::Value::make_obj());
        t.set("inputSchema", schema);
        arr.push(t);
    }
    return arr;
}

// ===========================================================================
// 5. diagnostics
// ===========================================================================
static void log_line(const std::string &line) {
    if (g_dir.empty()) return;
    std::string path = g_dir + "\\lhai_bridge.boot.txt";
    HANDLE f = CreateFileA(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st;
    GetSystemTime(&st);
    char stamp[64];
    std::snprintf(stamp, sizeof stamp, "%04u-%02u-%02uT%02u:%02u:%02uZ ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::string text = std::string(stamp) + line + "\r\n";
    DWORD wrote = 0;
    SetFilePointer(f, 0, nullptr, FILE_END);
    WriteFile(f, text.data(), (DWORD)text.size(), &wrote, nullptr);
    CloseHandle(f);
}

static void write_status() {
    if (g_dir.empty()) return;
    json::Value s = json::Value::make_obj();
    json::Value bridge = json::Value::make_obj();
    bridge.set("version", json::Value::make_str(kBridgeVersion));
    bridge.set("protocol", json::Value::make_num(kProtocol));
    bridge.set("builtAgainst", json::Value::make_str(kBuiltAgainst));
    bridge.set("target", json::Value::make_str("x64"));
    s.set("bridge", bridge);
    json::Value app = json::Value::make_obj();
    app.set("engine", json::Value::make_str(g_engine.id));
    app.set("exe", json::Value::make_str(g_engine.exe));
    app.set("version", json::Value::make_str(g_engine.version));
    s.set("app", app);
    json::Value server = json::Value::make_obj();
    server.set("endpoint", json::Value::make_str(g_port ? "http://127.0.0.1:" + std::to_string(g_port) + "/mcp" : ""));
    server.set("port", json::Value::make_num(g_port));
    server.set("pid", json::Value::make_num(GetCurrentProcessId()));
    s.set("server", server);
    s.set("map", json::Value::make_str(g_mapPath));
    json::Value errors = json::Value::make_arr();
    s.set("errors", errors);
    std::string path = g_dir + "\\lhai_bridge.status.json";
    HANDLE f = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    std::string text = json::dump(s, true);
    DWORD wrote = 0;
    WriteFile(f, text.data(), (DWORD)text.size(), &wrote, nullptr);
    CloseHandle(f);
}

// ===========================================================================
// 6. MCP over loopback HTTP
// ===========================================================================
static json::Value rpc_result(const json::Value &id, const json::Value &result) {
    json::Value v = json::Value::make_obj();
    v.set("jsonrpc", json::Value::make_str("2.0"));
    v.set("id", id);
    v.set("result", result);
    return v;
}

static json::Value rpc_error(const json::Value &id, int code, const std::string &message) {
    json::Value v = json::Value::make_obj();
    v.set("jsonrpc", json::Value::make_str("2.0"));
    v.set("id", id);
    json::Value e = json::Value::make_obj();
    e.set("code", json::Value::make_num(code));
    e.set("message", json::Value::make_str(message));
    v.set("error", e);
    return v;
}

static json::Value handle_rpc(const json::Value &request, bool &no_body) {
    no_body = false;
    const json::Value *id = request.find("id");
    std::string method = request.get_str("method");
    const json::Value *params = request.find("params");

    if (!id && method.size() > 14 && method.compare(0, 14, "notifications/") == 0) {
        no_body = true;
        return json::Value::make_null();
    }
    json::Value null_id = json::Value::make_null();
    const json::Value &rid = id ? *id : null_id;

    if (method == "initialize") {
        json::Value result = json::Value::make_obj();
        result.set("protocolVersion", json::Value::make_str("2024-11-05"));
        json::Value caps = json::Value::make_obj();
        caps.set("tools", json::Value::make_obj());
        result.set("capabilities", caps);
        json::Value info = json::Value::make_obj();
        info.set("name", json::Value::make_str("LhBridge"));
        info.set("version", json::Value::make_str(kBridgeVersion));
        info.set("title", json::Value::make_str("in-process bridge for " + g_engine.id));
        result.set("serverInfo", info);
        return rpc_result(rid, result);
    }
    if (method == "ping") return rpc_result(rid, json::Value::make_obj());
    if (method == "tools/list") {
        json::Value result = json::Value::make_obj();
        result.set("tools", tool_list());
        return rpc_result(rid, result);
    }
    if (method == "tools/call") {
        std::string tool = params ? params->get_str("name") : "";
        const json::Value *args = params ? params->find("arguments") : nullptr;
        json::Value empty = json::Value::make_obj();
        json::Value outcome = dispatch(tool, args ? *args : empty);
        json::Value content = json::Value::make_arr();
        json::Value item = json::Value::make_obj();
        item.set("type", json::Value::make_str("text"));
        item.set("text", json::Value::make_str(json::dump(outcome, true)));
        content.push(item);
        json::Value result = json::Value::make_obj();
        result.set("content", content);
        bool is_error = outcome.get_str("ok") == "false" || (outcome.find("ok") && !outcome.find("ok")->as_bool(true));
        result.set("isError", json::Value::make_bool(is_error));
        return rpc_result(rid, result);
    }
    return rpc_error(rid, -32601, "method not found: " + method);
}

static std::string http_reply_body(const std::string &body) {
    json::Value request;
    std::string error;
    if (!json::parse(body, request, error)) {
        return json::dump(rpc_error(json::Value::make_null(), -32700, "parse error: " + error));
    }
    bool no_body = false;
    json::Value reply = handle_rpc(request, no_body);
    if (no_body) return std::string();
    return json::dump(reply);
}

static void serve_connection(SOCKET client) {
    std::string raw;
    char buffer[8192];
    size_t header_end = std::string::npos;
    while (true) {
        int got = recv(client, buffer, (int)sizeof buffer, 0);
        if (got <= 0) break;
        raw.append(buffer, (size_t)got);
        header_end = raw.find("\r\n\r\n");
        if (header_end != std::string::npos) break;
        if (raw.size() > (1u << 20)) break;
    }
    std::string response;
    if (header_end == std::string::npos) {
        response = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    } else {
        std::string headers = raw.substr(0, header_end);
        size_t content_length = 0;
        {
            std::string lower_headers = lower(headers);
            size_t at = lower_headers.find("content-length:");
            if (at != std::string::npos) {
                size_t start = at + 15;
                while (start < lower_headers.size() && lower_headers[start] == ' ') ++start;
                content_length = (size_t)strtoul(lower_headers.c_str() + start, nullptr, 10);
            }
        }
        std::string body = raw.substr(header_end + 4);
        while (body.size() < content_length) {
            int got = recv(client, buffer, (int)sizeof buffer, 0);
            if (got <= 0) break;
            body.append(buffer, (size_t)got);
        }
        if (body.size() > content_length) body.resize(content_length);

        std::string payload = http_reply_body(body);
        if (payload.empty()) {
            response = "HTTP/1.1 202 Accepted\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        } else {
            char head[256];
            std::snprintf(head, sizeof head,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json; charset=utf-8\r\n"
                "Content-Length: %zu\r\n"
                "Connection: close\r\n\r\n",
                payload.size());
            response = std::string(head) + payload;
        }
    }
    size_t sent = 0;
    while (sent < response.size()) {
        int wrote = send(client, response.data() + sent, (int)(response.size() - sent), 0);
        if (wrote <= 0) break;
        sent += (size_t)wrote;
    }
    closesocket(client);
}

struct ClientCtx { SOCKET socket; };

static DWORD WINAPI client_thread(LPVOID param) {
    auto *ctx = (ClientCtx *)param;
    SOCKET client = ctx->socket;
    delete ctx;
    serve_connection(client);
    return 0;
}

// A bound listener has to outlive the bind call, so keep it here.
static SOCKET g_listener = INVALID_SOCKET;

static bool bind_listener() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        log_line("WSAStartup failed");
        return false;
    }
    for (int port = kPortFirst; port <= kPortLast; ++port) {
        SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET) continue;
        BOOL exclusive = TRUE;
        setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&exclusive, sizeof exclusive);
        sockaddr_in address = {0};
        address.sin_family = AF_INET;
        address.sin_port = htons((u_short)port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1 only
        if (bind(listener, (sockaddr *)&address, sizeof address) != 0) { closesocket(listener); continue; }
        if (listen(listener, SOMAXCONN) != 0) { closesocket(listener); continue; }
        g_listener = listener;
        g_port = port;
        log_line("listening http://127.0.0.1:" + std::to_string(port) + "/mcp engine=" + g_engine.id);
        return true;
    }
    log_line("no free port in " + std::to_string(kPortFirst) + ".." + std::to_string(kPortLast));
    return false;
}

static void serve_forever() {
    for (;;) {
        SOCKET client = accept(g_listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            if (WSAGetLastError() == WSAEINTR) continue;
            break;
        }
        auto *ctx = new ClientCtx();
        ctx->socket = client;
        HANDLE thread = CreateThread(nullptr, 0, client_thread, ctx, 0, nullptr);
        if (thread) CloseHandle(thread);
        else { delete ctx; closesocket(client); }
    }
}

static void initialise(const std::string &dir) {
    g_dir = dir;
    g_engine = detect_engine();
    log_line("booting " + std::string(kBridgeVersion) + " pid=" + std::to_string(GetCurrentProcessId()) +
             " exe=" + g_engine.exe + " engine=" + g_engine.id);
    if (!bind_listener()) return;
    write_status();
    serve_forever();
}

// ===========================================================================
// 7. entry points
// ===========================================================================
#ifdef LHAI_BRIDGE_EXE
int main() {
    char exe[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string path = exe;
    size_t slash = path.find_last_of("\\/");
    std::string dir = slash == std::string::npos ? "." : path.substr(0, slash);
    initialise(dir);
    return 0;
}
#else
static DWORD WINAPI boot_thread(LPVOID) {
    initialise(g_dir);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        DisableThreadLibraryCalls(module);
        char self[MAX_PATH] = {0};
        GetModuleFileNameA(module, self, MAX_PATH);
        std::string path = self;
        size_t slash = path.find_last_of("\\/");
        g_dir = slash == std::string::npos ? "." : path.substr(0, slash);
        HANDLE thread = CreateThread(nullptr, 0, boot_thread, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}

// Exports so a caller can confirm identity and read the port without the file.
extern "C" __declspec(dllexport) unsigned long LhaiBridgePort(void) {
    return (unsigned long)g_port;
}

extern "C" __declspec(dllexport) unsigned long LhaiBridgePid(void) {
    return GetCurrentProcessId();
}

// Unload on request, from a fresh thread - a thread may not free the module it
// is currently executing in, which is why this cannot simply call FreeLibrary.
// The client computes this export's remote address (module base + RVA) and runs
// it with CreateRemoteThread, the same way the DLL was loaded in the first place.
static DWORD WINAPI unload_thread(LPVOID) {
    if (g_listener != INVALID_SOCKET) {
        closesocket(g_listener);
        g_listener = INVALID_SOCKET;
    }
    WSACleanup();
    log_line("unloading");
    FreeLibraryAndExitThread(g_self, 0);
    return 0;
}

extern "C" __declspec(dllexport) void LhaiBridgeUnload(void) {
    HANDLE thread = CreateThread(nullptr, 0, unload_thread, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
}
#endif
