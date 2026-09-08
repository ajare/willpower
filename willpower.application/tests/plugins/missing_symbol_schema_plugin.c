#include <stdint.h>

#if defined(_WIN32)
#define TEST_PLUGIN_EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
#define TEST_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define TEST_PLUGIN_EXPORT
#endif

TEST_PLUGIN_EXPORT uint32_t not_the_willpower_schema_entry_point(void) { return 36u; }
