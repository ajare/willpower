#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "willpower/application/Platform.h"

namespace wp::application::resourcesystem {

// Values used by the language-neutral Resource Schema Bundle format. These
// types deliberately contain no JSON parser, YAML, validator, or generated
// catalog implementation types.
enum class ResourceSchemaKind { manifest, resourceType, dependency };

struct ResourceSchemaKey {
  std::string resourceType;
  // An empty factory type denotes the Resource Type's default factory.
  std::string factoryType;

  friend bool operator==(ResourceSchemaKey const&, ResourceSchemaKey const&) = default;
};

struct ResourceSchema {
  ResourceSchemaKind kind = ResourceSchemaKind::dependency;
  std::string resourceType;
  std::string factoryType;
  std::string schemaId;
  std::string document;
  std::string documentHash;
  std::string contents;
};

struct ResourceSchemaBundleDocument {
  // Safe, bundle-relative path, normally "schemas/<name>.schema.json".
  std::string document;
  std::string contents;
};

struct ResourceSchemaBundle {
  std::string catalogJson;
  std::vector<ResourceSchemaBundleDocument> documents;
};

// Hard security boundaries for untrusted, entirely local schema bundles.
// Network resolution is never attempted by the catalog.
struct ResourceSchemaCatalogLimits {
  static constexpr std::size_t maximumCatalogBytes = 1U * 1024U * 1024U;
  static constexpr std::size_t maximumDocumentBytes = 4U * 1024U * 1024U;
  static constexpr std::size_t maximumAggregateBytes = 16U * 1024U * 1024U;
  static constexpr std::size_t maximumDocuments = 4096U;
};

class ResourceSchemaCatalogException : public std::runtime_error {
 public:
  explicit ResourceSchemaCatalogException(std::string const& message)
      : std::runtime_error(message) {}
};

class WP_APPLICATION_API ResourceSchemaCatalogSnapshot {
 public:
  ResourceSchemaCatalogSnapshot();

  // Catalog order is deterministic and independent of bundle registration order.
  [[nodiscard]] std::vector<ResourceSchema> const& entries() const noexcept;

  // Exact lookup never applies default-factory fallback.
  [[nodiscard]] ResourceSchema const* findExact(ResourceSchemaKey const& key) const noexcept;

  // Looks for the exact key, then (when factoryType is non-empty) the Resource
  // Type's default-factory entry.
  [[nodiscard]] ResourceSchema const* find(ResourceSchemaKey const& key) const noexcept;
  [[nodiscard]] ResourceSchema const* findBySchemaId(std::string const& schemaId) const noexcept;

  // Returns an owned, deterministic Resource Schema Bundle. The returned data
  // remains valid independently of both this snapshot and all original inputs.
  [[nodiscard]] ResourceSchemaBundle exportBundle() const;
  void exportBundle(std::filesystem::path const& directory) const;

  // Replaces any input manifest entries with one deterministic root schema
  // that dispatches every registered Resource Type and factory. The result is
  // a complete, self-contained bundle suitable for YAML language servers.
  [[nodiscard]] ResourceSchemaBundle exportComposedBundle(
      std::string const& rootSchemaId) const;
  void exportComposedBundle(std::filesystem::path const& directory,
                            std::string const& rootSchemaId) const;

 private:
  explicit ResourceSchemaCatalogSnapshot(
      std::shared_ptr<std::vector<ResourceSchema> const> entries);
  std::shared_ptr<std::vector<ResourceSchema> const> mEntries;

  friend class ResourceSchemaCatalog;
};

// A mutable catalog publisher. addBundle/addBundles validate complete candidate
// states and publish them atomically; an existing snapshot is immutable and is
// never changed by later registrations.
class WP_APPLICATION_API ResourceSchemaCatalog {
 public:
  // Constructs an empty catalog. Use builtIn() to start with Willpower schemas.
  ResourceSchemaCatalog();
  explicit ResourceSchemaCatalog(ResourceSchemaBundle const& bundle);
  explicit ResourceSchemaCatalog(std::filesystem::path const& bundleDirectory);
  ~ResourceSchemaCatalog();

  ResourceSchemaCatalog(ResourceSchemaCatalog const&) = delete;
  ResourceSchemaCatalog& operator=(ResourceSchemaCatalog const&) = delete;
  ResourceSchemaCatalog(ResourceSchemaCatalog&&) noexcept;
  ResourceSchemaCatalog& operator=(ResourceSchemaCatalog&&) noexcept;

  [[nodiscard]] static ResourceSchemaCatalog builtIn();
  [[nodiscard]] static ResourceSchemaBundle readBundle(
      std::filesystem::path const& bundleDirectory);
  // Loads exactly the supplied shared-library path, negotiates C ABI version
  // 1, copies its Plugin Bundle Container, releases plugin-owned storage, and
  // unloads the library before returning the owned bundle.
  [[nodiscard]] static ResourceSchemaBundle readPluginBundle(
      std::filesystem::path const& pluginPath);

  void addBundle(ResourceSchemaBundle const& bundle);
  void addBundle(std::filesystem::path const& bundleDirectory);
  // Plugin discovery is explicit and schema-only. It neither searches for
  // libraries nor constructs Resource factories or application services.
  void addPlugin(std::filesystem::path const& pluginPath);
  // A batch is one mutation: readers observe either all bundles or none.
  void addBundles(std::span<ResourceSchemaBundle const> bundles);

  [[nodiscard]] ResourceSchemaCatalogSnapshot snapshot() const;

 private:
  struct Impl;
  explicit ResourceSchemaCatalog(std::unique_ptr<Impl> implementation);
  std::unique_ptr<Impl> mImplementation;
};

// Reusable main entry point for schema-only application exporters. Downstream
// code can construct a catalog, register application-owned bundles, and pass it
// here without constructing any Resource or runtime service. Returns a process
// exit code and writes JSON results/diagnostics to stdout/stderr.
WP_APPLICATION_API int runResourceSchemaExporter(ResourceSchemaCatalog& catalog, int argc,
                                                 char const* const* argv);

}  // namespace wp::application::resourcesystem
