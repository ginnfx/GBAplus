#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

// Minimal byte-blit (de)serializers for the emulator's save states. Every
// component's state is trivially copyable, so raw POD blits are sufficient.
// Save states are not portable across architectures/compilers (no endianness
// or layout normalisation) — they are meant for the same build on one machine.

class Serializer {
public:
    explicit Serializer(std::vector<uint8_t>& buffer) : buf(buffer) {}

    template <class T>
    void pod(const T& value) {
        const auto* p = reinterpret_cast<const uint8_t*>(&value);
        buf.insert(buf.end(), p, p + sizeof(T));
    }

    void bytes(const void* data, size_t n) {
        const auto* p = static_cast<const uint8_t*>(data);
        buf.insert(buf.end(), p, p + n);
    }

private:
    std::vector<uint8_t>& buf;
};

class Deserializer {
public:
    Deserializer(const uint8_t* data, size_t size)
        : cur(data), end(data + size) {}

    template <class T>
    void pod(T& value) {
        if (cur + sizeof(T) > end) {
            valid = false;
            return;
        }
        std::memcpy(&value, cur, sizeof(T));
        cur += sizeof(T);
    }

    void bytes(void* dst, size_t n) {
        if (cur + n > end) {
            valid = false;
            return;
        }
        std::memcpy(dst, cur, n);
        cur += n;
    }

    bool ok() const { return valid; }

private:
    const uint8_t* cur;
    const uint8_t* end;
    bool valid = true;
};
