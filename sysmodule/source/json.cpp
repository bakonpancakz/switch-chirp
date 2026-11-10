#include "json.h"

#include <string.h>

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool isNumberChar(char c) { return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E'; }

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

void Json::reset(JsonHandler* h) {
    h_ = h;
    state_ = S_VALUE;
    esc_ = 0;
    u_ = high_ = 0;
    depth_ = 0;
    path_len_ = 0;
    path_[0] = 0;
    key_len_ = str_len_ = 0;
    key_cut_ = str_cut_ = false;
}

bool Json::feed(const char* p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (!step(p[i])) {
            state_ = S_ERROR;
            return false;
        }
    }
    return true;
}

void Json::emit(JsonType t, const char* s, size_t n, bool cut) {
    if (h_) h_->value(path_, t, s, n, cut);
}

// path_ = the container on top's path + what's given (cut, ending in '#', when it doesn't fit)
static uint16_t appendPath(char* path, uint16_t at, const char* add, uint16_t n, bool cut) {
    uint16_t room = JSON_PATH_MAX - at;
    if (n > room || (cut && n == room)) {
        memcpy(path + at, add, room);
        at = JSON_PATH_MAX;
        path[at - 1] = '#';
    } else {
        memcpy(path + at, add, n);
        at += n;
        if (cut) path[at++] = '#';
    }
    path[at] = 0;
    return at;
}

void Json::enterElement() {
    uint16_t at = stack_[depth_ - 1].path_len;
    path_len_ = appendPath(path_, at, "[]", 2, false);
}

void Json::enterKey() {
    uint16_t at = stack_[depth_ - 1].path_len;
    if (at > 0) at = appendPath(path_, at, ".", 1, false);
    path_len_ = appendPath(path_, at, key_, key_len_, key_cut_);
}

void Json::putUtf8(uint32_t cp, char* buf, uint16_t cap, uint16_t* len, bool* cut) {
    char b[4];
    int n;
    if (cp < 0x80) b[0] = (char)cp, n = 1;
    else if (cp < 0x800) b[0] = (char)(0xC0 | cp >> 6), b[1] = (char)(0x80 | (cp & 0x3F)), n = 2;
    else if (cp < 0x10000)
        b[0] = (char)(0xE0 | cp >> 12), b[1] = (char)(0x80 | ((cp >> 6) & 0x3F)), b[2] = (char)(0x80 | (cp & 0x3F)), n = 3;
    else
        b[0] = (char)(0xF0 | cp >> 18), b[1] = (char)(0x80 | ((cp >> 12) & 0x3F)), b[2] = (char)(0x80 | ((cp >> 6) & 0x3F)),
        b[3] = (char)(0x80 | (cp & 0x3F)), n = 4;
    if (*len + n > cap) {
        *cut = true;
        return;
    }
    memcpy(buf + *len, b, n);
    *len += n;
}

// One character inside a string (after its opening quote); false: not JSON.
bool Json::stringChar(char c, char* buf, uint16_t cap, uint16_t* len, bool* cut) {
    if (esc_ == 1) {
        esc_ = 0;
        char out;
        switch (c) {
            case '"': out = '"'; break;
            case '\\': out = '\\'; break;
            case '/': out = '/'; break;
            case 'b': out = '\b'; break;
            case 'f': out = '\f'; break;
            case 'n': out = '\n'; break;
            case 'r': out = '\r'; break;
            case 't': out = '\t'; break;
            case 'u':
                esc_ = 2;
                u_ = 0;
                return true;
            default: return false;
        }
        if (high_) putUtf8(0xFFFD, buf, cap, len, cut), high_ = 0;
        putUtf8((uint8_t)out, buf, cap, len, cut);
        return true;
    }
    if (esc_ >= 2) {
        int v = hexValue(c);
        if (v < 0) return false;
        u_ = u_ << 4 | (uint32_t)v;
        if (++esc_ < 6) return true;
        esc_ = 0;
        if (u_ >= 0xD800 && u_ <= 0xDBFF) {  // the first half of a pair
            if (high_) putUtf8(0xFFFD, buf, cap, len, cut);
            high_ = u_;
        } else if (u_ >= 0xDC00 && u_ <= 0xDFFF) {
            if (high_) putUtf8(0x10000 + ((high_ - 0xD800) << 10) + (u_ - 0xDC00), buf, cap, len, cut);
            else putUtf8(0xFFFD, buf, cap, len, cut);
            high_ = 0;
        } else {
            if (high_) putUtf8(0xFFFD, buf, cap, len, cut), high_ = 0;
            putUtf8(u_, buf, cap, len, cut);
        }
        return true;
    }
    if (c == '\\') {
        esc_ = 1;
        return true;
    }
    if ((unsigned char)c < 0x20) return false;
    if (high_) putUtf8(0xFFFD, buf, cap, len, cut), high_ = 0;
    if (*len < cap) buf[(*len)++] = c;
    else *cut = true;
    return true;
}

