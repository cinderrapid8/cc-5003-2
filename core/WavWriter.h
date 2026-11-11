#pragma once

#include <string>
#include <vector>

namespace mix {

// PCM16 WAV writer. Called only from the control/offline path — the audio
// callback never performs file I/O.
bool writeWav(const std::string& path,
              const std::vector<float>& interleaved, int channels,
              int sampleRate);

}  // namespace mix
