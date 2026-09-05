#include <mpp/TextureStream.h>

#include "willpower/common/StringUtils.h"

#include "willpower/common/Exceptions.h"

#include "willpower/application/resourcesystem/ResourceExceptions.h"
#include "willpower/application/resourcesystem/ResourceManager.h"
#include "willpower/application/resourcesystem/ImageResource.h"
#include "willpower/application/resourcesystem/ImageSetResource.h"
#include "willpower/application/resourcesystem/AnimationSetResource.h"
#include "willpower/application/resourcesystem/ProgramResource.h"
#include "willpower/application/resourcesystem/ShaderResource.h"
#include "willpower/application/resourcesystem/MaterialResource.h"
#include "willpower/application/resourcesystem/TextFileResource.h"
#include "willpower/application/resourcesystem/XmlFileResource.h"
#include "willpower/application/resourcesystem/AudioBankResource.h"
#include "willpower/application/resourcesystem/AnimationSetDefaultDefinitionFactory.h"
#include "willpower/application/resourcesystem/ImageDefaultDefinitionFactory.h"
#include "willpower/application/resourcesystem/ImageSetDefaultDefinitionFactory.h"
#include "willpower/application/resourcesystem/ProgramDefaultDefinitionFactory.h"
#include "willpower/application/resourcesystem/ShaderDefaultDefinitionFactory.h"
#include "willpower/application/resourcesystem/MaterialDefaultDefinitionFactory.h"
#include "willpower/application/resourcesystem/TextFileDefaultDefinitionFactory.h"
#include "willpower/application/resourcesystem/XmlFileDefaultDefinitionFactory.h"
#include "willpower/application/resourcesystem/AudioBankDefaultDefinitionFactory.h"

