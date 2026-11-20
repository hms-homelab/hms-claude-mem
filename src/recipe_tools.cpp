#include "recipe_tools.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace {

// Recipes are expected in the hundreds (SDD-004 section 6); this is a ceiling
// on one scan, not a page size.
constexpr int kScanAll = 100000;

// Hash fields of a recipe, besides created_at/updated_at.
const char* const kListFields[] = {"runs_on", "params", "prereqs", "tags"};

// Lowercase letters and digits in runs joined by single '-'.
bool isKebab(const std::string& s) {
    if (s.empty() || s.front() == '-' || s.back() == '-') return false;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = s[i];
        if (c == '-') {
            if (s[i - 1] == '-') return false;
        } else if (!std::islower(c) && !std::isdigit(c)) {
            return false;
        }
    }
    return true;
}

bool isStringArray(const json& v) {
    if (!v.is_array()) return false;
    for (const auto& e : v)
        if (!e.is_string()) return false;
    return true;
}

bool isNonEmptyString(const json& v) {
    return v.is_string() && !v.get<std::string>().empty();
}

// Validates whichever recipe fields `args` carries. On create every required
// field must be present. Returns an error message, or "" when valid.
std::string validate(const json& args, bool creating) {
    if (creating) {
        for (const char* f : {"name", "description", "language", "script", "runs_on", "mutates"})
            if (!args.contains(f))
                return std::string("Missing required field: ") + f +
                       (std::string(f) == "mutates"
                            ? " (say true or false; it is never assumed)"
                            : "");
    }
    if (args.contains("name") && !(args["name"].is_string() && isKebab(args["name"])))
        return "name must be kebab-case: lowercase letters and digits joined by '-'";
    if (args.contains("new_name") &&
        !(args["new_name"].is_string() && isKebab(args["new_name"])))
        return "new_name must be kebab-case: lowercase letters and digits joined by '-'";
    for (const char* f : {"description", "language", "script"})
        if (args.contains(f) && !isNonEmptyString(args[f]))
            return std::string(f) + " must be a non-empty string";
    if (args.contains("script") &&
        args["script"].get<std::string>().size() > RecipeTools::kMaxScriptBytes)
        return "script is over the 64 KB limit";
    if (args.contains("runs_on") &&
        !(isStringArray(args["runs_on"]) && !args["runs_on"].empty()))
        return "runs_on must be a non-empty list of host labels (mac, hub, vps, windows, docker, any)";
    if (args.contains("mutates") && !args["mutates"].is_boolean())
        return "mutates must be true or false";
    for (const char* f : {"prereqs", "tags"})
        if (args.contains(f) && !isStringArray(args[f]))
            return std::string(f) + " must be a list of strings";
    if (args.contains("params")) {
        const json& p = args["params"];
        if (!p.is_array()) return "params must be a list of {name, description, example}";
        for (const auto& e : p) {
            if (!e.is_object() || !e.contains("name") || !isNonEmptyString(e["name"]))
                return "every param needs a non-empty string name";
            for (const char* f : {"description", "example"})
                if (e.contains(f) && !e[f].is_string())
                    return std::string("param ") + f + " must be a string";
        }
    }
    if (args.contains("verified") && !args["verified"].is_boolean())
        return "verified must be true or false";
    return "";
}

// D3 over every guarded field the arguments carry: script, prereqs, params.
std::optional<std::string> guardSecrets(const json& args) {
    if (args.contains("script"))
        if (auto r = RecipeTools::findSecret("script", args["script"].get<std::string>()))
            return r;
    if (args.contains("prereqs")) {
        const json& p = args["prereqs"];
        for (size_t i = 0; i < p.size(); ++i)
            if (auto r = RecipeTools::findSecret("prereqs[" + std::to_string(i) + "]",
                                                 p[i].get<std::string>()))
                return r;
    }
    if (args.contains("params")) {
        const json& p = args["params"];
        for (size_t i = 0; i < p.size(); ++i)
            for (const char* f : {"name", "description", "example"})
                if (p[i].contains(f))
                    if (auto r = RecipeTools::findSecret(
                            "params[" + std::to_string(i) + "]." + f,
                            p[i][f].get<std::string>()))
                        return r;
    }
    return std::nullopt;
}

