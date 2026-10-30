/* SPDX-License-Identifier: MIT */

#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unistd.h>

namespace openfg::config {

namespace {

    struct Value {
        enum class Type { String, Integer, Boolean, Array } type{Type::String};
        std::string string;
        long long integer{};
        bool boolean{};
        std::vector<Value> array;
    };

    Value makeBoolean(bool value) {
        Value result;
        result.type = Value::Type::Boolean;
        result.boolean = value;
        return result;
    }

    Value makeString(std::string value) {
        Value result;
        result.type = Value::Type::String;
        result.string = std::move(value);
        return result;
    }

    Value makeInteger(long long value) {
        Value result;
        result.type = Value::Type::Integer;
        result.integer = value;
        return result;
    }

    std::string_view trim(std::string_view text) {
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
            text.remove_prefix(1);
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
            text.remove_suffix(1);
        return text;
    }

    // Removes a trailing comment, taking quoted strings into account.
    std::string stripComment(const std::string& line) {
        bool basic{false};
        bool literal{false};
        bool escaped{false};
        for (size_t i = 0; i < line.size(); i++) {
            const char c = line[i];
            if (basic) {
                if (escaped)
                    escaped = false;
                else if (c == '\\')
                    escaped = true;
                else if (c == '"')
                    basic = false;
                continue;
            }
            if (literal) {
                if (c == '\'')
                    literal = false;
                continue;
            }
            if (c == '"')
                basic = true;
            else if (c == '\'')
                literal = true;
            else if (c == '#')
                return line.substr(0, i);
        }
        return line;
    }

    class Parser {
    public:
        Parser(std::string_view line, std::filesystem::path path, unsigned lineNumber)
            : m_line(line), m_path(std::move(path)), m_lineNumber(lineNumber) {}

        Value parse() {
            this->skipSpace();
            Value value = this->parseValue();
            this->skipSpace();
            if (this->m_pos != this->m_line.size())
                this->fail("unexpected trailing characters");
            return value;
        }

        [[noreturn]] void fail(const std::string& message) const {
            throw std::runtime_error(
                this->m_path.string() + ":" + std::to_string(this->m_lineNumber) +
                ": " + message);
        }

    private:
        void skipSpace() {
            while (this->m_pos < this->m_line.size() &&
                   std::isspace(static_cast<unsigned char>(this->m_line[this->m_pos])))
                this->m_pos++;
        }

        Value parseValue() {
            if (this->m_pos >= this->m_line.size())
                this->fail("missing value");

            const char c = this->m_line[this->m_pos];
            if (c == '"')
                return this->parseBasicString();
            if (c == '\'')
                return this->parseLiteralString();
            if (c == '[')
                return this->parseArray();
            if (this->m_line.compare(this->m_pos, 4, "true") == 0) {
                this->m_pos += 4;
                return makeBoolean(true);
            }
            if (this->m_line.compare(this->m_pos, 5, "false") == 0) {
                this->m_pos += 5;
                return makeBoolean(false);
            }
            return this->parseInteger();
        }

        Value parseBasicString() {
            this->m_pos++; // opening quote
            std::string result;
            while (this->m_pos < this->m_line.size()) {
                const char c = this->m_line[this->m_pos++];
                if (c == '"')
                    return makeString(std::move(result));
                if (c != '\\') {
                    result.push_back(c);
                    continue;
                }
                if (this->m_pos >= this->m_line.size())
                    this->fail("unterminated escape sequence");
                const char escape = this->m_line[this->m_pos++];
                switch (escape) {
                    case '"': result.push_back('"'); break;
                    case '\\': result.push_back('\\'); break;
                    case 'n': result.push_back('\n'); break;
                    case 't': result.push_back('\t'); break;
                    case 'r': result.push_back('\r'); break;
                    case 'b': result.push_back('\b'); break;
                    case 'f': result.push_back('\f'); break;
                    default:
                        this->fail(std::string("unsupported escape '\\") + escape + "'");
                }
            }
            this->fail("unterminated string");
        }

        Value parseLiteralString() {
            this->m_pos++; // opening quote
            const size_t end = this->m_line.find('\'', this->m_pos);
            if (end == std::string_view::npos)
                this->fail("unterminated literal string");
            const std::string result(this->m_line.substr(this->m_pos, end - this->m_pos));
            this->m_pos = end + 1;
            return makeString(result);
        }

