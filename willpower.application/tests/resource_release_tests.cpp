#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "willpower/common/Logger.h"
#include "willpower/application/resourcesystem/DirectoryResourceLocation.h"
#include "willpower/application/resourcesystem/ResourceCallback.h"
#include "willpower/application/resourcesystem/ResourceFactory.h"
#include "willpower/application/resourcesystem/ResourceManager.h"

namespace {
namespace fs = std::filesystem;
using wp::application::resourcesystem::DirectoryResourceLocation;
using wp::application::resourcesystem::Resource;
using wp::application::resourcesystem::ResourceCallback;
using wp::application::resourcesystem::ResourceFactory;
using wp::application::resourcesystem::ResourceLocation;
using wp::application::resourcesystem::ResourceManager;
using wp::application::resourcesystem::ResourcePtr;
using wp::application::resourcesystem::ResourceState;

void require(bool condition, std::string const& message) {
  if (!condition) throw std::runtime_error(message);
}

void writeFile(fs::path const& path, std::string const& contents) {
  fs::create_directories(path.parent_path());
  std::ofstream stream(path);
  stream << contents;
  if (!stream) throw std::runtime_error("Could not write test fixture: " + path.string());
}

struct ResourceStats {
  int creates{0};
  int loads{0};
  int unloads{0};

  bool operator==(ResourceStats const&) const = default;
};

using ResourceStatsMap = std::map<std::string, ResourceStats>;

class TrackingResource final : public Resource {
  ResourceStatsMap* mStats;

public:
  TrackingResource(std::string const& name, std::string const& namesp,
                   std::string const& source,
                   std::map<std::string, std::string> const& tags,
                   ResourceLocation* location, ResourceStatsMap* stats)
      : Resource(name, namesp, "TestResource", source, tags, location),
        mStats(stats) {
  }

  void create(wp::application::resourcesystem::DataStreamPtr,
              ResourceManager*) override {
    ++(*mStats)[getName()].creates;
  }

  bool load(mpp::RenderSystem*, mpp::ResourceManager*) override {
    ++(*mStats)[getName()].loads;
    return true;
  }

  bool unload(mpp::RenderSystem*, mpp::ResourceManager*) override {
    ++(*mStats)[getName()].unloads;
    return true;
  }
};

class TrackingResourceFactory final : public ResourceFactory {
  ResourceStatsMap* mStats;

public:
  explicit TrackingResourceFactory(ResourceStatsMap* stats)
      : ResourceFactory("TestResource"), mStats(stats) {
  }

  Resource* createResource(std::string const& name, std::string const& namesp,
                           std::string const& source,
                           std::map<std::string, std::string> const& tags,
                           ResourceLocation* location) override {
    return new TrackingResource(name, namesp, source, tags, location, mStats);
  }
};

void configureDirectoryFactory(ResourceManager& manager, wp::Logger& logger) {
  manager.addResourceLocationFactory(
      "Directory", [&logger](std::string const& path, std::string const& definition) -> ResourceLocation* {
        return new DirectoryResourceLocation(&logger, path, definition);
      });
}

class TestEnvironment {
  fs::path mRoot;
  wp::Logger mLogger;

public:
  ResourceStatsMap stats;
  ResourceManager manager;

  TestEnvironment()
      : mRoot(makeRoot()), manager(nullptr, nullptr, nullptr, &mLogger) {
    writeFile(mRoot / "Resources.yaml", R"(Resources:
  Resource:
    - type: "TestResource"
      name: "InstantiatedWithoutCallback"
    - type: "TestResource"
      name: "InstantiatedWithCallback"
    - type: "TestResource"
      name: "CreatedWithoutCallback"
    - type: "TestResource"
      name: "CreatedWithCallback"
    - type: "TestResource"
      name: "LoadedWithoutCallback"
    - type: "TestResource"
      name: "LoadedWithCallback"
    - type: "TestResource"
      name: "ChildWithoutCallback"
    - type: "TestResource"
      name: "OwnerWithoutCallback"
      DependentResources:
        DependentResource:
          ref: "ChildWithoutCallback"
    - type: "TestResource"
      name: "ChildWithCallback"
    - type: "TestResource"
      name: "OwnerWithCallback"
      DependentResources:
        DependentResource:
          ref: "ChildWithCallback"
)");

    configureDirectoryFactory(manager, mLogger);
    manager.addResourceFactory(new TrackingResourceFactory(&stats));
    manager.addResourceLocation("Directory", mRoot.string(), "Resources.yaml");
    manager.scanLocations();
  }