// A literal assigned to a secret-sounding name counts as a secret when it is
// long (>= 20) and opaque: no reference ($VAR, ${VAR}, {{param}}), not a path
// or URL, a single run of key-like characters with both letters and digits.
bool looksOpaque(const std::string& v) {
    if (v.size() < 20) return false;
    if (v.find('$') != std::string::npos) return false;
    if (v.find("{{") != std::string::npos) return false;
    if (v.find("://") != std::string::npos) return false;
    if (v[0] == '/' || v[0] == '~' || v[0] == '.') return false;
    bool letter = false, digit = false;
    for (unsigned char c : v) {
        if (std::isalpha(c)) letter = true;
        else if (std::isdigit(c)) digit = true;
        else if (c != '+' && c != '/' && c != '=' && c != '_' && c != '-' && c != '.')
            return false;
    }
    return letter && digit;
}

} // namespace

RecipeTools::RecipeTools(IMemoryStore& store, EmbeddingClient& embedder)
    : store_(store), embedder_(embedder) {}

std::string RecipeTools::now() {
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::ostringstream ss;
    ss << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

std::string RecipeTools::today() {
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::ostringstream ss;
    ss << std::put_time(std::gmtime(&t), "%Y-%m-%d");
    return ss.str();
}

namespace {

// Hand-written linear scans, not std::regex: a 64 KB one-line script made the
// regex version backtrack for minutes.
bool isWordChar(unsigned char c) { return std::isalnum(c) || c == '_'; }
bool isAlnum(unsigned char c) { return std::isalnum(c) != 0; }
bool isUpperOrDigit(unsigned char c) { return std::isupper(c) || std::isdigit(c); }
bool isAlnumUnderscore(unsigned char c) { return std::isalnum(c) || c == '_'; }
bool isAlnumDash(unsigned char c) { return std::isalnum(c) || c == '-'; }
bool isAlnumDashUnderscore(unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; }

struct KeyPrefix {
    const char* label;
    const char* prefix;
    bool (*body)(unsigned char);
    size_t min_body;
    bool aws; // exactly 16 body chars, not followed or preceded by an alnum
};

// Order matters: sk-ant- is checked before the generic sk- of OpenAI keys.
const KeyPrefix kPrefixes[] = {
    {"an AWS access key", "AKIA", isUpperOrDigit, 16, true},
    {"an AWS access key", "ASIA", isUpperOrDigit, 16, true},
    {"a GitHub token", "ghp_", isAlnum, 20, false},
    {"a GitHub token", "gho_", isAlnum, 20, false},
    {"a GitHub token", "ghs_", isAlnum, 20, false},
    {"a GitHub token", "ghu_", isAlnum, 20, false},
    {"a GitHub token", "ghr_", isAlnum, 20, false},
    {"a GitHub token", "github_pat_", isAlnumUnderscore, 20, false},
    {"a Stripe live key", "sk_live_", isAlnum, 10, false},
    {"a Stripe live key", "rk_live_", isAlnum, 10, false},
    {"a Slack token", "xoxa-", isAlnumDash, 10, false},
    {"a Slack token", "xoxb-", isAlnumDash, 10, false},
    {"a Slack token", "xoxo-", isAlnumDash, 10, false},
    {"a Slack token", "xoxp-", isAlnumDash, 10, false},
    {"a Slack token", "xoxr-", isAlnumDash, 10, false},
    {"a Slack token", "xoxs-", isAlnumDash, 10, false},
    {"an Anthropic key", "sk-ant-", isAlnumDashUnderscore, 20, false},
    {"an OpenAI key", "sk-", isAlnumDashUnderscore, 20, false},
    {"a Google API key", "AIza", isAlnumDashUnderscore, 35, false},
};

const char* findKeyPrefix(const std::string& line) {
    for (const auto& kp : kPrefixes) {
        const size_t plen = std::strlen(kp.prefix);
        for (size_t pos = line.find(kp.prefix); pos != std::string::npos;
             pos = line.find(kp.prefix, pos + 1)) {
            if (pos > 0) {
                unsigned char before = line[pos - 1];
                if (kp.aws ? isAlnum(before) : isWordChar(before)) continue;
            }
            size_t n = 0, i = pos + plen;
            while (i < line.size() && kp.body(line[i])) { ++n; ++i; }
            if (kp.aws) {
                if (n == 16 && (i == line.size() || !isAlnum(line[i]))) return kp.label;
            } else if (n >= kp.min_body) {
                return kp.label;
            }
        }
    }
    return nullptr;
}

bool hasPrivateKeyBlock(const std::string& line) {
    static const std::string begin = "-----BEGIN ", end = "PRIVATE KEY-----";
    for (size_t pos = line.find(begin); pos != std::string::npos; pos = line.find(begin, pos + 1)) {
        size_t i = pos + begin.size();
        while (i < line.size() && (isUpperOrDigit(line[i]) || line[i] == ' ')) {
            if (line.compare(i, end.size(), end) == 0) return true;
            ++i;
        }
    }
    return false;
}

bool isNameChar(unsigned char c) { return std::isalnum(c) || c == '_' || c == '.' || c == '-'; }

// A name containing password/secret/token/key (any case), an optional closing
// quote, '=' or ':', an optional opening quote, then a long opaque literal.
bool hasSecretAssignment(const std::string& line) {
    static const char* const kWords[] = {"password", "passwd", "secret", "token", "key"};
    for (size_t op = 0; op < line.size(); ++op) {
        if (line[op] != '=' && line[op] != ':') continue;
        // Left: spaces, optional quote, the name.
        size_t j = op;
        while (j > 0 && std::isspace(static_cast<unsigned char>(line[j - 1]))) --j;
        if (j > 0 && (line[j - 1] == '"' || line[j - 1] == '\'')) --j;
        size_t name_end = j;
        while (j > 0 && isNameChar(line[j - 1])) --j;
        if (j == name_end) continue;
        std::string name = line.substr(j, name_end - j);
        for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        bool named = false;
        for (const char* w : kWords)
            if (name.find(w) != std::string::npos) { named = true; break; }
        if (!named) continue;
        // Right: spaces, optional quote, the value.
        size_t k = op + 1;
        while (k < line.size() && std::isspace(static_cast<unsigned char>(line[k]))) ++k;
        if (k < line.size() && (line[k] == '"' || line[k] == '\'')) ++k;
        size_t vstart = k;
        while (k < line.size()) {
            unsigned char c = line[k];
            if (std::isspace(c) || c == '"' || c == '\'' || c == '`' || c == ',' || c == ';' || c == ')')
                break;
            ++k;
        }
        if (looksOpaque(line.substr(vstart, k - vstart))) return true;
    }
    return false;
}

} // namespace

std::optional<std::string> RecipeTools::findSecret(const std::string& field,
                                                   const std::string& text) {
    std::istringstream in(text);
    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        const char* what = findKeyPrefix(line);
        if (!what && hasPrivateKeyBlock(line)) what = "a private key block";
        if (!what && hasSecretAssignment(line))
            what = "a literal value assigned to a password/secret/token/key name";
        if (what) {
            return field + " line " + std::to_string(lineno) + " looks like it holds " +
                   what + ". Recipes never hold secret values: read it from a secrets "
                   "file by variable name instead (for example `source ~/.secrets` and "
                   "use $SERVICE_TOKEN or ${API_KEY}).";
        }
    }
    return std::nullopt;
}

