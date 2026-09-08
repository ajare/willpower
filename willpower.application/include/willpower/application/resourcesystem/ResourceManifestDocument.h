#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "willpower/application/Platform.h"
#include "willpower/application/resourcesystem/ResourceSchemaCatalog.h"

namespace wp::application::resourcesystem {

// Defensive defaults shared by runtime-independent Resource Manifest tooling.
// Callers may lower them for tests or stricter environments.
struct ResourceManifestLimits {
  std::size_t maximumBytes = 64U * 1024U * 1024U;
  std::size_t maximumDepth = 128;
  std::size_t maximumNodes = 1'000'000;
  std::size_t maximumDiagnostics = 100;
};

enum class ResourceManifestDiagnosticKind {
  yamlSyntax,
  structuralValidation,
  defensiveLimit,
  filesystem
};

// This record intentionally contains no YAML, JSON, or validator types.
struct ResourceManifestDiagnostic {
  ResourceManifestDiagnosticKind kind =
      ResourceManifestDiagnosticKind::structuralValidation;
  std::string manifestPath;
  std::string resourceType;
  std::string factoryType;
  std::string resourceNamespace;
  std::string resourceName;
  std::string instancePath;
  std::string message;
  int line = 0;
  int column = 0;
};

enum class ResourceManifestDocumentStatus {
  ready,
  yamlSyntaxError,
  defensiveLimitExceeded,
  filesystemError
};

enum class ResourceManifestValidationStatus {
  valid,
  yamlSyntaxError,
  structurallyInvalid,
  defensiveLimitExceeded,
  filesystemError
};

struct ResourceManifestValidationResult {
  ResourceManifestValidationStatus status = ResourceManifestValidationStatus::valid;
  std::vector<ResourceManifestDiagnostic> diagnostics;

  [[nodiscard]] bool valid() const noexcept {
    return status == ResourceManifestValidationStatus::valid;
  }
};

class ResourceManifestDocumentException : public std::runtime_error {
 public:
  explicit ResourceManifestDocumentException(std::string const& message)
      : std::runtime_error(message) {}
};

// An owned, immutable parsed Resource Manifest document. Its implementation
// retains scalar and collection types for validation and canonical round-trip,
// while the public ABI remains independent of parser and validator libraries.
class WP_APPLICATION_API ResourceManifestDocument {
 public:
  [[nodiscard]] static ResourceManifestDocument parse(
      std::string_view yaml, std::string sourceName = {},
      ResourceManifestLimits limits = {});
  [[nodiscard]] static ResourceManifestDocument load(
      std::filesystem::path const& path, ResourceManifestLimits limits = {});

  ~ResourceManifestDocument();
  ResourceManifestDocument(ResourceManifestDocument&&) noexcept;
  ResourceManifestDocument& operator=(ResourceManifestDocument&&) noexcept;
  ResourceManifestDocument(ResourceManifestDocument const&) = delete;
  ResourceManifestDocument& operator=(ResourceManifestDocument const&) = delete;

  [[nodiscard]] ResourceManifestDocumentStatus status() const noexcept;
  [[nodiscard]] std::string const& sourceName() const noexcept;
  [[nodiscard]] std::vector<ResourceManifestDiagnostic> const& diagnostics() const noexcept;

  // Validation uses exactly the supplied immutable catalog snapshot. Schema
  // references are resolved only from that snapshot and never from a network.
  [[nodiscard]] ResourceManifestValidationResult validate(
      ResourceSchemaCatalogSnapshot const& catalog) const;

  // Requires a successfully parsed, bounded document. Output is UTF-8 YAML
  // with LF endings, two-space indentation, block collections, and one final
  // newline. Collection order and scalar types are retained deterministically.
  [[nodiscard]] std::string serializeCanonical() const;

 private:
  struct Impl;
  explicit ResourceManifestDocument(std::unique_ptr<Impl> implementation);
  std::unique_ptr<Impl> mImplementation;
};

}  // namespace wp::application::resourcesystem