        Value parseArray() {
            this->m_pos++; // opening bracket
            Value result;
            result.type = Value::Type::Array;
            this->skipSpace();
            if (this->m_pos < this->m_line.size() && this->m_line[this->m_pos] == ']') {
                this->m_pos++;
                return result;
            }
            while (true) {
                this->skipSpace();
                result.array.push_back(this->parseValue());
                this->skipSpace();
                if (this->m_pos >= this->m_line.size())
                    this->fail("unterminated array");
                const char c = this->m_line[this->m_pos++];
                if (c == ']')
                    return result;
                if (c != ',')
                    this->fail("expected ',' or ']' in array");
                this->skipSpace();
                if (this->m_pos < this->m_line.size() && this->m_line[this->m_pos] == ']') {
                    this->m_pos++;
                    return result;
                }
            }
        }

        Value parseInteger() {
            const size_t start = this->m_pos;
            if (this->m_pos < this->m_line.size() && this->m_line[this->m_pos] == '-')
                this->m_pos++;
            while (this->m_pos < this->m_line.size() &&
                   std::isdigit(static_cast<unsigned char>(this->m_line[this->m_pos])))
                this->m_pos++;
            if (start == this->m_pos)
                this->fail("expected a string, number, boolean or array");
            if (this->m_pos < this->m_line.size() &&
                (this->m_line[this->m_pos] == '.' || this->m_line[this->m_pos] == 'e' ||
                 this->m_line[this->m_pos] == 'E'))
                this->fail("floating-point values are not supported");

            const std::string text(this->m_line.substr(start, this->m_pos - start));
            try {
                return makeInteger(std::stoll(text));
            } catch (...) {
                this->fail("invalid integer '" + text + "'");
            }
        }

        std::string_view m_line;
        std::filesystem::path m_path;
        unsigned m_lineNumber{};
        size_t m_pos{};
    };

    const Value* findValue(const std::vector<std::pair<std::string, Value>>& values,
        const std::string& section, const std::string& key) {
        for (const auto& [fullKey, value] : values) {
            if (fullKey == section + "." + key)
                return &value;
        }
        return nullptr;
    }

    bool getBool(const std::vector<std::pair<std::string, Value>>& values,
        const std::string& section, const std::string& key, bool& result,
        std::string& error) {
        const Value* value = findValue(values, section, key);
        if (!value)
            return true;
        if (value->type != Value::Type::Boolean) {
            error = "[" + section + "] " + key + " must be a boolean";
            return false;
        }
        result = value->boolean;
        return true;
    }

    bool getString(const std::vector<std::pair<std::string, Value>>& values,
        const std::string& section, const std::string& key, std::string& result,
        std::string& error) {
        const Value* value = findValue(values, section, key);
        if (!value)
            return true;
        if (value->type != Value::Type::String) {
            error = "[" + section + "] " + key + " must be a string";
            return false;
        }
        result = value->string;
        return true;
    }

    bool getUnsigned(const std::vector<std::pair<std::string, Value>>& values,
        const std::string& section, const std::string& key, unsigned& result,
        std::string& error) {
        const Value* value = findValue(values, section, key);
        if (!value)
            return true;
        if (value->type != Value::Type::Integer || value->integer < 0) {
            error = "[" + section + "] " + key + " must be a non-negative integer";
            return false;
        }
        result = static_cast<unsigned>(value->integer);
        return true;
    }

    bool getStringArray(const std::vector<std::pair<std::string, Value>>& values,
        const std::string& section, const std::string& key,
        std::vector<std::string>& result, std::string& error) {
        const Value* value = findValue(values, section, key);
        if (!value)
            return true;
        if (value->type != Value::Type::Array) {
            error = "[" + section + "] " + key + " must be an array of strings";
            return false;
        }
        result.clear();
        for (const Value& item : value->array) {
            if (item.type != Value::Type::String) {
                error = "[" + section + "] " + key + " must contain only strings";
                return false;
            }
            result.push_back(item.string);
        }
        return true;
    }

    std::optional<bool> envBool(const char* name) {
        const char* text = std::getenv(name);
        if (!text || !*text)
            return std::nullopt;
        std::string value(text);
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (value == "0" || value == "false" || value == "off" || value == "no")
            return false;
        return true;
    }

    std::optional<long long> envInt(const char* name) {
        const char* text = std::getenv(name);
        if (!text || !*text)
            return std::nullopt;
        char* end{};
        const long long value = std::strtoll(text, &end, 10);
        if (!end || *end != '\0')
            return std::nullopt;
        return value;
    }

