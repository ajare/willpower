# Resource Manifest Management

This context defines the language used to describe, edit, and validate application resource manifests and their extensible resource types.

## Language

**Resource Manifest**:
A YAML or XML document declaring application resources, their namespaces, dependencies, definitions, options, and source locations.
_Avoid_: Resources file, resource definition file

**Resource**:
A named application asset declaration belonging to a Resource Type and optionally to a namespace.
_Avoid_: Entry, item

**Resource Type**:
The registered kind of a Resource, such as `Image`, which determines its parameters and allowed values.
_Avoid_: Resource name, factory type

**Resource Schema Bundle**:
A versioned, language-neutral package containing Resource Type schemas, their local dependencies, and catalog metadata for Resource Manifest validation and tooling. A bundle resolves entirely from its own content; its format and compatibility policy are defined in [`docs/specifications/resource-schema-bundle.md`](docs/specifications/resource-schema-bundle.md).
_Avoid_: Schema dump, Resource plugin

**Resource Type Plugin**:
A dynamically loaded library that exposes one or more Resource Type schemas to resource-manifest tooling as a Resource Schema Bundle.
_Avoid_: ResourceFactory DLL, factory plugin

**Resource Manifest Editor**:
The standalone desktop tool for creating, editing, and structurally validating Resource Manifests.
_Avoid_: ResourceManager, resource-manager object