namespace WP_NAMESPACE {
namespace application {
namespace resourcesystem {
using namespace std;

ResourceManager::ResourceManager(mpp::RenderSystem* renderSystem, mpp::ResourceManager* renderResourceMgr, AudioSystem* audioSystem, Logger* logger)
    : mwLogger(logger), mwRenderResourceMgr(renderResourceMgr), mwRenderSystem(renderSystem), mwAudioSystem(audioSystem)

{
  addResourceFactory(new TextFileResourceFactory);
  addResourceFactory(new XmlFileResourceFactory);
  addResourceFactory(new ImageResourceFactory);
  addResourceFactory(new ImageSetResourceFactory);
  addResourceFactory(new AnimationSetResourceFactory);
  addResourceFactory(new ShaderResourceFactory);
  addResourceFactory(new ProgramResourceFactory);
  addResourceFactory(new MaterialResourceFactory);
  addResourceFactory(new AudioBankResourceFactory(audioSystem));

  // Exception-safe default definition-factory registration. A failure part
  // way through (e.g. allocation failure) must not leave this constructor's
  // earlier registrations in the process-wide registry: with no live manager
  // to own them they would leak and poison every future construction.
  //
  // The registry is empty when no manager is alive (every destructor clears
  // it), so "was empty at entry" means every entry present on failure was
  // added by this constructor and must be rolled back. If it was NOT empty,
  // a live manager owns it and the very first registration below throws as a
  // duplicate before this constructor registers anything, so rolling back is
  // both unnecessary and dangerous (it would delete the live manager's
  // factories) — the flag correctly gates the cleanup in that case too.
  bool const registryWasEmpty = Resource::msResourceDefinitionFactories.empty();

  try {
    addResourceDefinitionFactory(new TextFileDefaultDefinitionFactory);
    addResourceDefinitionFactory(new XmlFileDefaultDefinitionFactory);
    addResourceDefinitionFactory(new AnimationSetDefaultDefinitionFactory);
    addResourceDefinitionFactory(new ImageDefaultDefinitionFactory);
    addResourceDefinitionFactory(new ImageSetDefaultDefinitionFactory);
    addResourceDefinitionFactory(new ShaderDefaultDefinitionFactory);
    addResourceDefinitionFactory(new ProgramDefaultDefinitionFactory);
    addResourceDefinitionFactory(new MaterialDefaultDefinitionFactory);
    addResourceDefinitionFactory(new AudioBankDefaultDefinitionFactory);
  } catch (...) {
    if (registryWasEmpty) {
      for (auto& fItem : Resource::msResourceDefinitionFactories) {
        for (auto& mItem : fItem.second) {
          delete mItem.second;
        }
      }
      Resource::msResourceDefinitionFactories.clear();
    }
    throw;
  }
}

ResourceManager::~ResourceManager() {
  // Destroy resource locations
  for (auto const& record : mLocations) {
    delete record.location;
  }

  // Destroy all resources
  for (auto const& namespaceEntry : mResources) {
    auto const& resources = namespaceEntry.second;
    for (auto const& resourceEntry : resources) {
      auto const& resource = resourceEntry.second;
      if (resource->mLoaded) {
        _unloadResource(resource);
      }

      _destroyResource(resource);
    }
  }

  // Destroy all resource factories. Explicit clear() keeps the original
  // destruction order (resource factories before definition factories);
  // the unique_ptrs then have nothing left to delete at member destruction.
  mResourceFactories.clear();

  // Destroy all definition factories, then clear the process-wide static
  // registry. The registry outlives this manager (Resource instances look
  // factories up without holding a manager reference), but ownership follows
  // the manager's lifetime: leaving stale "already registered" entries behind
  // would make the next ResourceManager constructor throw and leak.
  for (auto const& fItem : Resource::msResourceDefinitionFactories) {
    for (auto const& mItem : fItem.second) {
      delete mItem.second;
    }
  }

  Resource::msResourceDefinitionFactories.clear();
}

vector<string> ResourceManager::sortResourcesByDependency(vector<string> const& resourceNames, map<string, vector<string>> dependencies) {
  vector<string> sorted, work;
  set<string> candidates;

  // Helper
  auto checkDeps = [&dependencies](auto const& entry) {
    for (auto item : dependencies) {
      auto const& [dep, list] = item;
      for (auto l : list) {
        if (l == entry) {
          return true;
        }
      }
    }

    return false;
  };

  // Initial set
  for (auto const& name : resourceNames) {
    if (checkDeps(name)) {
      candidates.insert(name);
    } else {
      work.push_back(name);
    }
  }

  // Process
  while (!work.empty()) {
    auto item = work.back();
    work.pop_back();
    sorted.push_back(item);

    dependencies.erase(item);

    // Check resources for dependencies
    vector<string> toRemove;
    for (auto c : candidates) {
      if (!checkDeps(c)) {
        toRemove.push_back(c);
      }
    }

    for (auto const& tr : toRemove) {
      work.push_back(tr);
      candidates.erase(tr);
    }
  }

  if (dependencies.size()) {
    throw ResourceDependencyException("Resources have cyclic dependencies.");
  }

  return sorted;
}

void ResourceManager::validateResourceDependencies() const {
  for (auto const& [namesp, records] : mNamespaces) {
    for (auto const& [name, record] : records) {
      string ownerQualifiedName = namesp.empty() ? name : namesp + "/" + name;

      for (auto const& dependency : record.dependentResources) {
        string dependencyNamespace, dependencyName;
        Resource::splitName(dependency.ref, namesp, &dependencyNamespace, &dependencyName);

        auto namespaceIt = mNamespaces.find(dependencyNamespace);
        if (namespaceIt == mNamespaces.end()) {
          throw ResourceDependencyException(format(
              "Resource '{}' depends on '{}', but namespace '{}' could not be found.",
              ownerQualifiedName, dependency.ref, dependencyNamespace));
        }

        if (!namespaceIt->second.contains(dependencyName)) {
          string dependencyQualifiedName = dependencyNamespace.empty()
                                               ? dependencyName
                                               : dependencyNamespace + "/" + dependencyName;
          throw ResourceDependencyException(format(
              "Resource '{}' depends on resource '{}', but that resource could not be found.",
              ownerQualifiedName, dependencyQualifiedName));
        }
      }
    }
  }
}

void ResourceManager::addResourceRecord(ResourceRecord const& record) {
  // Get namespace map
  auto nmspIt = mNamespaces.find(record.namesp);

  if (nmspIt == mNamespaces.end()) {
    nmspIt = mNamespaces.insert(make_pair(record.namesp, ResourceRecordMap())).first;
  }

  nmspIt->second[record.baseData.name] = record;
}

void ResourceManager::instantiateAllResources(bool create, bool load, ResourceCallback callback, bool rootResource) {
  validateResourceDependencies();

  // Get all resource names and dependencies
  vector<string> resourceNames;
  map<string, vector<string>> resourceDependencies;
  map<string, ResourceRecord const*> recordMap;
  for (auto const& [namesp, recordList] : mNamespaces) {
    for (auto const& [name, record] : recordList) {
      string qualifiedName = record.namesp == ""
                                 ? record.baseData.name
                                 : record.namesp + "/" + record.baseData.name;

      resourceNames.push_back(qualifiedName);

      for (auto const& drr : record.dependentResources) {
        // Parse ref to work out namespace and name
        string depNamesp, depName;
        Resource::splitName(drr.ref, record.namesp, &depNamesp, &depName);

        string depQualifiedName = depNamesp == ""
                                      ? depName
                                      : depNamesp + "/" + depName;

        auto it = resourceDependencies.insert(make_pair(depQualifiedName, vector<string>()));
        it.first->second.push_back(qualifiedName);
      }

      recordMap[qualifiedName] = &record;
    }
  }

  // Order all resources by dependencies, so we can be certain that all
  // dependent resources are instantiated before they are referenced.
  auto sortedResources = sortResourcesByDependency(resourceNames, resourceDependencies);

  // Instantiate records which do not already have a live Resource. Rescans
  // are additive: an existing Resource keeps its identity and state.
  for (auto const& resName : sortedResources) {
    auto const* record = recordMap.at(resName);
    auto namespaceIt = mResources.find(record->namesp);
    if (namespaceIt != mResources.end() &&
        namespaceIt->second.contains(record->baseData.name)) {
      continue;
    }
    instantiateResource(*record, create, load, callback, rootResource);
  }
}

ResourcePtr ResourceManager::instantiateResource(ResourceRecord const& record, bool create, bool load, ResourceCallback callback, bool rootResource) {
  // Find factory
  auto facIt = mResourceFactories.find(record.baseData.type);
  if (facIt == mResourceFactories.end()) {
    throw ResourceSystemException(format("Could not create resource '{}' because no factory for resource of type '{}' is registered.",
                                         record.baseData.name, record.baseData.type));
  }

  // Create
  string source = record.isComposite ? "" : (record.baseData.locationFound ? record.baseData.location : "");
  auto resource = facIt->second->createResource(record.baseData.name, record.namesp, source, record.baseData.tags, record.resourceLocation);
  auto resPtr = ResourcePtr(resource);

  mwLogger->info("Instantiated resource: " + resPtr->getQualifiedName());

  // Add dependent resources
  for (auto const& depResource : record.dependentResources) {
    // Get namespace for dependent resource
    string depNamesp, depResName;
    Resource::splitName(depResource.ref, record.namesp, &depNamesp, &depResName);

    auto acquiredRes = acquireResource(depResName, depNamesp);
    if (depResource.id != "") {
      resource->addDependentResource(depResource.id, acquiredRes);
    } else {
      resource->addDependentResource(acquiredRes);
    }
  }

  // Set definitions
  for (auto const& def : record.definitions) {
    resource->addDefinition(def.first, def.second);
  }

  if (callback) {
    callback(resPtr, ResourceState::Instantiated, rootResource);
  }

  if (create) {
    createResource(ResourcePtr(resource), callback, rootResource);
  }

  // Load
  if (load) {
    loadResource(ResourcePtr(resource), callback, rootResource);
  }

  // Add to lookup
  auto namespIt = mResources.find(record.namesp);
  if (namespIt == mResources.end()) {
    mResources[record.namesp] = ResourceMap();
  }

  mResources[record.namesp][record.baseData.name] = resPtr;
  return resPtr;
}

void ResourceManager::addResourceFactory(ResourceFactory* factory) {
  // Take ownership up front so the factory is freed on every exit path,
  // including the duplicate-registration throw below.
  unique_ptr<ResourceFactory> ownedFactory(factory);

  auto const& type = factory->getType();
  auto it = mResourceFactories.find(type);

  if (it != mResourceFactories.end()) {
    throw ResourceSystemException(format("Factory for resource of type '{}' is already registered.", type));
  }

  // emplace first: if it throws, ownedFactory still owns the factory.
  mResourceFactories.emplace(type, factory);
  ownedFactory.release();
}

void ResourceManager::addResourceLocationFactory(string const& type, ResourceLocationFactory factory) {
  auto it = mLocationFactories.find(type);

  if (it != mLocationFactories.end()) {
    throw ResourceSystemException(format("Factory for resource location of type '{}' is already registered.", type));
  }

  mLocationFactories[type] = factory;
}

void ResourceManager::addResourceDefinitionFactory(ResourceDefinitionFactory* factory) {
  // Take ownership up front so the factory is freed on every exit path,
  // including the duplicate-registration throw below.
  unique_ptr<ResourceDefinitionFactory> ownedFactory(factory);

  auto const& resType = factory->getResourceType();
  auto const& facType = factory->getFactoryType();

  auto it1 = Resource::msResourceDefinitionFactories.find(resType);
  if (it1 == Resource::msResourceDefinitionFactories.end()) {
    it1 = Resource::msResourceDefinitionFactories.insert(make_pair(resType, map<string, ResourceDefinitionFactory*>())).first;
  }

  auto& innerMap = it1->second;
  auto it2 = innerMap.find(facType);
  if (it2 != innerMap.end()) {
    throw ResourceSystemException(format("DefinitionFactory for resource type '{}', factory type '{}' is already registered.",
                                         resType, facType));
  }

  // emplace first: if it throws, ownedFactory still owns the factory.
  innerMap.emplace(facType, factory);
  ownedFactory.release();
}

void ResourceManager::addResourceLocation(string const& type, string const& location, string const& definitionFile) {
  auto it = mLocationFactories.find(type);

  if (it == mLocationFactories.end()) {
    throw ResourceSystemException(format("No factory for resource location of type '{}' is registered.", type));
  }

  ResourceLocationRecord record;
  record.location = it->second(location, definitionFile);
  record.scanned = false;

  mLocations.push_back(record);
}

void ResourceManager::addResources(string const& file) {
  WP_UNUSED(file);

  NOT_IMPLEMENTED_YET("adding resources from a file");
}

void ResourceManager::scanLocations(ResourceLocationCallback callback) {
  bool scannedLocation = false;
  for (auto& record : mLocations) {
    if (record.scanned) {
      continue;
    }

    mwLogger->info("Scanning resource location: " + record.location->getName());

    if (callback) {
      callback(record.location->getName(), ResourceLocationState::Unscanned);
    }

    record.location->scan();
    record.location->validateResourceDefinitions();

    // Add resource records to master list
    auto const& namespaceRecords = record.location->getNamespaceRecords();
    for (auto const& namespaceEntry : namespaceRecords) {
      auto const& namesp = namespaceEntry.second;
      for (auto const& resourceEntry : namesp.resourceRecords) {
        addResourceRecord(resourceEntry.second);
      }
    }

    record.scanned = true;
    scannedLocation = true;

    if (callback) {
      callback(record.location->getName(), ResourceLocationState::Scanned);
    }
  }

  // Dependency references are validated after every location has contributed
  // its records to the merged namespace registry. An ordinary repeat scan does
  // not replace already-instantiated resources.
  if (scannedLocation) {
    instantiateAllResources(false, false);
  }
}

void ResourceManager::rescanLocations(ResourceLocationCallback callback) {
  bool foundNewResource = false;
  for (auto& locationRecord : mLocations) {
    mwLogger->info(
        "Rescanning resource location: " + locationRecord.location->getName());

    if (callback) {
      callback(locationRecord.location->getName(), ResourceLocationState::Unscanned);
    }

    locationRecord.location->rescan();
    locationRecord.location->validateResourceDefinitions();

    // Merge only declarations which do not already have an instantiated
    // Resource. Existing records and Resources are deliberately immutable
    // through this operation.
    for (auto const& [namespaceName, namespaceRecord] :
         locationRecord.location->getNamespaceRecords()) {
      auto resourcesIt = mResources.find(namespaceName);
      for (auto const& [resourceName, resourceRecord] :
           namespaceRecord.resourceRecords) {
        if (resourcesIt != mResources.end() &&
            resourcesIt->second.contains(resourceName)) {
          continue;
        }
        addResourceRecord(resourceRecord);
        foundNewResource = true;
      }
    }

    locationRecord.scanned = true;
    if (callback) {
      callback(locationRecord.location->getName(), ResourceLocationState::Scanned);
    }
  }

  if (foundNewResource) {
    instantiateAllResources(false, false);
  }
}

void ResourceManager::addResource(ResourcePtr resource) {
  if (!resource) {
    throw ResourceSystemException("Cannot register a null Resource.");
  }
  if (resource->getName().empty() || resource->getName().find('/') != string::npos) {
    throw ResourceSystemException(
        "Programmatic Resource names must be non-empty and cannot contain '/'.");
  }

  auto& resources = mResources[resource->getNamespace()];
  if (resources.contains(resource->getName())) {
    throw ResourceSystemException(format(
        "Resource '{}' is already registered.", resource->getQualifiedName()));
  }

  mwLogger->info("Registered programmatic resource: " + resource->getQualifiedName());
  resources[resource->getName()] = move(resource);
}

ResourcePtr ResourceManager::getResource(string const& name, string const& namesp) {
  auto namespIt = mResources.find(namesp);

  if (namespIt == mResources.end()) {
    throw ResourceSystemException(format("Namespace '{}' could not be found.", namesp));
  }

  auto it = namespIt->second.find(name);

  if (it == namespIt->second.end()) {
    if (namesp.empty()) {
      throw ResourceSystemException(format("Resource '{}' could not be found.", name));
    } else {
      throw ResourceSystemException(format("Resource '{}/{}' could not be found.", namesp, name));
    }
  }

  return it->second;
}

ResourcePtr ResourceManager::getQualifiedResource(std::string const& name) {
  auto resParts = StringUtils::split(name, "/");
  auto numParts = resParts.size();

  if (numParts > 2) {
    throw ResourceSystemException(format("Resource '{}' is not a valid qualified name.", name));
  }

  auto resNamesp = numParts == 1 ? "" : resParts[0];
  auto resName = resParts[numParts - 1];

  return getResource(resName, resNamesp);
}

vector<ResourcePtr> ResourceManager::getResourcesByType(string const& type) {
  vector<ResourcePtr> resources;

  for (auto const& namespaceEntry : mResources) {
    auto const& namespaceResources = namespaceEntry.second;
    for (auto const& resourceEntry : namespaceResources) {
      auto const& resource = resourceEntry.second;
      if (resource->getType() == type) {
        resources.push_back(resource);
      }
    }
  }

  return resources;
}

vector<ResourcePtr> ResourceManager::getNamespaceResources(string const& namesp) {
  vector<ResourcePtr> resources;

  auto namespIt = mResources.find(namesp);

  if (namespIt == mResources.end()) {
    throw ResourceSystemException(format("Namespace '{}' could not be found.", namesp));
  }

  for (auto const& resourceEntry : namespIt->second) {
    resources.push_back(resourceEntry.second);
  }

  return resources;
}

vector<ResourcePtr> ResourceManager::getAllResources() {
  vector<ResourcePtr> resources;

  for (auto const& namespaceEntry : mResources) {
    auto const& namespaceResources = namespaceEntry.second;
    for (auto const& resourceEntry : namespaceResources) {
      resources.push_back(resourceEntry.second);
    }
  }

  return resources;
}

ResourcePtr ResourceManager::acquireResource(string const& name, string const& namesp) {
  auto res = getResource(name, namesp);
  acquireResource(res);
  return res;
}

void ResourceManager::acquireResource(ResourcePtr resource) {
  resource->mRefCount++;
}

void ResourceManager::releaseResource(ResourcePtr resource, ResourceCallback callback, bool rootResource) {
  // If it's already unloaded, we still want to fire the callback
  if (callback) {
    callback(resource, ResourceState::Releasing, rootResource);

    if (!resource->mLoaded) {
      if (resource->mCreated) {
        callback(resource, ResourceState::Created, rootResource);
      } else {
        callback(resource, ResourceState::Instantiated, rootResource);
      }
    }
  }

  // Handle the case where a resource has been loaded but nothing has actually needed
  // to acquire it.
  bool loadedButNeverAcquired = resource->mLoaded && resource->mRefCount == 0;
  bool readyForUnload = resource->mRefCount == 1;

  if (resource->mRefCount > 0) {
    resource->mRefCount--;
  }

  if (loadedButNeverAcquired || readyForUnload) {
    // Children
    for (auto childRes : resource->mDependentResourceList) {
      releaseResource(childRes, callback, false);
    }

    _unloadResource(resource, callback, rootResource);
  }

  if (callback) {
    callback(resource, ResourceState::Released, rootResource);
  }
}

bool ResourceManager::isResourceCreated(ResourcePtr resource) const {
  return resource->mCreated;
}

bool ResourceManager::isResourceLoaded(ResourcePtr resource) const {
  return resource->mLoaded;
}

void ResourceManager::_createResource(ResourcePtr resource, ResourceCallback callback, bool rootResource) {
  if (resource->mCreated) {
    return;
  }

  if (callback) {
    callback(resource, ResourceState::Creating, rootResource);
  }

  mwLogger->info("Creating resource: " + resource->getQualifiedName());

  DataStreamPtr dataPtr;
  if (resource->mSource != "") {
    dataPtr = make_shared<DataStream>(resource->mwLocation, resource->getSource(), resource->getNamespace());
    dataPtr->read();
  }

  resource->create(dataPtr, this);
  resource->mCreated = true;

  if (callback) {
    callback(resource, ResourceState::Created, rootResource);
  }
}

void ResourceManager::createResource(ResourcePtr resource, ResourceCallback callback, bool rootResource) {
  // Create dependent resources
  for (auto res : resource->mDependentResourceList) {
    createResource(res, callback, false);
  }

  _createResource(resource, callback, rootResource);
}

void ResourceManager::_destroyResource(ResourcePtr resource, ResourceCallback callback, bool rootResource) {
  if (resource->mLoaded) {
    throw ResourceException(resource.get(), "Resource cannot be destroyed before it has been unloaded.");
  }

  if (resource->mCreated) {
    if (callback) {
      callback(resource, ResourceState::Destroying, rootResource);
    }

    mwLogger->info("Destroying resource: " + resource->getQualifiedName());

    resource->destroy();
    resource->mCreated = false;

    if (callback) {
      callback(resource, ResourceState::Instantiated, rootResource);
    }
  }
}

void ResourceManager::_loadResource(ResourcePtr resource, ResourceCallback callback, bool rootResource) {
  if (!resource->mCreated) {
    throw ResourceException(resource.get(), "Resource cannot be loaded before it has been created.");
  }

  if (resource->mLoaded) {
    return;
  }

  if (callback) {
    callback(resource, ResourceState::Loading, rootResource);
  }

  mwLogger->info("Loading resource: " + resource->getQualifiedName());

  if (resource->load(mwRenderSystem, mwRenderResourceMgr)) {
    resource->mLoaded = true;
    if (callback) {
      callback(resource, ResourceState::Loaded, rootResource);
    }
  }
}

void ResourceManager::loadResource(ResourcePtr resource, ResourceCallback callback, bool rootResource) {
  // Load dependent resources
  for (auto res : resource->mDependentResourceList) {
    loadResource(res, callback, false);
  }

  _loadResource(resource, callback, rootResource);
}

void ResourceManager::_unloadResource(ResourcePtr resource, ResourceCallback callback, bool rootResource) {
  if (resource->mLoaded) {
    if (callback) {
      callback(resource, ResourceState::Unloading, rootResource);
    }

    mwLogger->info("Unloading resource: " + resource->getQualifiedName());

    if (resource->unload(mwRenderSystem, mwRenderResourceMgr)) {
      resource->mLoaded = false;
      if (callback) {
        callback(resource, ResourceState::Created, rootResource);
      }
    }
  }
}

void ResourceManager::createNamespaceResources(string const& namesp, ResourceCallback callback) {
  auto it = mResources.find(namesp);
  if (it != mResources.end()) {
    auto const& resourceRecord = it->second;
    for (auto const& resource : resourceRecord) {
      createResource(resource.second, callback, true);
    }
  } else {
    throw ResourceSystemException(format("Namespace '{}' could not be found.", namesp));
  }
}

void ResourceManager::createAllResources(ResourceCallback callback) {
  for (auto const& it : mResources) {
    createNamespaceResources(it.first, callback);
  }
}

void ResourceManager::createResources(vector<ResourcePtr> const& resources, ResourceCallback callback) {
  for (auto resource : resources) {
    createResource(resource, callback, true);
  }
}

void ResourceManager::destroyNamespaceResources(string const& namesp, ResourceCallback callback) {
  auto it = mResources.find(namesp);
  if (it != mResources.end()) {
    auto const& resourceRecord = it->second;
    for (auto const& resource : resourceRecord) {
      _destroyResource(resource.second, callback, true);
    }
  } else {
    throw ResourceSystemException(format("Namespace '{}' could not be found.", namesp));
  }
}

void ResourceManager::destroyAllResources(ResourceCallback callback) {
  for (auto const& it : mResources) {
    destroyNamespaceResources(it.first, callback);
  }
}

void ResourceManager::destroyResources(vector<ResourcePtr> const& resources, ResourceCallback callback) {
  for (auto resource : resources) {
    _destroyResource(resource, callback, true);
  }
}

void ResourceManager::loadNamespaceResources(string const& namesp, bool createFirst, ResourceCallback callback) {
  auto it = mResources.find(namesp);
  if (it != mResources.end()) {
    auto const& resourceRecord = it->second;
    for (auto const& resource : resourceRecord) {
      if (!resource.second->mCreated && createFirst) {
        createResource(resource.second, callback, true);
      }

      loadResource(resource.second, callback, true);
    }
  } else {
    throw ResourceSystemException(format("Namespace '{}' could not be found.", namesp));
  }
}

void ResourceManager::loadAllResources(bool createFirst, ResourceCallback callback) {
  for (auto const& it : mResources) {
    loadNamespaceResources(it.first, createFirst, callback);
  }
}

void ResourceManager::loadResources(vector<ResourcePtr> const& resources, bool createFirst, ResourceCallback callback) {
  for (auto resource : resources) {
    if (createFirst) {
      createResource(resource, callback, true);
    }

    loadResource(resource, callback, true);
  }
}

}  // namespace resourcesystem
}  // namespace application
}  // namespace WP_NAMESPACE