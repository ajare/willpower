#pragma once

#include <memory>
#include <vector>
#include <map>

#include <mpp/RenderSystem.h>
#include <mpp/ResourceManager.h>

#include "willpower/common/Logger.h"

#include "willpower/application/Platform.h"
#include "willpower/application/resourcesystem/ResourceRecord.h"
#include "willpower/application/resourcesystem/ResourceLocation.h"
#include "willpower/application/resourcesystem/ResourceFactory.h"
#include "willpower/application/resourcesystem/ResourceDefinitionFactory.h"
#include "willpower/application/resourcesystem/ResourceCallback.h"
#include "willpower/application/resourcesystem/Resource.h"
#include "willpower/application/AudioSystem.h"

namespace WP_NAMESPACE {
namespace application {
namespace resourcesystem {

/**
 * \class ResourceManager
 *
 * \brief Owns resource locations, resources and the factories that create them.
 *
 * Lifetime contract: a ResourceManager may be constructed, destroyed and
 * reconstructed any number of times within one process (e.g. per test case,
 * or for a "reload everything" feature). Because Resource instances look up
 * definition factories without holding a manager reference, the default
 * ResourceDefinitionFactory objects are registered in a process-wide static
 * registry (Resource::msResourceDefinitionFactories). The registry is cleared
 * by the destructor, so every construct-after-destroy starts from a clean
 * registry.
 *
 * Concurrently alive ResourceManager instances are NOT supported: the second
 * constructor throws (ResourceSystemException) because the default definition
 * factories are already registered. Registering two factories for the same
 * (resource type, factory type) pair is a programming error and throws the
 * same exception. The manager takes ownership of every factory passed to
 * addResourceFactory()/addResourceDefinitionFactory() on every code path,
 * including the throwing ones.
 */
class WP_APPLICATION_API ResourceManager {
  struct ResourceLocationRecord {
    ResourceLocation* location;
    bool scanned;
  };

public:
  typedef std::map<std::string, ResourceRecord> ResourceRecordMap;
  typedef ResourceRecordMap::const_iterator ResourceRecordIterator;

private:
  Logger* mwLogger;

  // Render system for graphics resources
  mpp::ResourceManager* mwRenderResourceMgr;

  mpp::RenderSystem* mwRenderSystem;

  // Audio system
  AudioSystem* mwAudioSystem;

  // Resource locations
  std::map<std::string, ResourceLocationFactory> mLocationFactories;

  std::vector<ResourceLocationRecord> mLocations;

  // Resource records
  std::map<std::string, ResourceRecordMap> mNamespaces;

  // Resource factories. Held by unique_ptr so a constructor that throws
  // (e.g. because another ResourceManager is still alive and owns the
  // process-wide definition factory registry) does not leak the factories
  // it already registered.
  std::map<std::string, std::unique_ptr<ResourceFactory>> mResourceFactories;

  typedef std::map<std::string, ResourcePtr> ResourceMap;
  std::map<std::string, ResourceMap> mResources;

private:
  void addResourceRecord(ResourceRecord const& record);

  void validateResourceDependencies() const;

  void instantiateAllResources(bool create, bool load, ResourceCallback callback = nullptr, bool rootResource = true);

  ResourcePtr instantiateResource(ResourceRecord const& record, bool create = false, bool load = false, ResourceCallback callback = nullptr, bool rootResource = true);

  void _createResource(ResourcePtr resource, ResourceCallback callback = nullptr, bool rootResource = true);

  void _destroyResource(ResourcePtr resource, ResourceCallback callback = nullptr, bool rootResource = true);

  void _loadResource(ResourcePtr resource, ResourceCallback callback = nullptr, bool rootResource = true);

  void _unloadResource(ResourcePtr resource, ResourceCallback callback = nullptr, bool rootResource = true);

  static std::vector<std::string> sortResourcesByDependency(std::vector<std::string> const& resourceNames, std::map<std::string, std::vector<std::string>> dependencies);

public:
  ResourceManager(mpp::RenderSystem* renderSystem, mpp::ResourceManager* renderResourceMgr, AudioSystem* audioSystem, Logger* logger);

  virtual ~ResourceManager();

  void addResourceFactory(ResourceFactory* factory);

  void addResourceLocationFactory(std::string const& type, ResourceLocationFactory factory);

  void addResourceDefinitionFactory(ResourceDefinitionFactory* factory);

  void addResourceLocation(std::string const& type, std::string const& location, std::string const& definitionFile);

  void addResources(std::string const& file);

  // Scans each configured location at most once. Repeated ordinary scans are
  // no-ops.
  void scanLocations(ResourceLocationCallback callback = nullptr);

  // Re-reads every configured location and instantiates resources that were
  // added since the initial scan. Existing instantiated resources are left
  // untouched; changing or removing their declarations remains unsupported.
  void rescanLocations(ResourceLocationCallback callback = nullptr);

  // Registers an already-instantiated, programmatic Resource. This is used
  // for host-owned built-ins which have no ResourceLocation or manifest
  // record.
  void addResource(ResourcePtr resource);

  ResourcePtr getResource(std::string const& name, std::string const& namesp = "");

  ResourcePtr getQualifiedResource(std::string const& name);

  std::vector<ResourcePtr> getResourcesByType(std::string const& type);

  std::vector<ResourcePtr> getNamespaceResources(std::string const& namesp);

  std::vector<ResourcePtr> getAllResources();

  ResourceDefinitionFactory* getResourceDefinitionFactory(std::string const& resType, std::string const& facType, bool errorIfNotFound = true);

  ResourcePtr acquireResource(std::string const& name, std::string const& namesp = "");

  void acquireResource(ResourcePtr resource);

  void releaseResource(ResourcePtr resource, ResourceCallback callback = nullptr, bool rootResource = true);

  bool isResourceCreated(ResourcePtr resource) const;

  bool isResourceLoaded(ResourcePtr resource) const;

  void createResource(ResourcePtr resource, ResourceCallback callback = nullptr, bool rootResource = true);

  void loadResource(ResourcePtr resource, ResourceCallback callback = nullptr, bool rootResource = true);

  void createNamespaceResources(std::string const& namesp, ResourceCallback callback = nullptr);

  void createAllResources(ResourceCallback callback = nullptr);

  void createResources(std::vector<ResourcePtr> const& resources, ResourceCallback callback = nullptr);

  void destroyNamespaceResources(std::string const& namesp, ResourceCallback callback = nullptr);

  void destroyAllResources(ResourceCallback callback = nullptr);

  void destroyResources(std::vector<ResourcePtr> const& resources, ResourceCallback callback = nullptr);

  void loadNamespaceResources(std::string const& namesp, bool createFirst, ResourceCallback callback = nullptr);

  void loadAllResources(bool createFirst, ResourceCallback callback = nullptr);

  void loadResources(std::vector<ResourcePtr> const& resources, bool createFirst, ResourceCallback callback = nullptr);
};

}  // namespace resourcesystem
}  // namespace application
}  // namespace WP_NAMESPACE
