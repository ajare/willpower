#include <stddef.h>
#include <stdint.h>

#include "willpower/application/resourcesystem/ResourceSchemaPlugin.h"

uint32_t WP_RESOURCE_SCHEMA_PLUGIN_CALL willpower_resource_schema_plugin_v1(
    const wp_resource_schema_plugin_request_v1* request,
    wp_resource_schema_plugin_response_v1* response) {
  static const uint8_t message[] = "test plugin implements ABI version 99";
  (void)request;
  if (response == NULL ||
      response->struct_size < sizeof(wp_resource_schema_plugin_response_v1))
    return WP_RESOURCE_SCHEMA_PLUGIN_FAILURE;
  response->struct_size = (uint32_t)sizeof(*response);
  response->abi_version = 99u;
  response->error_message = message;
  response->error_message_size = sizeof(message) - 1u;
  return WP_RESOURCE_SCHEMA_PLUGIN_UNSUPPORTED_ABI;
}
