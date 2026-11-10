// A JSON reader that never holds a whole message: bytes go in as they come
// off the websocket, in pieces of any size, and it tells a Handler what it
// meets. Each event comes with the path to it, keys joined by '.' and "[]"
// for an array's element: in {"d":{"guilds":[{"id":"1"}]}} the id is
// "d.guilds[].id". Strings are kept up to JSON_STRING_MAX bytes (the rest is
// dropped, `cut` says so); escapes come out as UTF-8.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define JSON_STRING_MAX 128
#define JSON_KEY_MAX 32
#define JSON_PATH_MAX 192
#define JSON_DEPTH_MAX 32

enum JsonType { Json_String, Json_Number, Json_True, Json_False, Json_Null };

class JsonHandler {
public:
    virtual ~JsonHandler() {}
    virtual void begin(const char* path, bool array) {}
    virtual void value(const char* path, JsonType type, const char* s, size_t n, bool cut) {}
    virtual void end(const char* path, bool array) {}
};

class Json {
public:
    void reset(JsonHandler* h);
    // false: it isn't JSON (or it's nested deeper than JSON_DEPTH_MAX); stop feeding it
    bool feed(const char* p, size_t n);
    bool done() const { return state_ == S_DONE; }

private:
    enum State : uint8_t {
        S_VALUE, S_ARRAY_FIRST, S_OBJECT_FIRST, S_NEXT_KEY, S_KEY, S_COLON, S_AFTER, S_STRING, S_NUMBER, S_LITERAL,
        S_DONE, S_ERROR
    };
    struct Frame {
        bool array;
        uint16_t path_len;  // the path's length at this container itself
    };

    bool step(char c);
    bool startValue(char c);
    bool afterValue();
    void enterElement();          // the path to the next value in the array on top
    void enterKey();              // the path to the value of the key just read
    bool stringChar(char c, char* buf, uint16_t cap, uint16_t* len, bool* cut);
    void putUtf8(uint32_t cp, char* buf, uint16_t cap, uint16_t* len, bool* cut);
    void emit(JsonType t, const char* s, size_t n, bool cut);

    JsonHandler* h_ = nullptr;
    State state_ = S_VALUE;
    uint8_t esc_ = 0;      // 0: none; 1: after '\'; 2..5: \u hex digits read
    uint32_t u_ = 0;       // the \u being read
    uint32_t high_ = 0;    // a high surrogate waiting for its low half
    Frame stack_[JSON_DEPTH_MAX];
    uint8_t depth_ = 0;
    char path_[JSON_PATH_MAX + 1];
    uint16_t path_len_ = 0;
    char key_[JSON_KEY_MAX + 1];
    uint16_t key_len_ = 0;
    bool key_cut_ = false;
    char str_[JSON_STRING_MAX + 4];
    uint16_t str_len_ = 0;
    bool str_cut_ = false;
};
