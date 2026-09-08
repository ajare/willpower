#include "willpower/application/resourcesystem/ResourceSchemaCatalog.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "willpower/application/resourcesystem/ResourceSchemaPlugin.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace wp::application::resourcesystem {
namespace {
using Json = nlohmann::json;

constexpr std::uint64_t maximumPluginBundleSize = 64U * 1024U * 1024U;
constexpr wp_resource_schema_version supportedBundleVersions[]{{1U, 0U}};
constexpr wp_resource_schema_version supportedManifestVersions[]{{1U, 0U}};

std::string pathText(std::filesystem::path const& path) { return path.generic_string(); }

#if defined(_WIN32)
std::string windowsError(DWORD code) {
  char* buffer = nullptr;
  auto const size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, reinterpret_cast<char*>(&buffer), 0, nullptr);
  std::string message = size == 0 || buffer == nullptr ? "Windows error " + std::to_string(code)
                                                       : std::string(buffer, size);
  if (buffer != nullptr) LocalFree(buffer);
  while (!message.empty() &&
         (message.back() == '\r' || message.back() == '\n' || message.back() == ' '))
    message.pop_back();
  return message;
}
#endif

class SharedLibrary {
 public:
  explicit SharedLibrary(std::filesystem::path const& path) {
#if defined(_WIN32)
    mHandle = LoadLibraryExW(path.c_str(), nullptr,
                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                 LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (mHandle == nullptr) {
      throw ResourceSchemaCatalogException("Cannot load Resource Type schema plugin '" +
                                           pathText(path) + "': " + windowsError(GetLastError()) +
                                           ".");
    }
#else
    mHandle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (mHandle == nullptr) {
      auto const* error = dlerror();
      throw ResourceSchemaCatalogException(
          "Cannot load Resource Type schema plugin '" + pathText(path) + "': " +
          (error == nullptr ? std::string("unknown dynamic-loader error") : std::string(error)) +
          ".");
    }
#endif
  }

  ~SharedLibrary() { close(); }
  SharedLibrary(SharedLibrary const&) = delete;
  SharedLibrary& operator=(SharedLibrary const&) = delete;

  wp_resource_schema_plugin_entry_v1 entryPoint() const {
#if defined(_WIN32)
    auto const address = GetProcAddress(mHandle, WP_RESOURCE_SCHEMA_PLUGIN_ENTRY_POINT_NAME);
    if (address == nullptr) return nullptr;
    return reinterpret_cast<wp_resource_schema_plugin_entry_v1>(address);
#else
    dlerror();
    auto* address = dlsym(mHandle, WP_RESOURCE_SCHEMA_PLUGIN_ENTRY_POINT_NAME);
    if (dlerror() != nullptr || address == nullptr) return nullptr;
    wp_resource_schema_plugin_entry_v1 result = nullptr;
    static_assert(sizeof(result) == sizeof(address));
    std::memcpy(&result, &address, sizeof(result));
    return result;
#endif
  }

 private:
  void close() noexcept {
    if (mHandle == nullptr) return;
#if defined(_WIN32)
    FreeLibrary(mHandle);
#else
    dlclose(mHandle);
#endif
    mHandle = nullptr;
  }

#if defined(_WIN32)
  HMODULE mHandle = nullptr;
#else
  void* mHandle = nullptr;
#endif
};

class ResponseLifetime {
 public:
  explicit ResponseLifetime(wp_resource_schema_plugin_response_v1& response)
      : mResponse(response) {}
  ~ResponseLifetime() { release(); }
  ResponseLifetime(ResponseLifetime const&) = delete;
  ResponseLifetime& operator=(ResponseLifetime const&) = delete;

  void release() noexcept {
    if (mReleased) return;
    mReleased = true;
    if (mResponse.struct_size >= sizeof(wp_resource_schema_plugin_response_v1) &&
        mResponse.release != nullptr) {
      mResponse.release(mResponse.release_context);
    }
  }