  ~TestEnvironment() {
    fs::remove_all(mRoot);
  }

private:
  static fs::path makeRoot() {
    auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() /
           ("willpower-resource-release-" + std::to_string(unique));
  }
};

struct ResourceSnapshot {
  bool created;
  bool loaded;
  ResourceStats stats;

  bool operator==(ResourceSnapshot const&) const = default;
};

ResourceSnapshot snapshot(ResourceManager const& manager, ResourcePtr resource,
                          ResourceStatsMap const& stats) {
  auto statsIt = stats.find(resource->getName());
  auto resourceStats = statsIt == stats.end() ? ResourceStats{} : statsIt->second;
  return {manager.isResourceCreated(resource), manager.isResourceLoaded(resource),
          resourceStats};
}

struct CallbackEvent {
  std::string resourceName;
  ResourceState state;
  bool rootResource;

  bool operator==(CallbackEvent const&) const = default;
};

enum class InitialState {
  Instantiated,
  Created,
  Loaded,
};

struct ReleaseRun {
  ResourceSnapshot afterInitialRelease;
  ResourceSnapshot afterUnloadBoundary;
  std::vector<CallbackEvent> events;
};

ReleaseRun releaseFrom(TestEnvironment& environment, std::string const& resourceName,
                       InitialState initialState, bool observeCallbacks) {
  auto resource = environment.manager.getResource(resourceName);
  if (initialState != InitialState::Instantiated) {
    environment.manager.createResource(resource);
  }
  if (initialState == InitialState::Loaded) {
    environment.manager.loadResource(resource);
  }

  std::vector<CallbackEvent> events;
  ResourceCallback callback;
  if (observeCallbacks) {
    callback = [&events](ResourcePtr callbackResource, ResourceState state,
                         bool rootResource) {
      events.push_back({callbackResource->getName(), state, rootResource});
    };
  }

  environment.manager.acquireResource(resource);
  environment.manager.releaseResource(resource, callback);
  auto afterInitialRelease = snapshot(environment.manager, resource, environment.stats);

  if (initialState != InitialState::Loaded) {
    environment.manager.createResource(resource, callback);
    environment.manager.loadResource(resource, callback);
    environment.manager.acquireResource(resource);
    environment.manager.releaseResource(resource, callback);
  }

  return {afterInitialRelease,
          snapshot(environment.manager, resource, environment.stats), std::move(events)};
}

void requireEvents(std::vector<CallbackEvent> const& actual,
                   std::string const& resourceName,
                   std::vector<ResourceState> const& expected,
                   std::string const& scenario) {
  require(actual.size() == expected.size(),
          scenario + " produced an unexpected number of callback events.");
  for (size_t index = 0; index < expected.size(); ++index) {
    require(actual[index].resourceName == resourceName && actual[index].rootResource,
            scenario + " reported a non-root callback event.");
    require(actual[index].state == expected[index],
            scenario + " did not report the expected lifecycle transition.");
  }
}

void testReleaseParityForState(TestEnvironment& environment, InitialState initialState,
                               std::vector<ResourceState> const& expectedEvents,
                               std::string const& scenario) {
  auto withoutCallback = releaseFrom(environment, scenario + "WithoutCallback",
                                     initialState, false);
  auto withCallback = releaseFrom(environment, scenario + "WithCallback",
                                  initialState, true);

  require(withoutCallback.events.empty(),
          scenario + " invoked callbacks when none was supplied.");
  require(withoutCallback.afterInitialRelease == withCallback.afterInitialRelease,
          scenario + " changed ownership or lifecycle state when observed.");
  require(withoutCallback.afterUnloadBoundary == withCallback.afterUnloadBoundary,
          scenario + " reached a different unload boundary when observed.");
  require(!withCallback.afterUnloadBoundary.loaded &&
              withCallback.afterUnloadBoundary.stats.unloads == 1,
          scenario + " did not release its final acquired reference.");
  requireEvents(withCallback.events, scenario + "WithCallback", expectedEvents, scenario);
}

struct DependencyRun {
  ResourceSnapshot owner;
  ResourceSnapshot child;
  std::vector<CallbackEvent> events;
};

DependencyRun releaseDependentOwner(TestEnvironment& environment, bool observeCallbacks) {
  auto suffix = observeCallbacks ? "WithCallback" : "WithoutCallback";
  auto owner = environment.manager.getResource(std::string("Owner") + suffix);
  auto child = environment.manager.getResource(std::string("Child") + suffix);
  environment.manager.createResource(owner);
  environment.manager.loadResource(owner);

  std::vector<CallbackEvent> events;
  ResourceCallback callback;
  if (observeCallbacks) {
    callback = [&events](ResourcePtr resource, ResourceState state, bool rootResource) {
      events.push_back({resource->getName(), state, rootResource});
    };
  }

  environment.manager.acquireResource(owner);
  environment.manager.releaseResource(owner, callback);
  environment.manager.releaseResource(owner, callback);

  return {snapshot(environment.manager, owner, environment.stats),
          snapshot(environment.manager, child, environment.stats), std::move(events)};
}

void testDependentReleaseAtUnloadBoundary(TestEnvironment& environment) {
  auto withoutCallback = releaseDependentOwner(environment, false);
  auto withCallback = releaseDependentOwner(environment, true);

  require(withoutCallback.owner == withCallback.owner &&
              withoutCallback.child == withCallback.child,
          "Callbacks changed dependent-resource ownership or lifecycle state.");
  require(withCallback.owner.stats.unloads == 1 &&
              withCallback.child.stats.unloads == 1,
          "Owner unload did not release each resource exactly once.");

  int childReleasing = 0;
  int childUnloading = 0;
  for (auto const& event : withCallback.events) {
    if (event.resourceName != "ChildWithCallback") continue;
    require(!event.rootResource,
            "Dependent resource was reported as a root callback event.");
    childReleasing += event.state == ResourceState::Releasing;
    childUnloading += event.state == ResourceState::Unloading;
  }
  require(childReleasing == 1 && childUnloading == 1,
          "Dependent resource was released more than once at its owner's unload boundary.");
}
}  // namespace

int main() {
  try {
    TestEnvironment environment;
    testReleaseParityForState(
        environment, InitialState::Instantiated,
        {ResourceState::Releasing, ResourceState::Instantiated, ResourceState::Released,
         ResourceState::Creating, ResourceState::Created, ResourceState::Loading,
         ResourceState::Loaded, ResourceState::Releasing, ResourceState::Unloading,
         ResourceState::Created, ResourceState::Released},
        "Instantiated");
    testReleaseParityForState(
        environment, InitialState::Created,
        {ResourceState::Releasing, ResourceState::Created, ResourceState::Released,
         ResourceState::Loading, ResourceState::Loaded, ResourceState::Releasing,
         ResourceState::Unloading, ResourceState::Created, ResourceState::Released},
        "Created");
    testReleaseParityForState(
        environment, InitialState::Loaded,
        {ResourceState::Releasing, ResourceState::Unloading, ResourceState::Created,
         ResourceState::Released},
        "Loaded");
    testDependentReleaseAtUnloadBoundary(environment);

    std::cout << "Resource release tests passed\n";
    return 0;
  } catch (std::exception const& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