    bool patternMatch(std::string_view pattern, std::string_view text) {
        // Iterative wildcard match with '*' and '?', case-insensitive.
        size_t p{0};
        size_t t{0};
        size_t star{std::string_view::npos};
        size_t retry{0};
        auto equal = [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                std::tolower(static_cast<unsigned char>(b));
        };
        while (t < text.size()) {
            if (p < pattern.size() && (pattern[p] == '?' || equal(pattern[p], text[t]))) {
                p++;
                t++;
            } else if (p < pattern.size() && pattern[p] == '*') {
                star = p++;
                retry = t;
            } else if (star != std::string_view::npos) {
                p = star + 1;
                t = ++retry;
            } else {
                return false;
            }
        }
        while (p < pattern.size() && pattern[p] == '*')
            p++;
        return p == pattern.size();
    }

    bool listMatches(const std::vector<std::string>& patterns,
        const std::vector<std::string>& names) {
        for (const std::string& pattern : patterns) {
            // Patterns copied from Windows paths often contain backslashes.
            // Process names are basenames, so also try the final component.
            const size_t separator = pattern.find_last_of("/\\");
            const std::string base = separator == std::string::npos
                ? pattern
                : pattern.substr(separator + 1);
            for (const std::string& name : names) {
                if (patternMatch(pattern, name) || patternMatch(base, name))
                    return true;
            }
        }
        return false;
    }

    std::string basename(std::string_view path) {
        const size_t slash = path.find_last_of('/');
        return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
    }