 private:
  wp_resource_schema_plugin_response_v1& mResponse;
  bool mReleased = false;
};

bool sameVersion(wp_resource_schema_version const& left,
                 wp_resource_schema_version const& right) {
  return left.major == right.major && left.minor == right.minor;
}

std::string versionText(wp_resource_schema_version const& version) {
  return std::to_string(version.major) + "." + std::to_string(version.minor);
}

std::string responseMessage(wp_resource_schema_plugin_response_v1 const& response) {
  if (response.struct_size < sizeof(wp_resource_schema_plugin_response_v1) ||
      response.error_message == nullptr || response.error_message_size == 0U ||
      response.error_message_size > maximumPluginBundleSize) {
    return {};
  }
  return std::string(reinterpret_cast<char const*>(response.error_message),
                     static_cast<std::size_t>(response.error_message_size));
}

void requireExactFields(Json const& object, std::set<std::string> const& fields,
                        std::string const& context) {
  if (!object.is_object())
    throw ResourceSchemaCatalogException(context + " must be a JSON object.");
  std::set<std::string> actual;
  for (auto const& [name, value] : object.items()) {
    static_cast<void>(value);
    actual.insert(name);
  }
  if (actual == fields) return;
  for (auto const& field : fields) {
    if (!actual.contains(field))
      throw ResourceSchemaCatalogException(context + " is missing required field '" + field +
                                           "'.");
  }
  for (auto const& field : actual) {
    if (!fields.contains(field))
      throw ResourceSchemaCatalogException(context + " has unknown field '" + field + "'.");
  }
}

ResourceSchemaBundle parsePluginContainer(std::string const& bytes,
                                          std::filesystem::path const& pluginPath) {
  Json container;
  auto const context = "Resource Type schema plugin '" + pathText(pluginPath) + "' container";
  try {
    container = Json::parse(bytes);
  } catch (Json::exception const& error) {
    throw ResourceSchemaCatalogException(context + ": malformed JSON: " + error.what());
  }
  requireExactFields(container, {"catalog", "documents"}, context);
  if (!container.at("catalog").is_object())
    throw ResourceSchemaCatalogException(context + ": 'catalog' must be an object.");
  if (!container.at("documents").is_array())
    throw ResourceSchemaCatalogException(context + ": 'documents' must be an array.");

  ResourceSchemaBundle bundle;
  bundle.catalogJson = container.at("catalog").dump();
  std::size_t index = 0;
  for (auto const& document : container.at("documents")) {
    auto const documentContext = context + " document " + std::to_string(index++);
    requireExactFields(document, {"document", "contents"}, documentContext);
    if (!document.at("document").is_string() || !document.at("contents").is_string()) {
      throw ResourceSchemaCatalogException(documentContext +
                                           ": 'document' and 'contents' must be strings.");
    }
    bundle.documents.push_back(
        {document.at("document").get<std::string>(), document.at("contents").get<std::string>()});
  }
  return bundle;
}

std::filesystem::path explicitPluginPath(std::filesystem::path const& supplied) {
  if (supplied.empty())
    throw ResourceSchemaCatalogException("Resource Type schema plugin path cannot be empty.");
  std::error_code error;
  auto result = std::filesystem::absolute(supplied, error).lexically_normal();
  if (error) {
    throw ResourceSchemaCatalogException("Cannot resolve Resource Type schema plugin path '" +
                                         pathText(supplied) + "': " + error.message() + ".");
  }
  if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(result, error)) || error) {
    throw ResourceSchemaCatalogException("Missing or non-regular Resource Type schema plugin '" +
                                         pathText(result) + "'.");
  }
  return result;
}
}  // namespace