std::string RecipeTools::embedText(const json& r) {
    std::string s = r.value("language", "") + ": " + r.value("name", "") + ". " +
                    r.value("description", "");
    const json tags = r.value("tags", json::array());
    if (!tags.empty()) {
        s += " [";
        for (size_t i = 0; i < tags.size(); ++i) {
            if (i) s += ", ";
            s += tags[i].get<std::string>();
        }
        s += "]";
    }
    return s;
}

std::optional<json> RecipeTools::load(const std::string& name) {
    auto e = store_.hashGet(name);
    if (!e || !e->fields.count("script")) return std::nullopt;
    const auto& f = e->fields;
    auto str = [&](const char* k) {
        auto it = f.find(k);
        return it == f.end() ? std::string() : it->second;
    };
    json r = {
        {"name", name},
        {"description", str("description")},
        {"language", str("language")},
        {"script", str("script")},
        {"mutates", str("mutates") == "true"},
        {"created_at", e->created_at},
        {"updated_at", e->updated_at},
    };
    for (const char* k : kListFields) {
        json v = json::array();
        try {
            std::string raw = str(k);
            if (!raw.empty()) v = json::parse(raw);
        } catch (...) {}
        r[k] = v;
    }
    std::string va = str("verified_at");
    r["verified_at"] = va.empty() ? json(nullptr) : json(va);
    return r;
}

