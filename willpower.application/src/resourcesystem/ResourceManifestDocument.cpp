#include "willpower/application/resourcesystem/ResourceManifestDocument.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

#include "utils/YamlReader.h"

#include "ResourceManifestValidator.h"

namespace wp::application::resourcesystem {
namespace {

bool isValidUtf8(std::string_view text) {
  for (std::size_t index = 0; index < text.size();) {
    auto const first = static_cast<unsigned char>(text[index]);
    if (first <= 0x7fU) {
      ++index;
      continue;
    }

    std::size_t length = 0;
    unsigned int value = 0;
    unsigned int minimum = 0;
    if ((first & 0xe0U) == 0xc0U) {
      length = 2;
      value = first & 0x1fU;
      minimum = 0x80U;
    } else if ((first & 0xf0U) == 0xe0U) {
      length = 3;
      value = first & 0x0fU;
      minimum = 0x800U;
    } else if ((first & 0xf8U) == 0xf0U) {
      length = 4;
      value = first & 0x07U;
      minimum = 0x10000U;
    } else {
      return false;
    }
    if (index + length > text.size()) return false;
    for (std::size_t offset = 1; offset < length; ++offset) {
      auto const continuation = static_cast<unsigned char>(text[index + offset]);
      if ((continuation & 0xc0U) != 0x80U) return false;
      value = (value << 6U) | (continuation & 0x3fU);
    }
    if (value < minimum || value > 0x10ffffU ||
        (value >= 0xd800U && value <= 0xdfffU)) {
      return false;
    }
    index += length;
  }
  return true;
}

ResourceManifestDiagnostic diagnostic(ResourceManifestDiagnosticKind kind,
                                      std::string const& sourceName,
                                      std::string message, int line = 0,
                                      int column = 0) {
  ResourceManifestDiagnostic result;
  result.kind = kind;
  result.manifestPath = sourceName;
  result.message = std::move(message);
  result.line = line;
  result.column = column;
  return result;
}

ResourceManifestValidationStatus validationStatus(
    ResourceManifestDocumentStatus status) {
  switch (status) {
    case ResourceManifestDocumentStatus::ready:
      return ResourceManifestValidationStatus::valid;
    case ResourceManifestDocumentStatus::yamlSyntaxError:
      return ResourceManifestValidationStatus::yamlSyntaxError;
    case ResourceManifestDocumentStatus::defensiveLimitExceeded:
      return ResourceManifestValidationStatus::defensiveLimitExceeded;
    case ResourceManifestDocumentStatus::filesystemError:
      return ResourceManifestValidationStatus::filesystemError;
  }
  return ResourceManifestValidationStatus::yamlSyntaxError;
}

std::string displayPath(std::filesystem::path const& path) {
  auto const utf8 = path.generic_u8string();
  return std::string(reinterpret_cast<char const*>(utf8.data()), utf8.size());
}

}  // namespace

struct ResourceManifestDocument::Impl {
  ResourceManifestDocumentStatus status = ResourceManifestDocumentStatus::ready;
  ResourceManifestLimits limits;
  std::string sourceName;
  std::unique_ptr<utils::YamlReader> reader;
  std::vector<ResourceManifestDiagnostic> diagnostics;
  std::vector<ResourceManifestDiagnostic> structuralDiagnostics;
};

ResourceManifestDocument::ResourceManifestDocument(
    std::unique_ptr<Impl> implementation)
    : mImplementation(std::move(implementation)) {}

ResourceManifestDocument::~ResourceManifestDocument() = default;
ResourceManifestDocument::ResourceManifestDocument(ResourceManifestDocument&&) noexcept =
    default;
ResourceManifestDocument& ResourceManifestDocument::operator=(
    ResourceManifestDocument&&) noexcept = default;

ResourceManifestDocument ResourceManifestDocument::parse(
    std::string_view yaml, std::string sourceName, ResourceManifestLimits limits) {
  auto implementation = std::make_unique<Impl>();
  implementation->limits = limits;
  implementation->sourceName = std::move(sourceName);

  if (yaml.size() > limits.maximumBytes) {
    implementation->status = ResourceManifestDocumentStatus::defensiveLimitExceeded;
    implementation->diagnostics.push_back(diagnostic(
        ResourceManifestDiagnosticKind::defensiveLimit,
        implementation->sourceName,
        "Resource Manifest size exceeds the defensive limit of " +
            std::to_string(limits.maximumBytes) + " bytes."));
    return ResourceManifestDocument(std::move(implementation));
  }
  if (!isValidUtf8(yaml)) {
    implementation->status = ResourceManifestDocumentStatus::yamlSyntaxError;
    implementation->diagnostics.push_back(diagnostic(
        ResourceManifestDiagnosticKind::yamlSyntax, implementation->sourceName,
        "Resource Manifest is not valid UTF-8."));
    return ResourceManifestDocument(std::move(implementation));
  }

  try {
    implementation->reader.reset(
        utils::YamlReader::fromString(std::string(yaml)));
    auto const inspection = implementation->reader->inspect(
        limits.maximumDepth, limits.maximumNodes);
    if (inspection.limitExceeded) {
      implementation->status =
          ResourceManifestDocumentStatus::defensiveLimitExceeded;
      implementation->diagnostics.push_back(diagnostic(
          ResourceManifestDiagnosticKind::defensiveLimit,
          implementation->sourceName, inspection.message, inspection.line,
          inspection.column));
      implementation->reader.reset();
    } else if (inspection.structuralProblem) {
      implementation->structuralDiagnostics.push_back(diagnostic(
          ResourceManifestDiagnosticKind::structuralValidation,
          implementation->sourceName, inspection.message, inspection.line,
          inspection.column));
    }
  } catch (std::exception const& error) {
    implementation->status = ResourceManifestDocumentStatus::yamlSyntaxError;
    implementation->diagnostics.push_back(diagnostic(
        ResourceManifestDiagnosticKind::yamlSyntax, implementation->sourceName,
        error.what()));
    implementation->reader.reset();
  }

  return ResourceManifestDocument(std::move(implementation));
}

ResourceManifestDocument ResourceManifestDocument::load(
    std::filesystem::path const& path, ResourceManifestLimits limits) {
  auto const sourceName = displayPath(path);
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) {
    auto implementation = std::make_unique<Impl>();
    implementation->limits = limits;
    implementation->sourceName = sourceName;
    implementation->status = ResourceManifestDocumentStatus::filesystemError;
    implementation->diagnostics.push_back(diagnostic(
        ResourceManifestDiagnosticKind::filesystem, sourceName,
        error ? "Could not inspect Resource Manifest: " + error.message()
              : "Resource Manifest is not a regular file."));
    return ResourceManifestDocument(std::move(implementation));
  }