ResourceSchemaBundle ResourceSchemaCatalog::readPluginBundle(
    std::filesystem::path const& pluginPath) {
  auto const path = explicitPluginPath(pluginPath);
  std::string copiedBundle;
  {
    SharedLibrary library(path);
    auto const entry = library.entryPoint();
    if (entry == nullptr) {
      throw ResourceSchemaCatalogException("Resource Type schema plugin '" + pathText(path) +
                                           "' is missing required symbol '" +
                                           WP_RESOURCE_SCHEMA_PLUGIN_ENTRY_POINT_NAME + "'.");
    }

    wp_resource_schema_plugin_request_v1 request{};
    request.struct_size = sizeof(request);
    request.abi_version = WP_RESOURCE_SCHEMA_PLUGIN_ABI_VERSION;
    request.bundle_format_version_count = std::size(supportedBundleVersions);
    request.bundle_format_versions = supportedBundleVersions;
    request.resource_manifest_schema_version_count = std::size(supportedManifestVersions);
    request.resource_manifest_schema_versions = supportedManifestVersions;

    wp_resource_schema_plugin_response_v1 response{};
    response.struct_size = sizeof(response);
    ResponseLifetime lifetime(response);
    auto const status = entry(&request, &response);

    if (response.struct_size != sizeof(wp_resource_schema_plugin_response_v1)) {
      throw ResourceSchemaCatalogException(
          "Resource Type schema plugin '" + pathText(path) +
          "' returned an incompatible ABI response structure size " +
          std::to_string(response.struct_size) + ".");
    }
    if (status != WP_RESOURCE_SCHEMA_PLUGIN_SUCCESS) {
      std::string reason;
      if (status == WP_RESOURCE_SCHEMA_PLUGIN_UNSUPPORTED_ABI)
        reason = "does not support Resource Schema Plugin ABI version " +
                 std::to_string(WP_RESOURCE_SCHEMA_PLUGIN_ABI_VERSION);
      else if (status == WP_RESOURCE_SCHEMA_PLUGIN_UNSUPPORTED_BUNDLE_VERSION)
        reason = "does not support bundle format 1.0 and Resource Manifest schema 1.0";
      else
        reason = "reported failure status " + std::to_string(status);
      auto const detail = responseMessage(response);
      if (!detail.empty()) reason += ": " + detail;
      throw ResourceSchemaCatalogException("Resource Type schema plugin '" + pathText(path) +
                                           "' " + reason + ".");
    }
    if (response.abi_version != WP_RESOURCE_SCHEMA_PLUGIN_ABI_VERSION) {
      throw ResourceSchemaCatalogException("Resource Type schema plugin '" + pathText(path) +
                                           "' returned incompatible ABI version " +
                                           std::to_string(response.abi_version) + " (supported: 1).");
    }
    if (!sameVersion(response.bundle_format_version, supportedBundleVersions[0]) ||
        !sameVersion(response.resource_manifest_schema_version,
                     supportedManifestVersions[0])) {
      throw ResourceSchemaCatalogException(
          "Resource Type schema plugin '" + pathText(path) +
          "' selected unsupported bundle versions " +
          versionText(response.bundle_format_version) + " / " +
          versionText(response.resource_manifest_schema_version) + " (supported: 1.0 / 1.0).");
    }
    if (response.bundle_data == nullptr || response.bundle_size == 0U) {
      throw ResourceSchemaCatalogException("Resource Type schema plugin '" + pathText(path) +
                                           "' returned no bundle bytes.");
    }
    if (response.bundle_size > maximumPluginBundleSize ||
        response.bundle_size > std::numeric_limits<std::size_t>::max()) {
      throw ResourceSchemaCatalogException("Resource Type schema plugin '" + pathText(path) +
                                           "' bundle exceeds the 64 MiB loader limit.");
    }
    copiedBundle.assign(reinterpret_cast<char const*>(response.bundle_data),
                        static_cast<std::size_t>(response.bundle_size));
    lifetime.release();
  }  // The plugin is unloaded before its copied bytes are parsed or merged.

  return parsePluginContainer(copiedBundle, path);
}

void ResourceSchemaCatalog::addPlugin(std::filesystem::path const& pluginPath) {
  auto bundle = readPluginBundle(pluginPath);
  addBundle(bundle);
}

}  // namespace wp::application::resourcesystem
