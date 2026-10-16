#pragma once
// A small streaming JSON writer for the status and calibration files: fields are added by name, commas and
// brackets are its business. Numbers keep the formats the readers of those files know (%.9g floats and so
// on); strings are escaped. Portable: a FILE* is all it needs.
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace fp {

class JsonOut {
public:
    explicit JsonOut(FILE* out) : f(out) { open('{'); }

    // Each value takes its key inside an object, nullptr inside an array.
    JsonOut& num(const char* key, long long v) {
        field(key);
        fprintf(f, "%lld", v);
        return *this;
    }
    JsonOut& unum(const char* key, unsigned long long v) {
        field(key);
        fprintf(f, "%llu", v);
        return *this;
    }
    JsonOut& real(const char* key, double v, int digits = 9) {
        field(key);
        fprintf(f, "%.*g", digits, v);
        return *this;
    }
    JsonOut& fixed(const char* key, double v, int decimals) {
        field(key);
        fprintf(f, "%.*f", decimals, v);
        return *this;
    }
    JsonOut& boolean(const char* key, bool v) {
        field(key);
        fputs(v ? "true" : "false", f);
        return *this;
    }
    JsonOut& null(const char* key) {
        field(key);
        fputs("null", f);
        return *this;
    }
    JsonOut& hex(const char* key, unsigned long long v) {
        field(key);
        fprintf(f, "\"0x%llx\"", v);
        return *this;
    }
    // At most `limit` bytes of v: fixed-size game buffers need not be terminated.
    JsonOut& str(const char* key, const char* v, size_t limit = SIZE_MAX) {
        field(key);
        fputc('"', f);
        for (size_t i = 0; v && i < limit && v[i]; ++i) {
            const unsigned char c = (unsigned char)v[i];
            if (c == '"' || c == '\\')
                fprintf(f, "\\%c", c);
            else if (c < 0x20)
                fprintf(f, "\\u%04x", c);
            else
                fputc(c, f);
        }
        fputc('"', f);
        return *this;
    }
    JsonOut& object(const char* key) {
        field(key);
        open('{');
        return *this;
    }
    JsonOut& array(const char* key) {
        field(key);
        open('[');
        return *this;
    }
    JsonOut& end() {
        if (depth) fputc(closers[--depth], f);
        first = false;
        return *this;
    }
    template <class T> JsonOut& reals(const char* key, const T* v, unsigned n, int digits = 9) {
        array(key);
        for (unsigned i = 0; i < n; ++i) real(nullptr, double(v[i]), digits);
        return end();
    }
    // Closes everything still open and ends the line.
    void finish() {
        while (depth) end();
        fputc('\n', f);
    }

private:
    FILE* f;
    char closers[16]{};
    unsigned depth = 0;
    bool first = true;
    void open(char c) {
        fputc(c, f);
        if (depth < sizeof closers) closers[depth++] = c == '{' ? '}' : ']';
        first = true;
    }
    void field(const char* key) {
        if (!first) fputc(',', f);
        first = false;
        if (key) fprintf(f, "\"%s\":", key);
    }
};

} // namespace fp
