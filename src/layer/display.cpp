/* SPDX-License-Identifier: MIT */

// Refresh detection for the presented-frame cap. The layer only needs the
// current mode of an active display, so this reads the connector id from
// sysfs and asks DRM for the encoder/CRTC behind it. Using the kernel UAPI
// directly keeps the layer free of a libdrm runtime dependency; systems
// without the headers simply compile the fallback below.

#include "display.hpp"

#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#if __has_include(<drm/drm.h>) && __has_include(<drm/drm_mode.h>)
#include <drm/drm.h>
#include <drm/drm_mode.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#define OPENFG_HAVE_DRM_UAPI 1
#endif

namespace openfg_layer {

namespace {

    std::string readText(const std::filesystem::path& path) {
        std::ifstream file(path);
        std::string text;
        if (file.is_open())
            std::getline(file, text);
        while (!text.empty() &&
               (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
            text.pop_back();
        return text;
    }

    bool parseU32(std::string_view text, uint32_t& value) {
        if (text.empty())
            return false;
        const char* begin = text.data();
        const char* end = begin + text.size();
        const auto [ptr, error] = std::from_chars(begin, end, value);
        return error == std::errc{} && ptr == end;
    }

#ifdef OPENFG_HAVE_DRM_UAPI

    // A connector that is plugged in and has an active CRTC.
    constexpr uint32_t kConnected = 1;

    std::string pciSlot(const std::string& card) {
        std::ifstream file(
            std::filesystem::path("/sys/class/drm") / card / "device" / "uevent");
        std::string line;
        constexpr std::string_view prefix{"PCI_SLOT_NAME="};
        while (std::getline(file, line)) {
            if (line.starts_with(prefix))
                return line.substr(prefix.size());
        }
        return {};
    }

    struct ConnectorEntry {
        std::string card;
        std::string connector;
        std::filesystem::path path;
    };

    // Splits "card1-DP-2" into "card1" and "DP-2". Cards and render nodes do
    // not have a dash, so this also rejects them.
    bool parseConnectorEntry(const std::string& name, ConnectorEntry& entry) {
        constexpr std::string_view prefix{"card"};
        if (!name.starts_with(prefix))
            return false;
        const size_t dash = name.find('-');
        if (dash == std::string::npos || dash == prefix.size())
            return false;
        for (size_t i = prefix.size(); i < dash; i++) {
            if (name[i] < '0' || name[i] > '9')
                return false;
        }
        entry.card = name.substr(0, dash);
        entry.connector = name.substr(dash + 1);
        return !entry.connector.empty();
    }

    std::optional<DisplayInfo> queryDrm(
        const std::string& pciAddress,
        uint32_t width,
        uint32_t height
    ) {
        std::error_code error;
        std::filesystem::directory_iterator iterator("/sys/class/drm", error);
        if (error)
            return std::nullopt;

        std::vector<ConnectorEntry> connectors;
        std::set<std::string> cards;
        for (const auto& entry : iterator) {
            const std::string name = entry.path().filename().string();
            ConnectorEntry connector;
            if (!parseConnectorEntry(name, connector))
                continue;
            connector.path = entry.path();
            cards.insert(connector.card);
            connectors.push_back(std::move(connector));
        }
        if (connectors.empty())
            return std::nullopt;

        // If the Vulkan device has a PCI address, prefer the card that owns
        // it. Some containers only expose render nodes, in which case every
        // card is considered and the mode/extent match decides.
        std::map<std::string, bool> cardMatches;
        bool haveCardMatch{false};
        for (const std::string& card : cards) {
            const bool matches = !pciAddress.empty() && pciSlot(card) == pciAddress;
            cardMatches[card] = matches;
            haveCardMatch = haveCardMatch || matches;
        }
        const bool filterCards = !pciAddress.empty() && haveCardMatch;

        std::map<std::string, int> descriptors;
        auto descriptorFor = [&descriptors](const std::string& card) {
            auto it = descriptors.find(card);
            if (it != descriptors.end())
                return it->second;
            const std::string path = "/dev/dri/" + card;
            const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
            descriptors[card] = fd;
            return fd;
        };

        std::vector<DisplayInfo> candidates;

        for (const ConnectorEntry& connector : connectors) {
            if (filterCards && !cardMatches[connector.card])
                continue;
            if (readText(connector.path / "status") != "connected")
                continue;

            uint32_t connectorId{};
            if (!parseU32(readText(connector.path / "connector_id"), connectorId))
                continue;

            const int fd = descriptorFor(connector.card);
            if (fd < 0)
                continue;

            drm_mode_get_connector drmConnector{};
            drmConnector.connector_id = connectorId;
            if (::ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &drmConnector) < 0)
                continue;
            if (drmConnector.connection != kConnected || !drmConnector.encoder_id)
                continue;

            drm_mode_get_encoder encoder{};
            encoder.encoder_id = drmConnector.encoder_id;
            if (::ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &encoder) < 0 ||
                !encoder.crtc_id)
                continue;

            drm_mode_crtc crtc{};
            crtc.crtc_id = encoder.crtc_id;
            if (::ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) < 0 || !crtc.mode_valid)
                continue;

            const drm_mode_modeinfo& mode = crtc.mode;
            if (!mode.htotal || !mode.vtotal || !mode.hdisplay || !mode.vdisplay)
                continue;

            double refresh = static_cast<double>(mode.clock) * 1000.0 /
                (static_cast<double>(mode.htotal) * mode.vtotal);
            if (mode.flags & DRM_MODE_FLAG_INTERLACE)
                refresh *= 2.0;
            if (!(refresh > 0.0))
                continue;

            candidates.push_back({
                refresh,
                mode.hdisplay,
                mode.vdisplay,
                connector.connector
            });
        }

        for (const auto& entry : descriptors) {
            if (entry.second >= 0)
                ::close(entry.second);
        }

        const DisplayInfo* best{};
        bool bestIsExact{false};
        for (const DisplayInfo& candidate : candidates) {
            const bool exact =
                candidate.width == width && candidate.height == height;
            if (!best ||
                (exact && !bestIsExact) ||
                (exact == bestIsExact &&
                 candidate.refreshHz > best->refreshHz)) {
                best = &candidate;
                bestIsExact = exact;
            }
        }
        if (best)
            return *best;
        return std::nullopt;
    }

#endif

}

std::optional<DisplayInfo> queryDisplayRefresh(
    const std::string& pciAddress,
    uint32_t width,
    uint32_t height
) {
    if (const char* overrideValue = std::getenv("OPENFG_REFRESH");
        overrideValue && *overrideValue) {
        char* end{};
        const double refresh = std::strtod(overrideValue, &end);
        if (end && *end == '\0' && refresh > 0.0) {
            DisplayInfo info;
            info.refreshHz = refresh;
            info.connector = "OPENFG_REFRESH";
            return info;
        }
    }

#ifdef OPENFG_HAVE_DRM_UAPI
    return queryDrm(pciAddress, width, height);
#else
    (void)pciAddress;
    (void)width;
    (void)height;
    return std::nullopt;
#endif
}

}
