#include "WidgetResource.h"

#include <utility>

namespace downstream_fixture {
WidgetResource::WidgetResource(
    std::string const& name, std::string const& namesp,
    std::map<std::string, std::string> const& tags,
    wp::application::resourcesystem::ResourceLocation* location)
    : Resource(name, namesp, "Widget", "", tags, location) {}
}  // namespace downstream_fixture
