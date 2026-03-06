#include "redis_client.h"
#include <hiredis/hiredis.h>
#include <sstream>
#include <cstring>

RedisClient::RedisClient(const std::string& host, int port, const std::string& ns)
    : host_(host), port_(port), namespace_(ns), ctx_(nullptr) {}

RedisClient::~RedisClient() {
    if (ctx_) {
        redisFree(static_cast<redisContext*>(ctx_));
    }
}

std::string RedisClient::vectorKey() const {
    return "claude:mem:" + namespace_ + ":vectors";
}

std::string RedisClient::dataKey(const std::string& key) const {
    return "claude:mem:" + namespace_ + ":data:" + key;
}

std::string RedisClient::dataPrefix() const {
    return "claude:mem:" + namespace_ + ":data:";
}

bool RedisClient::connect() {
    if (ctx_) {
        redisFree(static_cast<redisContext*>(ctx_));
        ctx_ = nullptr;
    }
    auto* c = redisConnect(host_.c_str(), port_);
    if (!c || c->err) {
        if (c) redisFree(c);
        return false;
    }
    ctx_ = c;
    return true;
}

bool RedisClient::isConnected() const {
    return ctx_ != nullptr;
}

bool RedisClient::vectorAdd(const std::string& key, const std::vector<float>& embedding) {
    auto* c = static_cast<redisContext*>(ctx_);
    if (!c) return false;

    std::string vkey = vectorKey();
    int dim = static_cast<int>(embedding.size());
    int argc = dim + 5;
    std::vector<const char*> argv(argc);
    std::vector<size_t> argvlen(argc);
    std::vector<std::string> str_args;
    str_args.reserve(argc);

    str_args.push_back("VADD");
    str_args.push_back(vkey);
    str_args.push_back("VALUES");
    str_args.push_back(std::to_string(dim));
    for (float v : embedding) {
        str_args.push_back(std::to_string(v));
    }
    str_args.push_back(key);

    for (int i = 0; i < argc; i++) {
        argv[i] = str_args[i].c_str();
        argvlen[i] = str_args[i].size();
    }

    auto* reply = static_cast<redisReply*>(
        redisCommandArgv(c, argc, argv.data(), argvlen.data()));
    if (!reply) return false;

    bool ok = (reply->type != REDIS_REPLY_ERROR);
    freeReplyObject(reply);
    return ok;
}

bool RedisClient::vectorRemove(const std::string& key) {
    auto* c = static_cast<redisContext*>(ctx_);
    if (!c) return false;

    std::string vkey = vectorKey();
    auto* reply = static_cast<redisReply*>(
        redisCommand(c, "VREM %s %s", vkey.c_str(), key.c_str()));
    if (!reply) return false;

    bool ok = (reply->type != REDIS_REPLY_ERROR);
    freeReplyObject(reply);
    return ok;
}

std::vector<std::pair<std::string, double>> RedisClient::vectorSearch(
    const std::vector<float>& query, int top_k) {

    auto* c = static_cast<redisContext*>(ctx_);
    std::vector<std::pair<std::string, double>> results;
    if (!c) return results;

    std::string vkey = vectorKey();
    int dim = static_cast<int>(query.size());
    int argc = dim + 7;
    std::vector<const char*> argv(argc);
    std::vector<size_t> argvlen(argc);
    std::vector<std::string> str_args;
    str_args.reserve(argc);

    str_args.push_back("VSIM");
    str_args.push_back(vkey);
    str_args.push_back("VALUES");
    str_args.push_back(std::to_string(dim));
    for (float v : query) {
        str_args.push_back(std::to_string(v));
    }
    str_args.push_back("COUNT");
    str_args.push_back(std::to_string(top_k));
    str_args.push_back("WITHSCORES");

    for (int i = 0; i < argc; i++) {
        argv[i] = str_args[i].c_str();
        argvlen[i] = str_args[i].size();
    }

    auto* reply = static_cast<redisReply*>(
        redisCommandArgv(c, argc, argv.data(), argvlen.data()));
    if (!reply || reply->type != REDIS_REPLY_ARRAY) {
        if (reply) freeReplyObject(reply);
        return results;
    }

    for (size_t i = 0; i + 1 < reply->elements; i += 2) {
        std::string k(reply->element[i]->str, reply->element[i]->len);
        double score = std::stod(
            std::string(reply->element[i + 1]->str, reply->element[i + 1]->len));
        results.push_back({k, score});
    }

    freeReplyObject(reply);
    return results;
}

