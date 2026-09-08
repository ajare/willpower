# Resource Type schema plugin C ABI

- **Status:** Accepted
- **ABI version:** 1
- **Plugin Bundle Container version:** Resource Schema Bundle 1.0 / Resource Manifest schema 1.0
- **Related issue:** [#36](https://github.com/ajare/willpower/issues/36)

## 1. Scope

A Resource Type Plugin is an optional native adapter that returns Resource Type schemas to tooling. It does not register or construct `Resource`, `ResourceFactory`, or `ResourceDefinitionFactory` objects and does not initialize application subsystems. A Resource Schema Bundle directory remains the preferred portable interchange format when dynamic discovery is unnecessary.

Loading a plugin executes native code. Hosts shall load plugins only from paths supplied explicitly by the user or embedding application. They shall not scan directories, environment variables, manifests, bundles, or system library paths for plugins.

## 2. Header and exported symbol

The complete ABI is declared by the C-compatible header:

```c
#include <willpower/application/resourcesystem/ResourceSchemaPlugin.h>
```

ABI version 1 has exactly one required exported symbol:

```text
willpower_resource_schema_plugin_v1
```

Its type is `wp_resource_schema_plugin_entry_v1`. The symbol uses C linkage and `WP_RESOURCE_SCHEMA_PLUGIN_CALL` (`__cdecl` on Windows). A plugin build defines `WP_RESOURCE_SCHEMA_PLUGIN_BUILD` so `WP_RESOURCE_SCHEMA_PLUGIN_EXPORT` exports the declaration. The header includes only `<stdint.h>` and exposes fixed-width integers, byte pointers, opaque `void*` context, and C function pointers. It contains no C++, Willpower runtime, standard-library, allocator, or third-party type.

A new incompatible ABI receives a new number, structures, function typedef, and symbol suffix. A version-1 host never guesses another entry-point name.

## 3. Negotiation and errors

The host passes `wp_resource_schema_plugin_request_v1` with:

- `struct_size` and `abi_version` set to the host's values;
- an array of supported Resource Schema Bundle versions; and
- an independent array of supported Resource Manifest schema versions.

The plugin checks `struct_size` and `abi_version` before reading later fields, selects one value from each array, and records both selections in the response. Willpower currently offers only `1.0` for each. Major and minor components are numeric in the ABI and correspond to the decimal `MAJOR.MINOR` strings in `catalog.json`.

The host zero-initializes `wp_resource_schema_plugin_response_v1` and sets its `struct_size`. The plugin returns one of:

| Status | Meaning |
| ---: | --- |
| `WP_RESOURCE_SCHEMA_PLUGIN_SUCCESS` | A bundle container and selected versions were returned. |
| `WP_RESOURCE_SCHEMA_PLUGIN_UNSUPPORTED_ABI` | The requested ABI cannot be used. |
| `WP_RESOURCE_SCHEMA_PLUGIN_UNSUPPORTED_BUNDLE_VERSION` | No offered bundle/schema version pair can be produced. |
| `WP_RESOURCE_SCHEMA_PLUGIN_FAILURE` | Another plugin-specific failure occurred. |

On failure, `error_message`/`error_message_size` may identify the cause as UTF-8 bytes. It need not be NUL terminated. The loader also rejects wrong response sizes, a mismatched response ABI, versions the host did not offer, null or oversized payloads, and unknown status values. Exceptions and platform runtime objects must never cross the entry point.

## 4. Plugin Bundle Container

On success, `bundle_data`/`bundle_size` is one UTF-8 JSON value with exactly this shape:

```json
{
  "catalog": {
    "bundleFormatVersion": "1.0",
    "resourceManifestSchemaVersion": "1.0",
    "schemas": []
  },
  "documents": [
    {
      "document": "schemas/example.schema.json",
      "contents": "{\"$schema\":\"http://json-schema.org/draft-07/schema#\",...}"
    }
  ]
}
```

`catalog` is the exact logical content of a Resource Schema Bundle's `catalog.json`; all normative catalog fields and rules remain those in [Resource Schema Bundle format](resource-schema-bundle.md). Each `documents` item has exactly `document` and `contents` string fields. `contents` decodes to the exact schema-document bytes used to calculate `documentHash`. Every catalogued path must have a matching document.

The container only transports an in-memory bundle across a one-call C ABI. After decoding it, the host submits the resulting bundle to `ResourceSchemaCatalog`, which applies the same versions, path containment, JSON shape, hash, schema ID, lookup-key collision, schema-ID collision, and closed `$ref` rules used for a directory bundle. There is no weaker plugin merge path.

## 5. Ownership and lifetime

All request memory belongs to the host and is valid only during the entry-point call. All response pointers belong to the plugin. The host never frees plugin memory directly.

The plugin keeps response pointers valid until the host calls the optional `release(response.release_context)` callback. If `release` is null, pointers refer to immutable plugin storage and stay valid until module unload. The one callback releases all response-owned storage and is called at most once while the module remains loaded.

Willpower's loader performs this sequence synchronously:

1. resolve the supplied path to an explicit absolute path and load that file;
2. resolve and invoke the version-1 symbol once;
3. copy all returned bytes into host-owned memory;
4. call `release`, when present;
5. unload the library;
6. parse and atomically merge the copied bundle.

Consequently, catalog entries and snapshots never point into a plugin module. A failed load, negotiation, container parse, bundle validation, collision check, or `$ref` check leaves the existing catalog unchanged.

## 6. Platform, trust, and deployment

Plugins are supported on 64-bit Windows and Linux. A plugin must match the host's operating system, CPU architecture, calling convention, and ABI structure layout. On Windows it is a DLL; on Linux it is a shared object. The host loads the exact absolute file (`LoadLibraryExW` or `dlopen` with local symbol visibility), but the operating-system loader still resolves that file's transitive native dependencies.

A plugin is executable code with the process's full privileges. Catalog validation is not a sandbox and cannot make a malicious library safe: module initializers run before the entry point and code may read files, access the network, mutate process state, or terminate the process. Load only trusted, authenticated plugins. Prefer signed artifacts and controlled deployment directories, and apply normal library search-path and dependency-hardening practices.

Deploy the plugin and all of its matching native dependencies for every target platform/configuration. Compiler runtimes used internally by the plugin are its deployment responsibility. Allocations are safe across different C/C++ runtimes only because allocation and release both happen inside the plugin; no CRT-owned object crosses the boundary. Unloading does not undo global process changes or stop threads created by a plugin, so conforming plugins must not leave threads, callbacks, or host hooks alive after `release` returns.

The loader enforces a 64 MiB response-byte limit to bound accidental copies. This is not a security boundary against native code. Resource Schema Bundle directories avoid all code-execution, architecture, runtime, and native-dependency concerns and should be used wherever possible.
