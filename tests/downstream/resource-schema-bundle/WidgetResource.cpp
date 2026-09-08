#include "WidgetResource.h"

namespace downstream_fixture {
WidgetResource::WidgetResource(
    std::string const& name, std::string const& namesp,
    std::map<std::string, std::string> const& tags,
    wp::application::resourcesystem::ResourceLocation* location)
    : Resource(name, namesp, "Widget", "", tags, location) {}

WidgetDefaultDefinitionFactory::WidgetDefaultDefinitionFactory()
    : ResourceDefinitionFactory("Widget", "") {}

void WidgetDefaultDefinitionFactory::create(
    wp::application::resourcesystem::Resource*,
    wp::application::resourcesystem::ResourceManager*, wp::DataNode*) {}

WidgetGlowDefinitionFactory::WidgetGlowDefinitionFactory()
    : ResourceDefinitionFactory("Widget", "GlowFactory") {}

void WidgetGlowDefinitionFactory::create(
    wp::application::resourcesystem::Resource*,
    wp::application::resourcesystem::ResourceManager*, wp::DataNode*) {}
}  // namespace downstream_fixture
