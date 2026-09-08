# Resource Schema editor annotations 1.0

Resource Schema editor annotations are optional JSON Schema Draft 7 metadata for the
Resource Manifest Editor. They do not change Resource Manifest validation semantics;
Draft 7 consumers that do not recognise them ignore them.

This document defines annotation vocabulary version **1.0**. An annotated property uses
all of these sibling keywords:

| Keyword | Value | Meaning |
| --- | --- | --- |
| `x-willpower-editor-version` | `"1.0"` | Annotation vocabulary version. |
| `x-willpower-widget` | `"file"` | The value is selected with a native file selector and is not edited as free text. |
| `x-willpower-file-kind` | non-empty string | Human-readable selector filter name. |
| `x-willpower-file-extensions` | non-empty array of strings | Extension names without `.`, `*`, separators, or SDL filter punctuation. |

Example:

```json
"location": {
  "$ref": "https://schemas.willpower.dev/resource-manifest/common.schema.json#/definitions/nonEmptyString",
  "x-willpower-editor-version": "1.0",
  "x-willpower-widget": "file",
  "x-willpower-file-kind": "XML files",
  "x-willpower-file-extensions": ["xml"]
}
```

A file widget accepts only an existing regular file after canonicalising both it and the
active document base directory. The canonical target must remain inside that canonical
base subtree; `..` traversal and symbolic-link/junction escapes are rejected. The value
committed to the Resource Manifest is the base-relative path with `/` separators.

The built-in `TextFile`, `XmlFile`, `Shader`, `AudioBank`, and `Image` schemas annotate
their required `location` property with this contract. Future annotation versions and
Resource-reference annotations are outside version 1.0 of this document.