bool Json::startValue(char c) {
    if (c == '{' || c == '[') {
        if (depth_ == JSON_DEPTH_MAX) return false;
        bool array = c == '[';
        stack_[depth_++] = Frame{array, path_len_};
        if (h_) h_->begin(path_, array);
        state_ = array ? S_ARRAY_FIRST : S_OBJECT_FIRST;
        return true;
    }
    if (c == '"') {
        str_len_ = 0;
        str_cut_ = false;
        high_ = 0;
        esc_ = 0;
        state_ = S_STRING;
        return true;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        str_[0] = c;
        str_len_ = 1;
        state_ = S_NUMBER;
        return true;
    }
    if (c == 't' || c == 'f' || c == 'n') {
        str_[0] = c;
        str_len_ = 1;
        state_ = S_LITERAL;
        return true;
    }
    return false;
}

// A value just ended: what may come next
bool Json::afterValue() {
    state_ = depth_ == 0 ? S_DONE : S_AFTER;
    return true;
}

bool Json::step(char c) {
    switch (state_) {
        case S_VALUE:
            if (isSpace(c)) return true;
            return startValue(c);

        case S_ARRAY_FIRST:
            if (isSpace(c)) return true;
            if (c == ']') goto close;
            enterElement();
            return startValue(c);

        case S_OBJECT_FIRST:
        case S_NEXT_KEY:
            if (isSpace(c)) return true;
            if (c == '}' && state_ == S_OBJECT_FIRST) goto close;
            if (c != '"') return false;
            key_len_ = 0;
            key_cut_ = false;
            high_ = 0;
            esc_ = 0;
            state_ = S_KEY;
            return true;

        case S_KEY:
            if (c == '"' && esc_ == 0) {
                if (high_) putUtf8(0xFFFD, key_, JSON_KEY_MAX, &key_len_, &key_cut_), high_ = 0;
                key_[key_len_] = 0;
                state_ = S_COLON;
                return true;
            }
            return stringChar(c, key_, JSON_KEY_MAX, &key_len_, &key_cut_);

        case S_COLON:
            if (isSpace(c)) return true;
            if (c != ':') return false;
            enterKey();
            state_ = S_VALUE;
            return true;

        case S_STRING:
            if (c == '"' && esc_ == 0) {
                if (high_) putUtf8(0xFFFD, str_, JSON_STRING_MAX, &str_len_, &str_cut_), high_ = 0;
                str_[str_len_] = 0;
                emit(Json_String, str_, str_len_, str_cut_);
                return afterValue();
            }
            return stringChar(c, str_, JSON_STRING_MAX, &str_len_, &str_cut_);

        case S_NUMBER:
            if (isNumberChar(c)) {
                if (str_len_ < 32) str_[str_len_++] = c;
                return true;
            }
            str_[str_len_] = 0;
            emit(Json_Number, str_, str_len_, false);
            afterValue();
            return step(c);  // (what ended the number is the next thing)

        case S_LITERAL:
            if (c >= 'a' && c <= 'z') {
                if (str_len_ >= 5) return false;
                str_[str_len_++] = c;
                return true;
            }
            str_[str_len_] = 0;
            if (strcmp(str_, "true") == 0) emit(Json_True, str_, 4, false);
            else if (strcmp(str_, "false") == 0) emit(Json_False, str_, 5, false);
            else if (strcmp(str_, "null") == 0) emit(Json_Null, str_, 4, false);
            else return false;
            afterValue();
            return step(c);

        case S_AFTER: {
            if (isSpace(c)) return true;
            bool array = stack_[depth_ - 1].array;
            if (c == ',') {
                if (array) {
                    enterElement();
                    state_ = S_VALUE;
                } else {
                    state_ = S_NEXT_KEY;  // (only a key: a trailing comma isn't JSON)
                }
                return true;
            }
            if ((array && c == ']') || (!array && c == '}')) goto close;
            return false;
        }

        case S_DONE:
            return isSpace(c);

        case S_ERROR:
            return false;
    }
    return false;

close : {
    bool array = stack_[depth_ - 1].array;
    if ((array && c != ']') || (!array && c != '}')) return false;
    path_len_ = stack_[depth_ - 1].path_len;
    path_[path_len_] = 0;
    depth_--;
    if (h_) h_->end(path_, array);
    return afterValue();
}
}
