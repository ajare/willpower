#ifndef WILLPOWER_APPLICATION_RESOURCESYSTEM_RESOURCE_SCHEMA_PLUGIN_H
#define WILLPOWER_APPLICATION_RESOURCESYSTEM_RESOURCE_SCHEMA_PLUGIN_H

/*
 * Version 1 of the optional Resource Type schema-plugin C ABI.
 *
 * This header is intentionally usable from both C and C++. The boundary owns
 * no memory allocated by the other side and exposes no Willpower, C++, or
 * third-party-library types.
 */

#include <stdint.h>

#if defined(_WIN32)
#define WP_RESOURCE_SCHEMA_PLUGIN_CALL __cdecl
#if defined(WP_RESOURCE_SCHEMA_PLUGIN_BUILD)
#define WP_RESOURCE_SCHEMA_PLUGIN_EXPORT __declspec(dllexport)
#else
#define WP_RESOURCE_SCHEMA_PLUGIN_EXPORT
#endif
#elif defined(__GNUC__) && defined(WP_RESOURCE_SCHEMA_PLUGIN_BUILD)
#define WP_RESOURCE_SCHEMA_PLUGIN_CALL
#define WP_RESOURCE_SCHEMA_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define WP_RESOURCE_SCHEMA_PLUGIN_CALL
#define WP_RESOURCE_SCHEMA_PLUGIN_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define WP_RESOURCE_SCHEMA_PLUGIN_ABI_VERSION 1u
#define WP_RESOURCE_SCHEMA_PLUGIN_ENTRY_POINT_NAME "willpower_resource_schema_plugin_v1"

#define WP_RESOURCE_SCHEMA_PLUGIN_SUCCESS 0u
#define WP_RESOURCE_SCHEMA_PLUGIN_UNSUPPORTED_ABI 1u
#define WP_RESOURCE_SCHEMA_PLUGIN_UNSUPPORTED_BUNDLE_VERSION 2u
#define WP_RESOURCE_SCHEMA_PLUGIN_FAILURE 3u

/* A Resource Schema Bundle or Resource Manifest schema MAJOR.MINOR version. */
typedef struct wp_resource_schema_version {
  uint32_t major;
  uint32_t minor;
} wp_resource_schema_version;

/*
 * Host-owned input valid only for the duration of the entry-point call.
 * A version-1 plugin must check struct_size and abi_version before reading the
 * remaining fields. It selects one version from each host-owned version array.
 */
typedef struct wp_resource_schema_plugin_request_v1 {
  uint32_t struct_size;
  uint32_t abi_version;
  uint64_t bundle_format_version_count;
  const wp_resource_schema_version* bundle_format_versions;
  uint64_t resource_manifest_schema_version_count;
  const wp_resource_schema_version* resource_manifest_schema_versions;
} wp_resource_schema_plugin_request_v1;

struct wp_resource_schema_plugin_response_v1;

/*
 * Releases every plugin-owned pointer in a response. The host invokes this at
 * most once, while the library is still loaded, after copying bundle/error
 * bytes. A null callback means all returned pointers refer to immutable plugin
 * storage and remain valid until the library is unloaded.
 */
typedef void(WP_RESOURCE_SCHEMA_PLUGIN_CALL* wp_resource_schema_plugin_release_v1)(
    void* release_context);

/*
 * Host-zeroed output. The host initializes struct_size before the call.
 *
 * On SUCCESS, bundle_data is a UTF-8 JSON Plugin Bundle Container and remains
 * plugin-owned. The container has exactly a `catalog` object and `documents`
 * array; each document has exactly string fields `document` and `contents`.
 * `catalog` is the catalog.json object from Resource Schema Bundle format 1.0,
 * and contents is the exact schema-document byte sequence after JSON decoding.
 *
 * On failure, error_message is optional UTF-8 diagnostic data. No returned
 * pointer needs a trailing NUL because every byte sequence has an explicit
 * size. The plugin must not throw exceptions across the entry point.
 */
typedef struct wp_resource_schema_plugin_response_v1 {
  uint32_t struct_size;
  uint32_t abi_version;
  wp_resource_schema_version bundle_format_version;
  wp_resource_schema_version resource_manifest_schema_version;
  const uint8_t* bundle_data;
  uint64_t bundle_size;
  const uint8_t* error_message;
  uint64_t error_message_size;
  void* release_context;
  wp_resource_schema_plugin_release_v1 release;
} wp_resource_schema_plugin_response_v1;

typedef uint32_t(WP_RESOURCE_SCHEMA_PLUGIN_CALL* wp_resource_schema_plugin_entry_v1)(
    const wp_resource_schema_plugin_request_v1* request,
    wp_resource_schema_plugin_response_v1* response);

/* The one required, versioned exported symbol for ABI version 1 plugins. */
WP_RESOURCE_SCHEMA_PLUGIN_EXPORT uint32_t WP_RESOURCE_SCHEMA_PLUGIN_CALL
willpower_resource_schema_plugin_v1(
    const wp_resource_schema_plugin_request_v1* request,
    wp_resource_schema_plugin_response_v1* response);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WILLPOWER_APPLICATION_RESOURCESYSTEM_RESOURCE_SCHEMA_PLUGIN_H */
