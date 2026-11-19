/* SPDX-License-Identifier: MIT */

#include "presenter.hpp"

#include "headless_presenter.hpp"
#ifdef OPENFG_SCALER_HAVE_SDL3
#include "sdl_presenter.hpp"
#endif

#include <stdexcept>

namespace openfg::scaler {

std::unique_ptr<Presenter> createPresenter(const PresentOptions& options)
{
    if (options.headless)
        return std::make_unique<HeadlessPresenter>(options);
#ifdef OPENFG_SCALER_HAVE_SDL3
    return std::make_unique<SdlPresenter>(options);
#else
    throw std::runtime_error(
        "openfg-scaler was built without SDL3; use --headless");
#endif
}

} // namespace openfg::scaler
