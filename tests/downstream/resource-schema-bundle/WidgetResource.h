#pragma once

#include <map>
#include <string>

#include "willpower/application/resourcesystem/Resource.h"

namespace downstream_fixture {
class WidgetResource final : public wp::application::resourcesystem::Resource {
 public:
  WidgetResource(std::string const& name, std::string const& namesp,
                 std::map<std::string, std::string> const& tags,
                 wp::application::resourcesystem::ResourceLocation* location);
};
}  // namespace downstream_fixture
