#pragma once

#include <map>
#include <string>

#include "willpower/application/resourcesystem/Resource.h"
#include "willpower/application/resourcesystem/ResourceDefinitionFactory.h"

namespace downstream_fixture {
class WidgetResource final : public wp::application::resourcesystem::Resource {
 public:
  WidgetResource(std::string const& name, std::string const& namesp,
                 std::map<std::string, std::string> const& tags,
                 wp::application::resourcesystem::ResourceLocation* location);
};

class WidgetDefaultDefinitionFactory final
    : public wp::application::resourcesystem::ResourceDefinitionFactory {
 public:
  WidgetDefaultDefinitionFactory();
  void create(wp::application::resourcesystem::Resource* resource,
              wp::application::resourcesystem::ResourceManager* resourceManager,
              wp::DataNode* definition) override;
};

class WidgetGlowDefinitionFactory final
    : public wp::application::resourcesystem::ResourceDefinitionFactory {
 public:
  WidgetGlowDefinitionFactory();
  void create(wp::application::resourcesystem::Resource* resource,
              wp::application::resourcesystem::ResourceManager* resourceManager,
              wp::DataNode* definition) override;
};
}  // namespace downstream_fixture