bool RecipeTools::save(const json& r, const std::vector<float>* embedding) {
    const std::string name = r["name"];
    MemoryEntry e;
    e.key = name;
    e.created_at = r["created_at"];
    e.updated_at = r["updated_at"];
    e.fields["name"] = name;
    e.fields["description"] = r["description"];
    e.fields["language"] = r["language"];
    e.fields["script"] = r["script"];
    e.fields["mutates"] = r["mutates"].get<bool>() ? "true" : "false";
    for (const char* k : kListFields) e.fields[k] = r[k].dump();
    e.fields["verified_at"] = r["verified_at"].is_null() ? "" : r["verified_at"].get<std::string>();

    if (embedding && !store_.vectorAdd(name, *embedding)) return false;
    return store_.hashSet(name, e);
}

json RecipeTools::summary(const json& r) {
    return {{"name", r["name"]},
            {"description", r["description"]},
            {"language", r["language"]},
            {"runs_on", r["runs_on"]},
            {"mutates", r["mutates"]},
            {"verified_at", r["verified_at"]}};
}

json RecipeTools::create(const json& args) {
    if (auto err = validate(args, true); !err.empty()) return {{"error", err}};
    if (args.contains("new_name")) return {{"error", "new_name is for recipe_update"}};
    if (auto s = guardSecrets(args)) return {{"error", "Refused: " + *s}};

    const std::string name = args["name"];
    std::lock_guard<std::mutex> lk(write_mutex_);
    if (load(name)) return {{"error", "Recipe already exists: " + name + " (use recipe_update)"}};

    std::string ts = now();
    json r = {
        {"name", name},
        {"description", args["description"]},
        {"language", args["language"]},
        {"script", args["script"]},
        {"runs_on", args["runs_on"]},
        {"mutates", args["mutates"]},
        {"params", args.value("params", json::array())},
        {"prereqs", args.value("prereqs", json::array())},
        {"tags", args.value("tags", json::array())},
        {"verified_at", nullptr},
        {"created_at", ts},
        {"updated_at", ts},
    };

    std::vector<float> emb;
    try {
        emb = embedder_.embed(embedText(r));
    } catch (const std::exception& e) {
        return {{"error", std::string("Embedding failed: ") + e.what()}};
    }
    if (!save(r, &emb)) {
        store_.vectorRemove(name);
        store_.hashDelete(name);
        return {{"error", "Failed to store recipe"}};
    }
    return {{"status", "created"}, {"name", name}};
}

json RecipeTools::update(const json& args) {
    if (!args.contains("name")) return {{"error", "Missing required field: name"}};
    if (auto err = validate(args, false); !err.empty()) return {{"error", err}};
    if (auto s = guardSecrets(args)) return {{"error", "Refused: " + *s}};

    const std::string name = args["name"];
    std::lock_guard<std::mutex> lk(write_mutex_);
    auto cur = load(name);
    if (!cur) return {{"error", "Recipe not found: " + name + " (use recipe_create)"}};

    json r = *cur;
    bool changed = false;
    for (const char* f : {"description", "language", "script", "runs_on", "mutates",
                          "params", "prereqs", "tags"}) {
        if (args.contains(f)) { r[f] = args[f]; changed = true; }
    }
    std::string new_name = args.value("new_name", name);
    if (new_name != name) {
        if (load(new_name)) return {{"error", "Recipe already exists: " + new_name}};
        r["name"] = new_name;
        changed = true;
    }
    bool verified = args.value("verified", false);
    if (verified) r["verified_at"] = today();
    if (!changed && !verified) return {{"error", "Nothing to update: give at least one field or verified: true"}};
    r["updated_at"] = now();

    // Re-embed only when what the vector stands for moved: name, description
    // or tags (SDD-004 section 5). A script change keeps the vector.
    bool reembed = r["name"] != (*cur)["name"] || r["description"] != (*cur)["description"] ||
                   r["tags"] != (*cur)["tags"];
    std::vector<float> emb;
    if (reembed) {
        try {
            emb = embedder_.embed(embedText(r));
        } catch (const std::exception& e) {
            return {{"error", std::string("Embedding failed: ") + e.what()}};
        }
    }

    if (new_name != name) {
        if (!save(r, &emb)) {
            store_.vectorRemove(new_name);
            store_.hashDelete(new_name);
            return {{"error", "Failed to store recipe"}};
        }
        store_.vectorRemove(name);
        store_.hashDelete(name);
    } else if (!save(r, reembed ? &emb : nullptr)) {
        return {{"error", "Failed to store recipe"}};
    }

    json out = {{"status", "updated"}, {"name", r["name"]}, {"verified_at", r["verified_at"]}};
    if (new_name != name) out["renamed_from"] = name;
    return out;
}

