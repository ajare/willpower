#include "willpower/common/StringUtils.h"

#include "willpower/application/resourcesystem/AudioBankDefaultDefinitionFactory.h"
#include "willpower/application/resourcesystem/AudioBankResource.h"
#include "willpower/application/resourcesystem/ResourceExceptions.h"
#include "willpower/common/Exceptions.h"

namespace WP_NAMESPACE {
namespace application {
namespace resourcesystem {
using namespace std;
using namespace wp;

AudioBankDefaultDefinitionFactory::AudioBankDefaultDefinitionFactory()
    : AudioBankResourceDefinitionFactory("") {
}

void AudioBankDefaultDefinitionFactory::create(application::resourcesystem::Resource* resource, application::resourcesystem::ResourceManager* resourceMgr, wp::DataNode* node) {
  WP_UNUSED(resource);
  WP_UNUSED(resourceMgr);
  WP_UNUSED(node);
}

}  // namespace resourcesystem
}  // namespace application
}  // namespace WP_NAMESPACE
