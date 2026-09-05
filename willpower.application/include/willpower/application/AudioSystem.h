#pragma once

#include "willpower/application/Platform.h"

#include "willpower/application/AudioOptions.h"
#include "willpower/application/resourcesystem/Resource.h"

namespace FMOD {
namespace Studio {
class System;
class EventInstance;
}  // namespace Studio
}  // namespace FMOD

namespace WP_NAMESPACE {
namespace application {

namespace resourcesystem {
class AudioBankResource;
}

class WP_APPLICATION_API AudioSystem {
  FMOD::Studio::System* mSystem;

public:
  explicit AudioSystem(AudioOptions const& options);

  ~AudioSystem();

  void createAudioBank(resourcesystem::AudioBankResource* audioBank, resourcesystem::DataStreamPtr dataPtr);

  FMOD::Studio::EventInstance* startEvent(std::string const& eventName);

  void setEventVolume(FMOD::Studio::EventInstance* inst, float volume);

  void update();
};

}  // namespace application
}  // namespace WP_NAMESPACE
