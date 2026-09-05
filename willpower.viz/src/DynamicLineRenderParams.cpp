#include "willpower/viz/DynamicLineRenderParams.h"

namespace WP_NAMESPACE {
namespace viz {
using namespace wp;
using namespace std;

DynamicLineRenderParams::DynamicLineRenderParams(shared_ptr<mpp::ModelRenderParams> params)
    : RenderParams() {
  WP_UNUSED(params);
}

float DynamicLineRenderParams::getGridPadding() const {
  return 0.0f;
}

}  // namespace viz
}  // namespace WP_NAMESPACE
