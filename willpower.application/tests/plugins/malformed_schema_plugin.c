#include <stddef.h>
#include <stdint.h>

#include "willpower/application/resourcesystem/ResourceSchemaPlugin.h"

uint32_t WP_RESOURCE_SCHEMA_PLUGIN_CALL willpower_resource_schema_plugin_v1(
    const wp_resource_schema_plugin_request_v1* request,
    wp_resource_schema_plugin_response_v1* response) {
  static const uint8_t malformed[] = "{not a Resource Schema Plugin Bundle Container";
  if (request == NULL || response == NULL ||
      request->struct_size < sizeof(wp_resource_schema_plugin_request_v1) ||
      response->struct_size < sizeof(wp_resource_schema_plugin_response_v1))
    return WP_RESOURCE_SCHEMA_PLUGIN_FAILURE;
  response->struct_size = (uint32_t)sizeof(*response);
  response->abi_version = WP_RESOURCE_SCHEMA_PLUGIN_ABI_VERSION;
  response->bundle_format_version.major = 1u;
  response->bundle_format_version.minor = 0u;
  response->resource_manifest_schema_version.major = 1u;
  response->resource_manifest_schema_version.minor = 0u;
  response->bundle_data = malformed;
  response->bundle_size = sizeof(malformed) - 1u;
  return WP_RESOURCE_SCHEMA_PLUGIN_SUCCESS;
}