  auto const size = std::filesystem::file_size(path, error);
  if (error) {
    auto implementation = std::make_unique<Impl>();
    implementation->limits = limits;
    implementation->sourceName = sourceName;
    implementation->status = ResourceManifestDocumentStatus::filesystemError;
    implementation->diagnostics.push_back(diagnostic(
        ResourceManifestDiagnosticKind::filesystem, sourceName,
        "Could not determine Resource Manifest size: " + error.message()));
    return ResourceManifestDocument(std::move(implementation));
  }
  if (size > limits.maximumBytes) {
    auto implementation = std::make_unique<Impl>();
    implementation->limits = limits;
    implementation->sourceName = sourceName;
    implementation->status =
        ResourceManifestDocumentStatus::defensiveLimitExceeded;
    implementation->diagnostics.push_back(diagnostic(
        ResourceManifestDiagnosticKind::defensiveLimit, sourceName,
        "Resource Manifest size exceeds the defensive limit of " +
            std::to_string(limits.maximumBytes) + " bytes."));
    return ResourceManifestDocument(std::move(implementation));
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    auto implementation = std::make_unique<Impl>();
    implementation->limits = limits;
    implementation->sourceName = sourceName;
    implementation->status = ResourceManifestDocumentStatus::filesystemError;
    implementation->diagnostics.push_back(diagnostic(
        ResourceManifestDiagnosticKind::filesystem, sourceName,
        "Could not open Resource Manifest."));
    return ResourceManifestDocument(std::move(implementation));
  }
  std::string contents((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
  if (!input.eof() && input.fail()) {
    auto implementation = std::make_unique<Impl>();
    implementation->limits = limits;
    implementation->sourceName = sourceName;
    implementation->status = ResourceManifestDocumentStatus::filesystemError;
    implementation->diagnostics.push_back(diagnostic(
        ResourceManifestDiagnosticKind::filesystem, sourceName,
        "Could not read Resource Manifest."));
    return ResourceManifestDocument(std::move(implementation));
  }
  return parse(contents, sourceName, limits);
}

ResourceManifestDocumentStatus ResourceManifestDocument::status() const noexcept {
  return mImplementation->status;
}

std::string const& ResourceManifestDocument::sourceName() const noexcept {
  return mImplementation->sourceName;
}

std::vector<ResourceManifestDiagnostic> const&
ResourceManifestDocument::diagnostics() const noexcept {
  return mImplementation->diagnostics;
}

ResourceManifestValidationResult ResourceManifestDocument::validate(
    ResourceSchemaCatalogSnapshot const& catalog) const {
  ResourceManifestValidationResult result;
  if (mImplementation->status != ResourceManifestDocumentStatus::ready) {
    result.status = validationStatus(mImplementation->status);
    result.diagnostics = mImplementation->diagnostics;
    return result;
  }

  if (!mImplementation->structuralDiagnostics.empty()) {
    result.status = ResourceManifestValidationStatus::structurallyInvalid;
    result.diagnostics = mImplementation->structuralDiagnostics;
    return result;
  }

  ResourceManifestValidator validator(catalog);
  auto failures = validator.validate(
      *mImplementation->reader, mImplementation->sourceName,
      (std::max)(std::size_t{1}, mImplementation->limits.maximumDiagnostics));
  if (failures.empty()) return result;

  result.status = ResourceManifestValidationStatus::structurallyInvalid;
  if (failures.size() > mImplementation->limits.maximumDiagnostics) {
    failures.resize(mImplementation->limits.maximumDiagnostics);
  }
  result.diagnostics.reserve(failures.size());
  for (auto& failure : failures) {
    ResourceManifestDiagnostic converted;
    converted.kind = ResourceManifestDiagnosticKind::structuralValidation;
    converted.manifestPath = std::move(failure.manifestPath);
    converted.resourceType = std::move(failure.resourceType);
    converted.factoryType = std::move(failure.factoryType);
    converted.resourceNamespace = std::move(failure.resourceNamespace);
    converted.resourceName = std::move(failure.resourceName);
    converted.instancePath = std::move(failure.instancePath);
    converted.message = std::move(failure.message);
    converted.line = failure.line;
    converted.column = failure.column;
    result.diagnostics.push_back(std::move(converted));
  }
  return result;
}

std::string ResourceManifestDocument::serializeCanonical() const {
  if (mImplementation->status != ResourceManifestDocumentStatus::ready ||
      !mImplementation->reader) {
    throw ResourceManifestDocumentException(
        "Cannot serialize a Resource Manifest that did not parse successfully.");
  }
  if (!mImplementation->structuralDiagnostics.empty()) {
    throw ResourceManifestDocumentException(
        "Cannot serialize a Resource Manifest with ambiguous mapping keys.");
  }
  try {
    return mImplementation->reader->canonicalYaml();
  } catch (std::exception const& error) {
    throw ResourceManifestDocumentException(error.what());
  }
}

}  // namespace wp::application::resourcesystem
