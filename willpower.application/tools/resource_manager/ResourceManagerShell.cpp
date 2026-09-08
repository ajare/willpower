#include "ResourceManagerShell.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <cerrno>
#include <cstring>
#endif

namespace resource_manager {
namespace {
namespace fs = std::filesystem;
using namespace wp::application::resourcesystem;

std::string displayPath(fs::path const& path) {
  auto const utf8 = path.generic_u8string();
  return std::string(reinterpret_cast<char const*>(utf8.data()), utf8.size());
}

bool accessibleDirectory(fs::path const& path, fs::path& canonical,
                         std::string& failure) {
  std::error_code error;
  if (!fs::is_directory(path, error)) {
    failure = "Base directory is not an accessible directory: " +
              displayPath(path);
    if (error) failure += ": " + error.message();
    return false;
  }
  canonical = fs::weakly_canonical(path, error);
  if (error) {
    failure = "Could not canonicalize base directory '" + displayPath(path) +
              "': " + error.message();
    return false;
  }
  return true;
}

std::string validationMessage(ResourceManifestValidationResult const& result) {
  if (result.diagnostics.empty()) return "Resource Manifest is not structurally valid.";
  auto const& diagnostic = result.diagnostics.front();
  std::ostringstream message;
  message << diagnostic.message;
  if (!diagnostic.instancePath.empty()) message << " at " << diagnostic.instancePath;
  if (diagnostic.line > 0) {
    message << " (line " << diagnostic.line;
    if (diagnostic.column > 0) message << ", column " << diagnostic.column;
    message << ')';
  }
  if (result.diagnostics.size() > 1) {
    message << " (and " << (result.diagnostics.size() - 1)
            << " more diagnostic(s))";
  }
  return message.str();
}

fs::path temporarySibling(fs::path const& destination) {
  auto const stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  auto parent = destination.parent_path();
  if (parent.empty()) parent = fs::current_path();
  auto const name = destination.filename().string();
  for (unsigned int attempt = 0; attempt != 100; ++attempt) {
    auto candidate = parent /
                     ("." + name + ".tmp-" + std::to_string(stamp) + "-" +
                      std::to_string(attempt));
    std::error_code error;
    if (!fs::exists(candidate, error)) return candidate;
  }
  throw std::runtime_error("Could not reserve a sibling temporary file.");
}

void replaceAtomically(fs::path const& temporary, fs::path const& destination) {
#if defined(_WIN32)
  if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    auto const code = GetLastError();
    throw std::system_error(static_cast<int>(code), std::system_category(),
                            "Could not atomically replace Resource Manifest");
  }
#else
  if (::rename(temporary.c_str(), destination.c_str()) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "Could not atomically replace Resource Manifest");
  }
#endif
}

void writeAtomically(fs::path const& destination, std::string const& bytes) {
  auto parent = destination.parent_path();
  if (parent.empty()) parent = fs::current_path();
  std::error_code error;
  if (!fs::is_directory(parent, error)) {
    throw std::runtime_error("Save directory is not accessible: " +
                             displayPath(parent));
  }

  auto temporary = temporarySibling(destination);
  try {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
      throw std::runtime_error("Could not create sibling temporary file '" +
                               displayPath(temporary) + "'.");
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    if (!output) {
      throw std::runtime_error("Could not write sibling temporary file '" +
                               displayPath(temporary) + "'.");
    }
    output.close();
    replaceAtomically(temporary, destination);
  } catch (...) {
    fs::remove(temporary, error);
    throw;
  }
}

std::string readBytes(fs::path const& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

}  // namespace

bool hasYamlExtension(fs::path const& path) {
  auto extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return extension == ".yaml" || extension == ".yml";
}

ManifestWorkspace::ManifestWorkspace()
    : mCatalog(ResourceSchemaCatalog::builtIn().snapshot()) {}

