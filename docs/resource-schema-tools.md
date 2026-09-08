# Resource Schema Bundle tooling

See [Application Resource Schema integration](resource-schema-integration.md) for the
end-to-end authoring, CMake/runtime registration, editor, compatibility, and security
workflow.

`willpower-resource-schemas` inspects, verifies, merges, and exports Resource Schema
Bundles without reading Willpower's schema source directory. It starts with the twelve
schemas embedded in `Willpower.Application` (the manifest, two dependencies, and nine
built-in Resource Types), then merges each explicitly supplied `--bundle` or `--plugin`
in command-line order. Plugins are never searched for or loaded implicitly.

```sh
# Inventory the embedded catalog as JSON.
willpower-resource-schemas list

# Verify an application bundle, including versions, hashes, collisions, and $refs.
willpower-resource-schemas verify --bundle build/resource-schema-bundle

# Merge into a complete bundle, replacing input manifest entries with one root
# that dispatches every registered Resource Type and factory.
willpower-resource-schemas merge --bundle vendor/resource-schema-bundle \
  --output build/merged-resource-schema-bundle

# Optionally discover schemas by executing one trusted native plugin at an
# exact user-supplied path (.dll on Windows, .so on Linux).
willpower-resource-schemas list --plugin vendor/plugins/widget-schema.dll

# Export does the same deterministic composition and also writes a convenient
# standalone copy of the root schema.
willpower-resource-schemas export --bundle build/application-resource-schema-bundle \
  --output build/editor-resource-schemas \
  --root-schema build/editor/resource-manifest.schema.json \
  --root-schema-id https://schemas.example.com/editor-resource-manifest.schema.json
```

`--root-schema` is optional and defaults to
`<output>/resource-manifest.schema.json`. The catalogued copy is always at the document
path recorded by `catalog.json`. Identical inputs, IDs, and options produce identical
catalog, document, and root-schema bytes on Windows and Linux.

All successful commands write JSON to standard output. Failures write one JSON object to
standard error with `ok`, numeric `code`, `message`, `bundle`, `key`, and `schemaId`
fields. Plugin-aware diagnostics additionally include `plugin`; `bundle`, `plugin`,
`key`, or `schemaId` is `null` when it cannot apply. Exit codes are:

| Code | Meaning |
| ---: | --- |
| 2 | command-line usage |
| 3 | incompatible plugin ABI, bundle, or Resource Manifest schema version |
| 4 | Resource Schema key or schema-ID collision |
| 5 | malformed or mismatched document hash |
| 6 | unresolved local `$ref` document or fragment |
| 7 | another invalid bundle or export error |

## CI

A reproducible CI job can build only the exporter, verify checked-in downstream bundles,
and publish generated editor data:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target willpower_resource_schemas --parallel
build/bin/Release/willpower_resource_schemas/willpower-resource-schemas \
  verify --bundle schemas/resource-schema-bundle
build/bin/Release/willpower_resource_schemas/willpower-resource-schemas \
  export --bundle schemas/resource-schema-bundle --output build/schema-artifact
```

For a Visual Studio generator, add `--config Release`; the executable is under the same
configuration-specific output layout documented in the main README. Archive the whole
`schema-artifact` directory so all `$ref` targets remain available.

For VS Code with Red Hat YAML, point the workspace at the convenient root copy:

```json
{
  "yaml.schemas": {
    "./build/schema-artifact/resource-manifest.schema.json": ["**/Resources.yaml", "**/Resources.yml"]
  }
}
```

## Application-owned schema registration

Applications that select schemas in C++ can reuse the CLI entry point in a tiny exporter
target. Registration needs only `ResourceSchemaCatalog`; it does not construct a
`Resource`, `ResourceManager`, renderer, audio system, platform, or service locator.

```cpp
#include "willpower/application/resourcesystem/ResourceSchemaCatalog.h"

void registerApplicationSchemas(
    wp::application::resourcesystem::ResourceSchemaCatalog&); // application-owned code

int main(int argc, char const* const* argv) {
  auto catalog = wp::application::resourcesystem::ResourceSchemaCatalog::builtIn();
  registerApplicationSchemas(catalog);
  return wp::application::resourcesystem::runResourceSchemaExporter(catalog, argc, argv);
}
```

Link this target to `Willpower.Application`. A complete working target is in
`tests/downstream/resource-schema-bundle/schema_exporter.cpp`; its registration function
adds the application bundle and delegates argument handling, deterministic composition,
and machine-readable diagnostics to `runResourceSchemaExporter`.

## Optional native plugins

`--plugin FILE` may be repeated and may be mixed with `--bundle`. Each plugin is loaded
only from that explicit path, invoked once, copied, released, and unloaded before its
bundle is validated and merged. The sample C plugin in
`willpower.application/tests/plugins/sample_schema_plugin.c` contributes the
`PluginWidget` Resource Type and `GlowFactory` specialized schema without linking to
`Willpower.Application` or initializing any runtime service.

Plugins execute arbitrary native code and must match the host OS and architecture. Use
only trusted artifacts and deploy all of their native dependencies. Prefer bundle
directories when code execution and dynamic discovery are unnecessary. The normative
ABI, ownership, negotiation, deployment, and trust contract is in
[Resource Type schema plugin C ABI](specifications/resource-schema-plugin.md).
