#include "WavWriter.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mix {

namespace {

void put32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(v & 0xff);
    b.push_back((v >> 8) & 0xff);
    b.push_back((v >> 16) & 0xff);
    b.push_back((v >> 24) & 0xff);
}
void put16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(v & 0xff);
    b.push_back((v >> 8) & 0xff);
}

}  // namespace

bool writeWav(const std::string& path,
              const std::vector<float>& interleaved, int channels,
              int sampleRate) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;

    const uint32_t dataBytes =
        static_cast<uint32_t>(interleaved.size() * sizeof(int16_t));
    std::vector<uint8_t> hdr;
    hdr.reserve(44);
    hdr.insert(hdr.end(), {'R', 'I', 'F', 'F'});
    put32(hdr, 36 + dataBytes);
    hdr.insert(hdr.end(), {'W', 'A', 'V', 'E'});
    hdr.insert(hdr.end(), {'f', 'm', 't', ' '});
    put32(hdr, 16);
    put16(hdr, 1);                       // PCM
    put16(hdr, static_cast<uint16_t>(channels));
    put32(hdr, static_cast<uint32_t>(sampleRate));
    put32(hdr, static_cast<uint32_t>(sampleRate * channels * 2));
    put16(hdr, static_cast<uint16_t>(channels * 2));
    put16(hdr, 16);
    hdr.insert(hdr.end(), {'d', 'a', 't', 'a'});
    put32(hdr, dataBytes);

    bool ok = std::fwrite(hdr.data(), 1, hdr.size(), f) == hdr.size();
    for (float v : interleaved) {
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        int16_t s = static_cast<int16_t>(v * 32767.0f);
        ok &= std::fwrite(&s, sizeof(s), 1, f) == 1;
    }
    std::fclose(f);
    return ok;
}

}  // namespace mix
