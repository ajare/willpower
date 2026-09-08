#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "willpower/application/resourcesystem/ResourceSchemaPlugin.h"

static const uint8_t sample_bundle[] =
    "{\"catalog\":{\"bundleFormatVersion\":\"1.0\",\"resourceManifestSchemaVersion\":\"1.0\",\"schemas\":[{\"kind\":\"resourceType\",\"resourceType\":\"PluginWidget\",\"factoryType\":null,\"schemaId\":\"https://schemas.example.test/plugins/plugin-widget.schema.json\",\"document\":\"schemas/plugin-widget.schema.json\",\"documentHash\":\"sha256:aa7d7622f1ced219ef2661e2388b8d28a7a2e845e5bc03dfc3660e8337589c8e\"},{\"kind\":\"resourceType\",\"resourceType\":\"PluginWidget\",\"factoryType\":\"GlowFactory\",\"schemaId\":\"https://schemas.example.test/plugins/plugin-widget-glow.schema.json\",\"document\":\"schemas/plugin-widget-glow.schema.json\",\"documentHash\":\"sha256:270df697126faf9f429b18e7e4d04e98886075e9c9bbe062b91e11b8ab459c84\"}]},\"documents\":[{\"document\":\"schemas/plugin-widget.schema.json\",\"contents\":\"{\\\"$schema\\\":\\\"http://json-schema.org/draft-07/schema#\\\",\\\"$id\\\":\\\"https://schemas.example.test/plugins/plugin-widget.schema.json\\\",\\\"type\\\":\\\"object\\\",\\\"allOf\\\":[{\\\"$ref\\\":\\\"https://schemas.willpower.dev/resource-manifest/resource.schema.json#/definitions/resource\\\"}],\\\"properties\\\":{\\\"type\\\":{\\\"const\\\":\\\"PluginWidget\\\"}}}\"},{\"document\":\"schemas/plugin-widget-glow.schema.json\",\"contents\":\"{\\\"$schema\\\":\\\"http://json-schema.org/draft-07/schema#\\\",\\\"$id\\\":\\\"https://schemas.example.test/plugins/plugin-widget-glow.schema.json\\\",\\\"type\\\":\\\"object\\\",\\\"allOf\\\":[{\\\"$ref\\\":\\\"https://schemas.willpower.dev/resource-manifest/resource.schema.json#/definitions/definition\\\"}],\\\"properties\\\":{\\\"factory\\\":{\\\"const\\\":\\\"GlowFactory\\\"},\\\"glow\\\":{\\\"type\\\":\\\"number\\\",\\\"minimum\\\":0}},\\\"required\\\":[\\\"factory\\\",\\\"glow\\\"]}\"}]}";

static int supports_version(const wp_resource_schema_version* versions, uint64_t count) {
  uint64_t index;
  if (versions == NULL) return 0;
  for (index = 0; index < count; ++index) {
    if (versions[index].major == 1u && versions[index].minor == 0u) return 1;
  }
  return 0;
}

static void WP_RESOURCE_SCHEMA_PLUGIN_CALL release_response(void* context) { free(context); }

uint32_t WP_RESOURCE_SCHEMA_PLUGIN_CALL willpower_resource_schema_plugin_v1(
    const wp_resource_schema_plugin_request_v1* request,
    wp_resource_schema_plugin_response_v1* response) {
  uint8_t* copy;
  if (request == NULL || response == NULL ||
      request->struct_size < sizeof(wp_resource_schema_plugin_request_v1) ||
      response->struct_size < sizeof(wp_resource_schema_plugin_response_v1))
    return WP_RESOURCE_SCHEMA_PLUGIN_FAILURE;

  response->struct_size = (uint32_t)sizeof(*response);
  response->abi_version = WP_RESOURCE_SCHEMA_PLUGIN_ABI_VERSION;
  if (request->abi_version != WP_RESOURCE_SCHEMA_PLUGIN_ABI_VERSION)
    return WP_RESOURCE_SCHEMA_PLUGIN_UNSUPPORTED_ABI;
  if (!supports_version(request->bundle_format_versions,
                        request->bundle_format_version_count) ||
      !supports_version(request->resource_manifest_schema_versions,
                        request->resource_manifest_schema_version_count))
    return WP_RESOURCE_SCHEMA_PLUGIN_UNSUPPORTED_BUNDLE_VERSION;

  copy = (uint8_t*)malloc(sizeof(sample_bundle) - 1u);
  if (copy == NULL) {
    static const uint8_t error[] = "cannot allocate sample bundle";
    response->error_message = error;
    response->error_message_size = sizeof(error) - 1u;
    return WP_RESOURCE_SCHEMA_PLUGIN_FAILURE;
  }
  memcpy(copy, sample_bundle, sizeof(sample_bundle) - 1u);
  response->bundle_format_version.major = 1u;
  response->bundle_format_version.minor = 0u;
  response->resource_manifest_schema_version.major = 1u;
  response->resource_manifest_schema_version.minor = 0u;
  response->bundle_data = copy;
  response->bundle_size = sizeof(sample_bundle) - 1u;
  response->release_context = copy;
  response->release = release_response;
  return WP_RESOURCE_SCHEMA_PLUGIN_SUCCESS;
}
