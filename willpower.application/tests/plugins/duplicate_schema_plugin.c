#include <stddef.h>
#include <stdint.h>

#include "willpower/application/resourcesystem/ResourceSchemaPlugin.h"

static const uint8_t duplicate_bundle[] = "{\"catalog\":{\"bundleFormatVersion\":\"1.0\",\"resourceManifestSchemaVersion\":\"1.0\",\"schemas\":[{\"kind\":\"resourceType\",\"resourceType\":\"Image\",\"factoryType\":null,\"schemaId\":\"https://schemas.example.test/plugins/duplicate-image.schema.json\",\"document\":\"schemas/duplicate-image.schema.json\",\"documentHash\":\"sha256:a73682866289599227eb5a8def845511d97a01dfabcaa77c55258c8fb951d948\"}]},\"documents\":[{\"document\":\"schemas/duplicate-image.schema.json\",\"contents\":\"{\\\"$schema\\\":\\\"http://json-schema.org/draft-07/schema#\\\",\\\"$id\\\":\\\"https://schemas.example.test/plugins/duplicate-image.schema.json\\\",\\\"type\\\":\\\"object\\\"}\"}]}";

uint32_t WP_RESOURCE_SCHEMA_PLUGIN_CALL willpower_resource_schema_plugin_v1(
    const wp_resource_schema_plugin_request_v1* request,
    wp_resource_schema_plugin_response_v1* response) {
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
  response->bundle_data = duplicate_bundle;
  response->bundle_size = sizeof(duplicate_bundle) - 1u;
  return WP_RESOURCE_SCHEMA_PLUGIN_SUCCESS;
}
