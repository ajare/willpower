#pragma once

#include "willpower/application/Platform.h"

namespace WP_NAMESPACE {
namespace application {

// The core system's output channel layout. This mirrors a subset of
// FMOD_SPEAKERMODE without exposing FMOD in a public header (see
// AudioSystem's own note on that). Default follows whatever the OS reports
// for the active output device.
enum class SpeakerMode {
  Default,
  Stereo,
  Surround5Point1,
};

struct AudioOptions {
  bool synchronous{false};
  int numChannels{1024};
  SpeakerMode speakerMode{SpeakerMode::Default};
};

}  // namespace application
}  // namespace WP_NAMESPACE