    bool endsWithExe(std::string_view text) {
        if (text.size() < 4)
            return false;
        std::string suffix(text.substr(text.size() - 4));
        std::transform(suffix.begin(), suffix.end(), suffix.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return suffix == ".exe";
    }

    void addName(std::vector<std::string>& names, const std::string& name) {
        if (name.empty())
            return;
        for (const std::string& existing : names) {
            if (patternMatch(name, existing) && patternMatch(existing, name))
                return;
        }
        names.push_back(name);
    }

    std::string readFile(const std::filesystem::path& path) {
        std::ifstream file(path);
        if (!file.is_open())
            throw std::runtime_error(path.string() + ": cannot open");
        std::ostringstream contents;
        contents << file.rdbuf();
        return contents.str();
    }

}

std::filesystem::path defaultPath() {
    if (const char* config = std::getenv("OPENFG_CONFIG"); config && *config)
        return config;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / "openfg" / "conf.toml";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".config" / "openfg" / "conf.toml";
    return "/etc/openfg/conf.toml";
}

bool validate(const Config& config, std::string& error) {
    if (config.multiplier < 2 || config.multiplier > 4) {
        error = "openfg.multiplier must be 2, 3 or 4";
        return false;
    }
    if (config.gpu.empty()) {
        error = "openfg.gpu must not be empty (use \"auto\" for the default device)";
        return false;
    }
    if (config.gpu.find_first_of(" \t\r\n") != std::string::npos) {
        error = "openfg.gpu must not contain whitespace";
        return false;
    }
    for (const auto& pattern : config.include) {
        if (pattern.empty()) {
            error = "processes.include contains an empty pattern";
            return false;
        }
    }
    for (const auto& pattern : config.exclude) {
        if (pattern.empty()) {
            error = "processes.exclude contains an empty pattern";
            return false;
        }
    }
    return true;
}

LoadResult parse(const std::string& text, const std::filesystem::path& path) {
    LoadResult result;
    result.path = path;
    result.found = true;

    std::vector<std::pair<std::string, Value>> values;
    std::set<std::string> seen;
    std::string section;

    try {
        std::istringstream stream(text);
        std::string raw;
        unsigned lineNumber{0};
        while (std::getline(stream, raw)) {
            lineNumber++;
            if (!raw.empty() && raw.back() == '\r')
                raw.pop_back();
            std::string line = stripComment(raw);
            std::string_view view = trim(line);
            if (view.empty())
                continue;

            if (view.front() == '[') {
                if (view.back() != ']')
                    throw std::runtime_error(
                        path.string() + ":" + std::to_string(lineNumber) +
                        ": unterminated section header");
                section = std::string(trim(view.substr(1, view.size() - 2)));
                if (section.empty())
                    throw std::runtime_error(
                        path.string() + ":" + std::to_string(lineNumber) +
                        ": empty section name");
                if (section != "openfg" && section != "processes")
                    throw std::runtime_error(
                        path.string() + ":" + std::to_string(lineNumber) +
                        ": unknown section [" + section + "]");
                continue;
            }

            const size_t equals = view.find('=');
            if (equals == std::string_view::npos)
                throw std::runtime_error(
                    path.string() + ":" + std::to_string(lineNumber) +
                    ": expected 'key = value'");
            const std::string key(trim(view.substr(0, equals)));
            if (key.empty())
                throw std::runtime_error(
                    path.string() + ":" + std::to_string(lineNumber) + ": empty key");

            std::string valueText(trim(view.substr(equals + 1)));
            // Allow arrays to continue on following lines until the bracket
            // closes. Strings cannot span lines in this subset.
            while (!valueText.empty() && valueText.front() == '[') {
                int depth{0};
                bool inString{false};
                bool escaped{false};
                char quote{};
                for (char c : valueText) {
                    if (inString) {
                        if (escaped)
                            escaped = false;
                        else if (c == '\\') {
                            escaped = true;
                        } else if (c == quote) {
                            inString = false;
                        }
                    } else if (c == '"' || c == '\'') {
                        inString = true;
                        quote = c;
                    } else if (c == '[') {
                        depth++;
                    } else if (c == ']') {
                        depth--;
                    }
                }
                if (depth <= 0)
                    break;
                std::string more;
                if (!std::getline(stream, more)) {
                    throw std::runtime_error(
                        path.string() + ":" + std::to_string(lineNumber) +
                        ": unterminated array");
                }
                lineNumber++;
                if (!more.empty() && more.back() == '\r')
                    more.pop_back();
                valueText += " " + std::string(trim(stripComment(more)));
            }

            Parser parser(valueText, path, lineNumber);
            Value value = parser.parse();

            const std::string fullKey =
                (section.empty() ? std::string{} : section + ".") + key;
            if (seen.count(fullKey))
                throw std::runtime_error(
                    path.string() + ":" + std::to_string(lineNumber) +
                    ": duplicate key '" + fullKey + "'");
            seen.insert(fullKey);
            values.emplace_back(fullKey, std::move(value));
        }

        // Reject unknown keys, so typos are reported instead of silently
        // ignored. This also catches keys outside any section.
        static const std::set<std::string> known{
            "openfg.enabled", "openfg.gpu", "openfg.cross_gpu",
            "openfg.multiplier", "openfg.debug",
            "processes.include", "processes.exclude"
        };
        for (const auto& [key, value] : values) {
            (void)value;
            if (!known.count(key))
                throw std::runtime_error(path.string() + ": unknown key '" + key + "'");
        }

        std::string error;
        Config& config = result.config;
        if (!getBool(values, "openfg", "enabled", config.enabled, error) ||
            !getString(values, "openfg", "gpu", config.gpu, error) ||
            !getBool(values, "openfg", "cross_gpu", config.crossGpu, error) ||
            !getUnsigned(values, "openfg", "multiplier", config.multiplier, error) ||
            !getBool(values, "openfg", "debug", config.debug, error) ||
            !getStringArray(values, "processes", "include", config.include, error) ||
            !getStringArray(values, "processes", "exclude", config.exclude, error))
            throw std::runtime_error(path.string() + ": " + error);

        if (!validate(config, error))
            throw std::runtime_error(path.string() + ": " + error);
    } catch (const std::exception& exception) {
        result.error = exception.what();
        result.config = Config{};
    }

    return result;
}

LoadResult load(const std::filesystem::path& path) {
    const std::filesystem::path resolved = path.empty() ? defaultPath() : path;
    LoadResult result;

    std::error_code ec;
    if (!std::filesystem::exists(resolved, ec)) {
        result.path = resolved;
        result.found = false;
    } else {
        try {
            result = parse(readFile(resolved), resolved);
        } catch (const std::exception& exception) {
            result.path = resolved;
            result.found = true;
            result.error = exception.what();
            result.config = Config{};
        }
    }

    // Environment overrides apply even without a config file, which keeps the
    // old ad-hoc workflow working for testing.
    Config& config = result.config;
    if (const auto value = envBool("OPENFG_ENABLE"); value)
        config.enabled = *value;
    if (const char* gpu = std::getenv("OPENFG_GPU"); gpu && *gpu)
        config.gpu = gpu;
    if (const auto value = envBool("OPENFG_CROSS_GPU"); value)
        config.crossGpu = *value;
    if (const auto value = envInt("OPENFG_MULTIPLIER"); value) {
        if (*value >= 2 && *value <= 4)
            config.multiplier = static_cast<unsigned>(*value);
        else
            result.warnings.push_back("OPENFG_MULTIPLIER must be 2, 3 or 4; ignored");
    }
    if (const auto value = envBool("OPENFG_DEBUG"); value)
        config.debug = *value;

    if (result.error.empty()) {
        if (config.crossGpu && config.gpu == "auto")
            result.warnings.push_back(
                "cross_gpu needs an explicit gpu (for example 0000:07:00.0); "
                "\"auto\" selects the application's GPU");
    }

    if (result.error.empty()) {
        std::string error;
        if (!validate(config, error))
            result.error = resolved.string() + ": " + error;
    }

    return result;
}

const LoadResult& processConfig() {
    static const LoadResult result = load();
    return result;
}

std::vector<std::string> processNames() {
    std::vector<std::string> names;

    char exe[4096]{};
    const ssize_t length = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (length > 0)
        addName(names, basename(std::string_view(exe, static_cast<size_t>(length))));

    if (std::ifstream cmdline{"/proc/self/cmdline"}) {
        std::string arg;
        bool first{true};
        while (std::getline(cmdline, arg, '\0')) {
            if (arg.empty())
                continue;
            if (first) {
                addName(names, basename(arg));
                first = false;
            }
            if (endsWithExe(arg))
                addName(names, basename(arg));
        }
    }

    // Wine/Proton maps the game executable; that name is usually what users
    // put in the include list.
    if (std::ifstream maps{"/proc/self/maps"}) {
        std::string line;
        while (std::getline(maps, line)) {
            std::istringstream fields(line);
            std::string address;
            std::string permissions;
            std::string offset;
            std::string device;
            std::string inode;
            if (!(fields >> address >> permissions >> offset >> device >> inode))
                continue;
            std::string path;
            std::getline(fields, path);
            path = std::string{trim(path)};
            if (endsWithExe(path))
                addName(names, basename(path));
        }
    }

    return names;
}

bool processMatches(const Config& config) {
    const std::vector<std::string> names = processNames();
    if (!listMatches(config.exclude, names))
        return config.include.empty() || listMatches(config.include, names);
    return false;
}

bool matchesName(const Config& config, const std::string& name) {
    const std::vector<std::string> names{ name };
    if (!listMatches(config.exclude, names))
        return config.include.empty() || listMatches(config.include, names);
    return false;
}

std::string defaultFile() {
    return R"(# openfg configuration
#
# The layer is inactive unless enabled = true, or OPENFG_ENABLE=1 is set.
# The file is read once per process, early in device startup.

[openfg]
enabled = false

# Vulkan device used for interpolation: "auto", a PCI address such as
# "0000:07:00.0", a device name, or "vendor:device".
gpu = "auto"

# true: run the backend on the GPU above and share frames with the display
# GPU over DMA-BUF. false: run on the display GPU with OpaqueFd.
cross_gpu = false

# Generated frames per real frame: 2, 3 or 4.
multiplier = 2

# Log generated frames and present results to stderr.
debug = false

[processes]
# Enable only for these process names. Empty means all processes.
# '*' and '?' wildcards are supported; matching is case-insensitive.
include = []

# Always keep frame generation off for these processes.
exclude = []
)";
}

std::string toToml(const Config& config) {
    auto quote = [](const std::string& value) {
        std::string result{"\""};
        for (char c : value) {
            switch (c) {
                case '"': result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\n': result += "\\n"; break;
                case '\t': result += "\\t"; break;
                case '\r': result += "\\r"; break;
                default: result.push_back(c);
            }
        }
        result += '"';
        return result;
    };
    auto list = [&](const std::vector<std::string>& values) {
        std::string result{"["};
        for (size_t i = 0; i < values.size(); i++) {
            if (i)
                result += ", ";
            result += quote(values[i]);
        }
        result += ']';
        return result;
    };

    std::string result;
    result += "[openfg]\n";
    result += std::string("enabled = ") + (config.enabled ? "true" : "false") + "\n";
    result += "gpu = " + quote(config.gpu) + "\n";
    result += std::string("cross_gpu = ") + (config.crossGpu ? "true" : "false") + "\n";
    result += "multiplier = " + std::to_string(config.multiplier) + "\n";
    result += std::string("debug = ") + (config.debug ? "true" : "false") + "\n\n";
    result += "[processes]\n";
    result += "include = " + list(config.include) + "\n";
    result += "exclude = " + list(config.exclude) + "\n";
    return result;
}

}