bool ManifestWorkspace::createNew(fs::path const& baseDirectory) {
  fs::path canonicalBase;
  std::string failure;
  if (!accessibleDirectory(baseDirectory, canonicalBase, failure)) {
    setFailure(std::move(failure));
    return false;
  }

  auto candidate = std::make_unique<ResourceManifestDocument>(
      ResourceManifestDocument::parse("Resources: {}\n", "Untitled Resource Manifest"));
  auto validation = candidate->validate(mCatalog);
  if (!validation.valid()) {
    mStructuralDiagnostics = std::move(validation.diagnostics);
    setFailure("Could not create an empty Resource Manifest: " +
               validationMessage(validation));
    return false;
  }

  mDocument = std::move(candidate);
  mPath.clear();
  mBaseDirectory = std::move(canonicalBase);
  mStructuralDiagnostics.clear();
  mOperationDiagnostic.clear();
  mDirty = true;
  return true;
}

bool ManifestWorkspace::open(fs::path const& manifestPath) {
  if (!hasYamlExtension(manifestPath)) {
    setFailure("Resource Manifest must use a .yaml or .yml extension.");
    return false;
  }

  auto candidate = std::make_unique<ResourceManifestDocument>(
      ResourceManifestDocument::load(manifestPath));
  auto validation = candidate->validate(mCatalog);
  if (!validation.valid()) {
    mStructuralDiagnostics = validation.diagnostics;
    setFailure("Could not open Resource Manifest '" + displayPath(manifestPath) +
               "': " + validationMessage(validation));
    return false;
  }

  std::error_code error;
  auto canonicalPath = fs::weakly_canonical(manifestPath, error);
  if (error) {
    setFailure("Could not canonicalize Resource Manifest path '" +
               displayPath(manifestPath) + "': " + error.message());
    return false;
  }
  fs::path canonicalBase;
  std::string failure;
  if (!accessibleDirectory(canonicalPath.parent_path(), canonicalBase, failure)) {
    setFailure(std::move(failure));
    return false;
  }

  mDocument = std::move(candidate);
  mPath = std::move(canonicalPath);
  mBaseDirectory = std::move(canonicalBase);
  mStructuralDiagnostics.clear();
  mOperationDiagnostic.clear();
  mDirty = false;
  return true;
}

bool ManifestWorkspace::save() {
  if (mPath.empty()) {
    setFailure("Save As is required for an unsaved Resource Manifest.");
    return false;
  }
  return saveTo(mPath);
}

bool ManifestWorkspace::saveAs(fs::path const& manifestPath) {
  if (!hasYamlExtension(manifestPath)) {
    setFailure("Resource Manifest must use a .yaml or .yml extension.");
    return false;
  }
  return saveTo(manifestPath);
}

bool ManifestWorkspace::saveTo(fs::path const& manifestPath) {
  if (!mDocument) {
    setFailure("No Resource Manifest is open.");
    return false;
  }
  if (!hasYamlExtension(manifestPath)) {
    setFailure("Resource Manifest must use a .yaml or .yml extension.");
    return false;
  }

  auto validation = mDocument->validate(mCatalog);
  if (!validation.valid()) {
    mStructuralDiagnostics = std::move(validation.diagnostics);
    setFailure("Resource Manifest is not structurally valid and cannot be saved.");
    return false;
  }

  try {
    auto canonical = mDocument->serializeCanonical();
    writeAtomically(manifestPath, canonical);

    auto saved = std::make_unique<ResourceManifestDocument>(
        ResourceManifestDocument::load(manifestPath));
    auto savedValidation = saved->validate(mCatalog);
    if (!savedValidation.valid() || saved->serializeCanonical() != canonical) {
      throw std::runtime_error(
          "Saved Resource Manifest failed canonical round-trip validation.");
    }
    std::error_code error;
    auto canonicalPath = fs::weakly_canonical(manifestPath, error);
    if (error) canonicalPath = fs::absolute(manifestPath).lexically_normal();
    mDocument = std::move(saved);
    mPath = std::move(canonicalPath);
    mStructuralDiagnostics.clear();
    mOperationDiagnostic.clear();
    mDirty = false;
    return true;
  } catch (std::exception const& exception) {
    setFailure("Could not save Resource Manifest '" + displayPath(manifestPath) +
               "': " + exception.what());
    return false;
  }
}