json RecipeTools::remove(const json& args) {
    if (!args.contains("name") || !args["name"].is_string())
        return {{"error", "Missing required field: name"}};
    const std::string name = args["name"];
    std::lock_guard<std::mutex> lk(write_mutex_);
    if (!load(name)) return {{"error", "Recipe not found: " + name}};
    store_.vectorRemove(name);
    store_.hashDelete(name);
    return {{"status", "deleted"}, {"name", name}};
}

json RecipeTools::get(const json& args) {
    if (!args.contains("name") || !args["name"].is_string())
        return {{"error", "Missing required field: name"}};
    const std::string name = args["name"];
    auto r = load(name);
    if (!r) return {{"error", "Recipe not found: " + name}};
    return *r;
}

json RecipeTools::search(const json& args) {
    // With name: exact match, same as recipe_get.
    if (args.contains("name")) return get(args);

    for (const char* f : {"query", "tag", "language", "runs_on"})
        if (args.contains(f) && !args[f].is_string())
            return {{"error", std::string(f) + " must be a string"}};
    if (args.contains("mutates") && !args["mutates"].is_boolean())
        return {{"error", "mutates must be true or false"}};
    if (args.contains("top_k") && !args["top_k"].is_number_integer())
        return {{"error", "top_k must be an integer"}};

    const std::string query = args.value("query", "");
    const std::string tag = args.value("tag", "");
    const std::string language = args.value("language", "");
    const std::string runs_on = args.value("runs_on", "");
    const bool has_mutates = args.contains("mutates");
    const bool mutates = has_mutates && args["mutates"].get<bool>();
    const int top_k = std::max(1, args.value("top_k", 5));
    const bool filtered = !tag.empty() || !language.empty() || !runs_on.empty() || has_mutates;

    auto contains = [](const json& arr, const std::string& v) {
        return std::find(arr.begin(), arr.end(), json(v)) != arr.end();
    };
    auto matches = [&](const json& r) {
        if (!tag.empty() && !contains(r["tags"], tag)) return false;
        if (!language.empty() && r["language"] != language) return false;
        if (!runs_on.empty() && !contains(r["runs_on"], runs_on)) return false;
        if (has_mutates && r["mutates"].get<bool>() != mutates) return false;
        return true;
    };

    json results = json::array();
    if (query.empty()) {
        // Filters only: every recipe matching all of them, by name.
        auto names = store_.scanKeys("*", kScanAll);
        std::sort(names.begin(), names.end());
        for (const auto& n : names) {
            auto r = load(n);
            if (r && matches(*r)) results.push_back(summary(*r));
        }
        return {{"results", results}, {"count", results.size()}};
    }

    // Semantic, then the filters applied to the results. With filters, rank the
    // whole pool so a filter never empties a short top-k.
    std::vector<float> emb;
    try {
        emb = embedder_.embed(query);
    } catch (const std::exception& e) {
        return {{"error", std::string("Embedding failed: ") + e.what()}};
    }
    int fetch = top_k;
    if (filtered)
        fetch = std::max(top_k, static_cast<int>(store_.scanKeys("*", kScanAll).size()));
    for (const auto& [n, score] : store_.vectorSearch(emb, fetch)) {
        if (static_cast<int>(results.size()) >= top_k) break;
        auto r = load(n);
        if (!r || !matches(*r)) continue;
        json s = summary(*r);
        s["similarity"] = score;
        results.push_back(s);
    }
    return {{"results", results}, {"count", results.size()}};
}