bool RedisClient::hashSet(const std::string& key, const MemoryEntry& entry) {
    auto* c = static_cast<redisContext*>(ctx_);
    if (!c) return false;

    std::string hkey = dataKey(key);
    std::string pinned_str = entry.pinned ? "1" : "0";
    auto* reply = static_cast<redisReply*>(
        redisCommand(c, "HSET %s value %s category %s created_at %s updated_at %s pinned %s",
                     hkey.c_str(), entry.value.c_str(), entry.category.c_str(),
                     entry.created_at.c_str(), entry.updated_at.c_str(),
                     pinned_str.c_str()));
    if (!reply) return false;

    bool ok = (reply->type != REDIS_REPLY_ERROR);
    freeReplyObject(reply);
    return ok;
}

std::optional<MemoryEntry> RedisClient::hashGet(const std::string& key) {
    auto* c = static_cast<redisContext*>(ctx_);
    if (!c) return std::nullopt;

    std::string hkey = dataKey(key);
    auto* reply = static_cast<redisReply*>(
        redisCommand(c, "HGETALL %s", hkey.c_str()));
    if (!reply || reply->type != REDIS_REPLY_ARRAY || reply->elements == 0) {
        if (reply) freeReplyObject(reply);
        return std::nullopt;
    }

    MemoryEntry entry;
    entry.key = key;
    for (size_t i = 0; i + 1 < reply->elements; i += 2) {
        std::string field(reply->element[i]->str, reply->element[i]->len);
        std::string val(reply->element[i + 1]->str, reply->element[i + 1]->len);
        if (field == "value") entry.value = val;
        else if (field == "category") entry.category = val;
        else if (field == "created_at") entry.created_at = val;
        else if (field == "updated_at") entry.updated_at = val;
        else if (field == "pinned") entry.pinned = (val == "1");
    }

    freeReplyObject(reply);
    return entry;
}

bool RedisClient::hashDelete(const std::string& key) {
    auto* c = static_cast<redisContext*>(ctx_);
    if (!c) return false;

    std::string hkey = dataKey(key);
    auto* reply = static_cast<redisReply*>(
        redisCommand(c, "DEL %s", hkey.c_str()));
    if (!reply) return false;

    bool ok = (reply->type != REDIS_REPLY_ERROR);
    freeReplyObject(reply);
    return ok;
}

std::vector<std::string> RedisClient::scanKeys(const std::string& pattern, int count) {
    auto* c = static_cast<redisContext*>(ctx_);
    std::vector<std::string> keys;
    if (!c) return keys;

    std::string cursor = "0";
    std::string prefix = dataPrefix();
    std::string full_pattern = prefix + pattern;

    do {
        auto* reply = static_cast<redisReply*>(
            redisCommand(c, "SCAN %s MATCH %s COUNT %d",
                         cursor.c_str(), full_pattern.c_str(), count));
        if (!reply || reply->type != REDIS_REPLY_ARRAY || reply->elements != 2) {
            if (reply) freeReplyObject(reply);
            break;
        }

        cursor = std::string(reply->element[0]->str, reply->element[0]->len);

        auto* arr = reply->element[1];
        for (size_t i = 0; i < arr->elements; i++) {
            std::string k(arr->element[i]->str, arr->element[i]->len);
            if (k.size() > prefix.size()) {
                keys.push_back(k.substr(prefix.size()));
            }
        }
        freeReplyObject(reply);
    } while (cursor != "0" && static_cast<int>(keys.size()) < count);

    return keys;
}