void ManifestWorkspace::reportFailure(std::string message) {
  setFailure(std::move(message));
}

bool ManifestWorkspace::hasDocument() const noexcept { return mDocument != nullptr; }
bool ManifestWorkspace::hasPath() const noexcept { return !mPath.empty(); }
bool ManifestWorkspace::dirty() const noexcept { return mDirty; }
fs::path const& ManifestWorkspace::path() const noexcept { return mPath; }
fs::path const& ManifestWorkspace::baseDirectory() const noexcept {
  return mBaseDirectory;
}
std::string const& ManifestWorkspace::operationDiagnostic() const noexcept {
  return mOperationDiagnostic;
}
std::vector<ResourceManifestDiagnostic> const&
ManifestWorkspace::structuralDiagnostics() const noexcept {
  return mStructuralDiagnostics;
}
std::string ManifestWorkspace::canonicalYaml() const {
  return mDocument ? mDocument->serializeCanonical() : std::string{};
}
void ManifestWorkspace::setFailure(std::string message) {
  mOperationDiagnostic = std::move(message);
}

bool runDocumentTests(std::string* failure) {
  auto fail = [&](std::string message) {
    if (failure) *failure = std::move(message);
    return false;
  };
  auto const unique = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  auto root = fs::temp_directory_path() /
              ("willpower-resource-manager-document-tests-" + unique);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
  } cleanup{root};

  try {
    fs::create_directories(root / "assets");
    ManifestWorkspace workspace;
    if (!workspace.createNew(root / "assets") || !workspace.hasDocument() ||
        workspace.hasPath() || !workspace.dirty()) {
      return fail("New did not create a valid empty unsaved Resource Manifest.");
    }
    if (workspace.canonicalYaml() != "Resources:\n  {}\n") {
      return fail("New Resource Manifest did not have canonical empty content.");
    }
    auto yamlPath = root / "manifest.YML";
    if (!workspace.saveAs(yamlPath) || !workspace.hasPath() || workspace.dirty()) {
      return fail("Save As did not save a YAML Resource Manifest.");
    }
    if (readBytes(yamlPath) != "Resources:\n  {}\n") {
      return fail("Save As did not emit canonical Resource Manifest YAML.");
    }

    fs::path originalPath = workspace.path();
    auto invalidPath = root / "invalid.yaml";
    std::ofstream(invalidPath) << "Resources:\n  unexpected: true\n";
    if (workspace.open(invalidPath) || workspace.path() != originalPath ||
        workspace.operationDiagnostic().empty()) {
      return fail("Structurally invalid Open replaced the current document.");
    }
    auto multiDocumentPath = root / "multi-document.yaml";
    std::ofstream(multiDocumentPath) << "Resources: {}\n---\nResources: {}\n";
    if (workspace.open(multiDocumentPath) || workspace.path() != originalPath) {
      return fail("Open accepted multiple YAML documents or replaced the current document.");
    }
    if (workspace.saveAs(root / "manifest.txt") ||
        fs::exists(root / "manifest.txt")) {
      return fail("Save As accepted a non-YAML extension.");
    }

    std::ofstream(yamlPath, std::ios::binary | std::ios::trunc)
        << "Resources:\n  Resource:\n    type: TextFile\n    name: notes\n"
           "    location: notes.txt\n";
    if (!workspace.open(yamlPath) || workspace.dirty()) {
      return fail("Open did not accept a structurally valid single-document manifest.");
    }
    auto expected = workspace.canonicalYaml();
    if (!workspace.save() || readBytes(yamlPath) != expected) {
      return fail("Save did not atomically replace the destination with canonical YAML.");
    }
    for (auto const& entry : fs::directory_iterator(root)) {
      if (entry.path().filename().string().find(".tmp-") != std::string::npos) {
        return fail("Atomic Save left a sibling temporary file behind.");
      }
    }
    return true;
  } catch (std::exception const& exception) {
    return fail(exception.what());
  }
}

}  // namespace resource_manager
