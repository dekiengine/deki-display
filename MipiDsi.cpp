#include "MipiDsi.h"

namespace DekiDisplay
{

MipiDsi::Factory MipiDsi::s_Factory = nullptr;

void MipiDsi::SetFactory(Factory factory)
{
    s_Factory = factory;
}

bool MipiDsi::HasFactory()
{
    return s_Factory != nullptr;
}

std::unique_ptr<IDsiPanel> MipiDsi::Create(const DsiPanelConfig& config)
{
    return s_Factory ? s_Factory(config) : nullptr;
}

}  // namespace DekiDisplay
