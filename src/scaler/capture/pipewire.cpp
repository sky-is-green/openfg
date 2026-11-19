/* SPDX-License-Identifier: MIT */

#include "pipewire.hpp"

#include "vkutil/vkutil.hpp"

#include <pipewire/keys.h>
#include <pipewire/pipewire.h>
#include <pipewire/stream.h>
#include <pipewire/thread-loop.h>
#include <spa/param/video/raw-utils.h>
#include <spa/pod/builder.h>
#include <spa/pod/vararg.h>
#include <spa/utils/hook.h>

#include <dbus/dbus.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iostream>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace openfg::scaler {
namespace {

    constexpr const char* PORTAL_BUS = "org.freedesktop.portal.Desktop";
    constexpr const char* PORTAL_PATH = "/org/freedesktop/portal/desktop";
    constexpr const char* PORTAL_SCREENCAST =
        "org.freedesktop.portal.ScreenCast";

    struct PortalResponse {
        bool done{false};
        uint32_t code{1};
        std::string session;
        std::string restoreToken;
        uint32_t node{0};
        bool hasNode{false};
    };

    std::string methodToken(const char* method, unsigned counter)
    {
        std::string token = "openfg_";
        token += method;
        token += "_";
        token += std::to_string(counter);
        return token;
    }

    struct Portal {
        DBusConnection* connection{};
        PortalResponse response;
        std::string session;
        std::string restoreToken;
        uint32_t node{0};
        unsigned counter{0};

        ~Portal() { close(); }

        bool open(std::string& error)
        {
            DBusError dbusError;
            dbus_error_init(&dbusError);
            connection = dbus_bus_get(DBUS_BUS_SESSION, &dbusError);
            if (!connection) {
                error = "cannot connect to the session bus: ";
                error += dbusError.message ? dbusError.message : "unknown";
                dbus_error_free(&dbusError);
                return false;
            }
            dbus_connection_add_filter(
                connection, &Portal::filter, this, nullptr);
            return true;
        }

        void close()
        {
            if (!connection)
                return;
            dbus_connection_remove_filter(
                connection, &Portal::filter, this);
            dbus_connection_unref(connection);
            connection = nullptr;
        }

        std::string uniqueSender() const
        {
            const char* name = dbus_bus_get_unique_name(connection);
            std::string sender = name ? name : "";
            if (!sender.empty() && sender.front() == ':')
                sender.erase(sender.begin());
            std::replace(sender.begin(), sender.end(), '.', '_');
            return sender;
        }

        bool addMatch(const std::string& path, std::string& error)
        {
            DBusError dbusError;
            dbus_error_init(&dbusError);
            const std::string rule =
                "type='signal',interface='org.freedesktop.portal.Request',"
                "member='Response',path='" + path + "'";
            dbus_bus_add_match(connection, rule.c_str(), &dbusError);
            dbus_connection_flush(connection);
            if (dbus_error_is_set(&dbusError)) {
                error = dbusError.message ? dbusError.message : "D-Bus match";
                dbus_error_free(&dbusError);
                return false;
            }
            return true;
        }

        void removeMatch(const std::string& path)
        {
            DBusError dbusError;
            dbus_error_init(&dbusError);
            const std::string rule =
                "type='signal',interface='org.freedesktop.portal.Request',"
                "member='Response',path='" + path + "'";
            dbus_bus_remove_match(connection, rule.c_str(), &dbusError);
            if (dbus_error_is_set(&dbusError))
                dbus_error_free(&dbusError);
            dbus_connection_flush(connection);
        }

        static DBusHandlerResult filter(
            DBusConnection*,
            DBusMessage* message,
            void* userData
        ) {
            auto* self = static_cast<Portal*>(userData);
            if (!dbus_message_is_signal(message,
                    "org.freedesktop.portal.Request", "Response"))
                return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

            DBusMessageIter iter;
            if (!dbus_message_iter_init(message, &iter))
                return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
            if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_UINT32)
                return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

            PortalResponse response;
            dbus_message_iter_get_basic(&iter, &response.code);
            dbus_message_iter_next(&iter);
            if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY)
                self->parseResults(&iter, response);
            response.done = true;
            self->response = response;
            return DBUS_HANDLER_RESULT_HANDLED;
        }

        void parseResults(DBusMessageIter* iter, PortalResponse& result)
        {
            DBusMessageIter dictionary;
            dbus_message_iter_recurse(iter, &dictionary);
            while (dbus_message_iter_get_arg_type(&dictionary) ==
                   DBUS_TYPE_DICT_ENTRY) {
                DBusMessageIter entry;
                dbus_message_iter_recurse(&dictionary, &entry);
                const char* key{};
                if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING)
                    break;
                dbus_message_iter_get_basic(&entry, &key);
                dbus_message_iter_next(&entry);
                DBusMessageIter variant;
                dbus_message_iter_recurse(&entry, &variant);
                const int type = dbus_message_iter_get_arg_type(&variant);

                if (std::strcmp(key, "session_handle") == 0 &&
                    type == DBUS_TYPE_STRING) {
                    const char* value{};
                    dbus_message_iter_get_basic(&variant, &value);
                    result.session = value ? value : "";
                } else if (std::strcmp(key, "restore_token") == 0 &&
                           type == DBUS_TYPE_STRING) {
                    const char* value{};
                    dbus_message_iter_get_basic(&variant, &value);
                    result.restoreToken = value ? value : "";
                } else if (std::strcmp(key, "streams") == 0 &&
                           type == DBUS_TYPE_ARRAY) {
                    DBusMessageIter streams;
                    dbus_message_iter_recurse(&variant, &streams);
                    if (dbus_message_iter_get_arg_type(&streams) ==
                        DBUS_TYPE_STRUCT) {
                        DBusMessageIter stream;
                        dbus_message_iter_recurse(&streams, &stream);
                        uint32_t id{};
                        if (dbus_message_iter_get_arg_type(&stream) ==
                            DBUS_TYPE_UINT32) {
                            dbus_message_iter_get_basic(&stream, &id);
                            result.node = id;
                            result.hasNode = true;
                        }
                    }
                }
                dbus_message_iter_next(&dictionary);
            }
        }

        using AppendArgs =
            std::function<void(DBusMessageIter&, const std::string&)>;

        bool request(
            const char* method,
            const AppendArgs& append,
            int timeoutMs,
            std::string& error
        ) {
            const std::string token = methodToken(method, ++counter);
            const std::string requestObject =
                "/org/freedesktop/portal/desktop/request/" +
                uniqueSender() + "/" + token;

            response = {};
            if (!addMatch(requestObject, error))
                return false;

            DBusMessage* message = dbus_message_new_method_call(
                PORTAL_BUS, PORTAL_PATH, PORTAL_SCREENCAST, method);
            if (!message) {
                removeMatch(requestObject);
                error = "cannot allocate a portal D-Bus message";
                return false;
            }
            DBusMessageIter args;
            dbus_message_iter_init_append(message, &args);
            append(args, token);

            DBusError dbusError;
            dbus_error_init(&dbusError);
            DBusMessage* reply = dbus_connection_send_with_reply_and_block(
                connection, message, 10000, &dbusError);
            dbus_message_unref(message);
            if (!reply) {
                error = std::string(method) + ": " +
                    (dbusError.message ? dbusError.message : "no reply");
                dbus_error_free(&dbusError);
                removeMatch(requestObject);
                return false;
            }

            DBusMessageIter replyIter;
            std::string actualPath;
            if (dbus_message_iter_init(reply, &replyIter) &&
                dbus_message_iter_get_arg_type(&replyIter) ==
                    DBUS_TYPE_OBJECT_PATH) {
                const char* path{};
                dbus_message_iter_get_basic(&replyIter, &path);
                actualPath = path ? path : "";
            }
            dbus_message_unref(reply);

            if (!actualPath.empty() && actualPath != requestObject) {
                if (!addMatch(actualPath, error)) {
                    removeMatch(requestObject);
                    return false;
                }
            }

            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(timeoutMs);
            while (!response.done) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    error = std::string(method) +
                        ": portal request timed out";
                    break;
                }
                if (!dbus_connection_read_write_dispatch(connection, 50)) {
                    error = std::string(method) +
                        ": session bus disconnected";
                    break;
                }
            }

            if (!actualPath.empty() && actualPath != requestObject)
                removeMatch(actualPath);
            removeMatch(requestObject);

            if (!response.done)
                return false;
            if (response.code != 0) {
                error = std::string(method) + ": portal response code " +
                    std::to_string(response.code);
                return false;
            }
            if (!response.session.empty())
                session = response.session;
            if (!response.restoreToken.empty())
                restoreToken = response.restoreToken;
            if (response.hasNode)
                node = response.node;
            return true;
        }

        static void addString(
            DBusMessageIter& dictionary,
            const char* key,
            const std::string& value
        ) {
            DBusMessageIter entry;
            DBusMessageIter variant;
            dbus_message_iter_open_container(
                &dictionary, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
            dbus_message_iter_append_basic(
                &entry, DBUS_TYPE_STRING, &key);
            dbus_message_iter_open_container(
                &entry, DBUS_TYPE_VARIANT, "s", &variant);
            const char* text = value.c_str();
            dbus_message_iter_append_basic(
                &variant, DBUS_TYPE_STRING, &text);
            dbus_message_iter_close_container(&entry, &variant);
            dbus_message_iter_close_container(&dictionary, &entry);
        }

        static void addBool(
            DBusMessageIter& dictionary,
            const char* key,
            bool value
        ) {
            DBusMessageIter entry;
            DBusMessageIter variant;
            dbus_message_iter_open_container(
                &dictionary, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
            dbus_message_iter_append_basic(
                &entry, DBUS_TYPE_STRING, &key);
            dbus_message_iter_open_container(
                &entry, DBUS_TYPE_VARIANT, "b", &variant);
            const dbus_bool_t flag = value ? TRUE : FALSE;
            dbus_message_iter_append_basic(
                &variant, DBUS_TYPE_BOOLEAN, &flag);
            dbus_message_iter_close_container(&entry, &variant);
            dbus_message_iter_close_container(&dictionary, &entry);
        }

        static void addUint32(
            DBusMessageIter& dictionary,
            const char* key,
            uint32_t value
        ) {
            DBusMessageIter entry;
            DBusMessageIter variant;
            dbus_message_iter_open_container(
                &dictionary, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
            dbus_message_iter_append_basic(
                &entry, DBUS_TYPE_STRING, &key);
            dbus_message_iter_open_container(
                &entry, DBUS_TYPE_VARIANT, "u", &variant);
            dbus_message_iter_append_basic(
                &variant, DBUS_TYPE_UINT32, &value);
            dbus_message_iter_close_container(&entry, &variant);
            dbus_message_iter_close_container(&dictionary, &entry);
        }

        bool createSession(std::string& error)
        {
            return request("CreateSession",
                [](DBusMessageIter& args, const std::string& token) {
                    DBusMessageIter dictionary;
                    dbus_message_iter_open_container(
                        &args, DBUS_TYPE_ARRAY, "{sv}", &dictionary);
                    addString(dictionary, "handle_token", token);
                    addString(dictionary, "session_handle_token", token);
                    dbus_message_iter_close_container(&args, &dictionary);
                }, 60000, error);
        }

        bool selectSources(bool window, std::string& error)
        {
            const uint32_t types = window ? 2U : 1U;
            return request("SelectSources",
                [this, types](DBusMessageIter& args,
                              const std::string& token) {
                    const char* handle = session.c_str();
                    dbus_message_iter_append_basic(
                        &args, DBUS_TYPE_OBJECT_PATH, &handle);
                    DBusMessageIter dictionary;
                    dbus_message_iter_open_container(
                        &args, DBUS_TYPE_ARRAY, "{sv}", &dictionary);
                    addString(dictionary, "handle_token", token);
                    addUint32(dictionary, "types", types);
                    addBool(dictionary, "multiple", false);
                    addUint32(dictionary, "cursor_mode", 1);
                    dbus_message_iter_close_container(&args, &dictionary);
                }, 60000, error);
        }

        bool start(std::string& error)
        {
            return request("Start",
                [this](DBusMessageIter& args, const std::string& token) {
                    const char* handle = session.c_str();
                    dbus_message_iter_append_basic(
                        &args, DBUS_TYPE_OBJECT_PATH, &handle);
                    const char* parent = "";
                    dbus_message_iter_append_basic(
                        &args, DBUS_TYPE_STRING, &parent);
                    DBusMessageIter dictionary;
                    dbus_message_iter_open_container(
                        &args, DBUS_TYPE_ARRAY, "{sv}", &dictionary);
                    addString(dictionary, "handle_token", token);
                    dbus_message_iter_close_container(&args, &dictionary);
                }, 120000, error);
        }

        bool openRemote(int& fd, std::string& error)
        {
            DBusMessage* message = dbus_message_new_method_call(
                PORTAL_BUS, PORTAL_PATH, PORTAL_SCREENCAST,
                "OpenPipeWireRemote");
            if (!message) {
                error = "cannot allocate a portal D-Bus message";
                return false;
            }
            DBusMessageIter args;
            dbus_message_iter_init_append(message, &args);
            const char* handle = session.c_str();
            dbus_message_iter_append_basic(
                &args, DBUS_TYPE_OBJECT_PATH, &handle);
            DBusMessageIter dictionary;
            dbus_message_iter_open_container(
                &args, DBUS_TYPE_ARRAY, "{sv}", &dictionary);
            dbus_message_iter_close_container(&args, &dictionary);

            DBusError dbusError;
            dbus_error_init(&dbusError);
            DBusMessage* reply = dbus_connection_send_with_reply_and_block(
                connection, message, 10000, &dbusError);
            dbus_message_unref(message);
            if (!reply) {
                error = std::string("OpenPipeWireRemote: ") +
                    (dbusError.message ? dbusError.message : "no reply");
                dbus_error_free(&dbusError);
                return false;
            }

            DBusMessageIter replyIter;
            if (!dbus_message_iter_init(reply, &replyIter) ||
                dbus_message_iter_get_arg_type(&replyIter) !=
                    DBUS_TYPE_UNIX_FD) {
                error = "OpenPipeWireRemote returned no file descriptor";
                dbus_message_unref(reply);
                return false;
            }
            int remote = -1;
            dbus_message_iter_get_basic(&replyIter, &remote);
            dbus_message_unref(reply);
            if (remote < 0) {
                error = "OpenPipeWireRemote returned an invalid descriptor";
                return false;
            }
            fd = remote;
            return true;
        }
    };

    struct ImportedImage {
        vk::UniqueImage image;
        vk::UniqueDeviceMemory memory;
        int fd{-1};

        ~ImportedImage() { reset(); }

        void reset()
        {
            image = {};
            memory = {};
            if (fd >= 0) {
                ::close(fd);
                fd = -1;
            }
        }
    };

} // namespace

struct PipeWireSource::Impl {
    struct RingImage {
        vk::UniqueImage image;
        vk::UniqueDeviceMemory memory;
        vk::UniqueImageView view;
    };

    struct Queued {
        pw_buffer* buffer{};
        uint64_t timestamp{};
    };

    Runtime& runtime;
    vk::Extent2D wanted;
    std::string capture;
    std::string description;

    Portal portal;
    bool pipewireInitialized{false};
    pw_thread_loop* loop{};
    pw_context* context{};
    pw_core* core{};
    pw_stream* stream{};
    pw_stream_events events{};
    spa_hook hook{};

    std::mutex mutex;
    std::condition_variable condition;
    std::deque<Queued> queue;
    bool formatReady{false};
    bool failed{false};
    bool stopping{false};
    std::string error;

    vk::Extent2D extent{};
    vk::Format format{vk::Format::eUndefined};
    uint64_t modifier{0};
    uint32_t spaFormat{0};

    std::array<RingImage, 3> ring;
    std::array<bool, 3> initialized{};
    std::array<bool, 3> busy{};
    bool ringReady{false};
    bool reportedMode{false};
    int returned{-1};
    uint64_t sequence{0};

    vk::UniqueCommandPool commandPool;
    vk::UniqueCommandBuffer command;
    vk::UniqueFence fence;
    vk::UniqueBuffer staging;
    vk::UniqueDeviceMemory stagingMemory;
    void* stagingMapped{};
    vk::DeviceSize stagingSize{};

    Impl(Runtime& runtimeValue, vk::Extent2D wantedValue,
         std::string captureValue)
        : runtime{runtimeValue},
          wanted{wantedValue},
          capture{std::move(captureValue)}
    {
    }

    ~Impl() { stop(); }

    void fail(std::string message)
    {
        std::lock_guard lock{mutex};
        failed = true;
        error = std::move(message);
        condition.notify_all();
    }

    static std::optional<vk::Format> toVulkan(uint32_t format)
    {
        switch (format) {
            case SPA_VIDEO_FORMAT_BGRx:
            case SPA_VIDEO_FORMAT_BGRA:
                return vk::Format::eB8G8R8A8Unorm;
            case SPA_VIDEO_FORMAT_RGBx:
            case SPA_VIDEO_FORMAT_RGBA:
                return vk::Format::eR8G8B8A8Unorm;
            default:
                return std::nullopt;
        }
    }

    static const char* formatName(uint32_t format)
    {
        switch (format) {
            case SPA_VIDEO_FORMAT_BGRx: return "BGRx";
            case SPA_VIDEO_FORMAT_BGRA: return "BGRA";
            case SPA_VIDEO_FORMAT_RGBx: return "RGBx";
            case SPA_VIDEO_FORMAT_RGBA: return "RGBA";
            default: return "unknown";
        }
    }

    static void onStateChanged(
        void* data,
        pw_stream_state,
        pw_stream_state state,
        const char* message
    ) {
        auto* self = static_cast<Impl*>(data);
        if (state == PW_STREAM_STATE_ERROR)
            self->fail(message && *message
                ? message : "PipeWire stream error");
    }

    static void onParamChanged(
        void* data,
        uint32_t id,
        const spa_pod* param
    ) {
        auto* self = static_cast<Impl*>(data);
        if (id != SPA_PARAM_Format || !param)
            return;

        spa_video_info_raw info{};
        if (spa_format_video_raw_parse(param, &info) < 0) {
            self->fail("cannot parse the PipeWire video format");
            return;
        }
        const auto format = toVulkan(info.format);
        if (!format) {
            self->fail("unsupported PipeWire format " +
                std::to_string(info.format));
            return;
        }
        if (info.size.width == 0 || info.size.height == 0) {
            self->fail("PipeWire negotiated an empty frame");
            return;
        }

        std::lock_guard lock{self->mutex};
        if (self->formatReady) {
            if (*format != self->format ||
                info.size.width != self->extent.width ||
                info.size.height != self->extent.height)
                self->fail("PipeWire changed the capture format mid-stream");
            return;
        }
        self->format = *format;
        self->extent = vk::Extent2D{
            info.size.width,
            info.size.height
        };
        self->modifier = info.modifier;
        self->spaFormat = info.format;
        self->description = "pipewire " + self->capture + " " +
            formatName(info.format) + " " +
            std::to_string(info.size.width) + "x" +
            std::to_string(info.size.height);
        self->formatReady = true;
        self->condition.notify_all();
    }

    static void onProcess(void* data)
    {
        auto* self = static_cast<Impl*>(data);
        if (!self->stream)
            return;
        pw_buffer* buffer = pw_stream_dequeue_buffer(self->stream);
        if (!buffer)
            return;

        {
            std::lock_guard lock{self->mutex};
            if (self->failed || self->stopping) {
                pw_stream_queue_buffer(self->stream, buffer);
                return;
            }
            if (self->queue.size() >= 2) {
                const Queued old = self->queue.front();
                self->queue.pop_front();
                pw_stream_queue_buffer(self->stream, old.buffer);
            }
            const uint64_t timestamp = buffer->time != 0
                ? buffer->time
                : static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
            self->queue.push_back({ buffer, timestamp });
        }
        self->condition.notify_all();
    }


    std::vector<uint64_t> supportedModifiers() const
    {
        std::vector<uint64_t> result{ 0 };
        for (const auto candidate : {
                 vk::Format::eB8G8R8A8Unorm,
                 vk::Format::eR8G8B8A8Unorm }) {
            vk::DrmFormatModifierPropertiesListEXT list{};
            vk::FormatProperties2 properties{ .pNext = &list };
            runtime.physical.getFormatProperties2(
                candidate, &properties, runtime.dld);
            if (list.drmFormatModifierCount == 0)
                continue;
            std::vector<vk::DrmFormatModifierPropertiesEXT> values(
                list.drmFormatModifierCount);
            list.pDrmFormatModifierProperties = values.data();
            runtime.physical.getFormatProperties2(
                candidate, &properties, runtime.dld);
            for (const auto& value : values) {
                const auto features = value.drmFormatModifierTilingFeatures;
                if (!(features & vk::FormatFeatureFlagBits::eSampledImage) ||
                    !(features & vk::FormatFeatureFlagBits::eTransferSrc))
                    continue;
                if (std::find(result.begin(), result.end(),
                        value.drmFormatModifier) == result.end())
                    result.push_back(value.drmFormatModifier);
            }
        }
        return result;
    }

    spa_pod* buildFormat(spa_pod_builder& builder)
    {
        spa_pod_frame object;
        spa_pod_builder_push_object(&builder, &object,
            SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
        spa_pod_builder_prop(&builder, SPA_FORMAT_mediaType, 0);
        spa_pod_builder_id(&builder, SPA_MEDIA_TYPE_video);
        spa_pod_builder_prop(&builder, SPA_FORMAT_mediaSubtype, 0);
        spa_pod_builder_id(&builder, SPA_MEDIA_SUBTYPE_raw);

        spa_pod_builder_prop(&builder, SPA_FORMAT_VIDEO_format, 0);
        spa_pod_frame choice;
        spa_pod_builder_push_choice(
            &builder, &choice, SPA_CHOICE_Enum, 0);
        spa_pod_builder_id(&builder, SPA_VIDEO_FORMAT_BGRx);
        spa_pod_builder_id(&builder, SPA_VIDEO_FORMAT_BGRA);
        spa_pod_builder_id(&builder, SPA_VIDEO_FORMAT_RGBx);
        spa_pod_builder_id(&builder, SPA_VIDEO_FORMAT_RGBA);
        spa_pod_builder_pop(&builder, &choice);

        spa_pod_builder_prop(&builder, SPA_FORMAT_VIDEO_size, 0);
        spa_pod_builder_push_choice(
            &builder, &choice, SPA_CHOICE_Range, 0);
        spa_pod_builder_rectangle(
            &builder, wanted.width, wanted.height);
        spa_pod_builder_rectangle(&builder, 1, 1);
        spa_pod_builder_rectangle(&builder, 8192, 8192);
        spa_pod_builder_pop(&builder, &choice);

        spa_pod_builder_prop(&builder, SPA_FORMAT_VIDEO_framerate, 0);
        spa_pod_builder_push_choice(
            &builder, &choice, SPA_CHOICE_Range, 0);
        spa_pod_builder_fraction(&builder, 0, 1);
        spa_pod_builder_fraction(&builder, 0, 1);
        spa_pod_builder_fraction(&builder, 240, 1);
        spa_pod_builder_pop(&builder, &choice);

        // A modifier choice enables DMA-BUF. The producer picks one of the
        // modifiers Vulkan can import for our capture formats.
        spa_pod_builder_prop(&builder, SPA_FORMAT_VIDEO_modifier,
            SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
        spa_pod_builder_push_choice(
            &builder, &choice, SPA_CHOICE_Enum, 0);
        for (const auto modifier : supportedModifiers())
            spa_pod_builder_long(&builder,
                static_cast<int64_t>(modifier));
        spa_pod_builder_pop(&builder, &choice);

        return static_cast<spa_pod*>(spa_pod_builder_pop(&builder, &object));
    }

    bool createRing(std::string& error)
    {
        runtime.device->waitIdle(runtime.dld);
        for (auto& image : ring)
            image = {};
        initialized = {};
        busy = {};

        const bool bgr = format == vk::Format::eB8G8R8A8Unorm;
        for (auto& image : ring) {
            image.image = vkutil::createImage(
                runtime.dld, *runtime.device, extent, format, 1,
                vk::ImageUsageFlagBits::eSampled |
                vk::ImageUsageFlagBits::eTransferDst);
            if (!image.image) {
                error = "cannot create the PipeWire capture image";
                return false;
            }
            const auto requirements =
                runtime.device->getImageMemoryRequirements(
                    *image.image, runtime.dld);
            image.memory = vkutil::allocateMemory(
                runtime.dld, *runtime.device, runtime.physical,
                requirements.size, requirements.memoryTypeBits);
            runtime.device->bindImageMemory(
                *image.image, *image.memory, 0, runtime.dld);
            const vk::ImageViewCreateInfo viewInfo{
                .image = *image.image,
                .viewType = vk::ImageViewType::e2D,
                .format = format,
                .components = {
                    .r = bgr ? vk::ComponentSwizzle::eB
                             : vk::ComponentSwizzle::eIdentity,
                    .g = vk::ComponentSwizzle::eIdentity,
                    .b = bgr ? vk::ComponentSwizzle::eR
                             : vk::ComponentSwizzle::eIdentity,
                    .a = vk::ComponentSwizzle::eIdentity
                },
                .subresourceRange = {
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .levelCount = 1,
                    .layerCount = 1
                }
            };
            image.view = runtime.device->createImageViewUnique(
                viewInfo, nullptr, runtime.dld);
        }

        commandPool = vkutil::createCommandPool(
            runtime.dld, *runtime.device, runtime.queueFamily, true);
        command = vkutil::createCommandBuffer(
            runtime.dld, *runtime.device, *commandPool);
        fence = vkutil::createFence(runtime.dld, *runtime.device);
        ringReady = true;
        return true;
    }

    bool importDmaBuf(
        const spa_data& data,
        uint32_t offset,
        int32_t stride,
        uint32_t size,
        ImportedImage& imported,
        std::string& error
    ) {
        imported.fd = ::dup(static_cast<int>(data.fd));
        if (imported.fd < 0) {
            error = "cannot duplicate the PipeWire DMA-BUF descriptor";
            return false;
        }

        vk::PhysicalDeviceExternalImageFormatInfo external{
            .handleType = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT
        };
        vk::PhysicalDeviceImageDrmFormatModifierInfoEXT modifierInfo{
            .pNext = &external,
            .drmFormatModifier = modifier,
            .sharingMode = vk::SharingMode::eExclusive
        };
        vk::PhysicalDeviceImageFormatInfo2 formatInfo{
            .pNext = &modifierInfo,
            .format = format,
            .type = vk::ImageType::e2D,
            .tiling = vk::ImageTiling::eDrmFormatModifierEXT,
            .usage = vk::ImageUsageFlagBits::eSampled |
                vk::ImageUsageFlagBits::eTransferSrc
        };
        vk::ExternalImageFormatProperties externalProperties;
        vk::ImageFormatProperties2 properties{
            .pNext = &externalProperties
        };
        const auto queryResult = runtime.physical.getImageFormatProperties2(
            &formatInfo, &properties, runtime.dld);
        if (queryResult != vk::Result::eSuccess) {
            error = "the GPU cannot import this DMA-BUF format";
            return false;
        }
        const auto memoryFeatures =
            externalProperties.externalMemoryProperties.externalMemoryFeatures;
        if (!(memoryFeatures & vk::ExternalMemoryFeatureFlagBits::eImportable)) {
            error = "the GPU cannot import DMA-BUF memory for this format";
            return false;
        }

        bool created = false;
        if (modifier != 0) {
            const vk::SubresourceLayout layout{
                .offset = offset,
                .size = size != 0
                    ? size
                    : static_cast<uint64_t>(stride) * extent.height,
                .rowPitch = static_cast<uint64_t>(stride),
                .arrayPitch = 0,
                .depthPitch = 0
            };
            vk::ImageDrmFormatModifierExplicitCreateInfoEXT explicitInfo{
                .drmFormatModifier = modifier,
                .drmFormatModifierPlaneCount = 1,
                .pPlaneLayouts = &layout
            };
            vk::ExternalMemoryImageCreateInfo externalImage{
                .pNext = &explicitInfo,
                .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT
            };
            const vk::ImageCreateInfo info{
                .pNext = &externalImage,
                .imageType = vk::ImageType::e2D,
                .format = format,
                .extent = { extent.width, extent.height, 1 },
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .tiling = vk::ImageTiling::eDrmFormatModifierEXT,
                .usage = vk::ImageUsageFlagBits::eSampled |
                    vk::ImageUsageFlagBits::eTransferSrc,
                .sharingMode = vk::SharingMode::eExclusive,
                .initialLayout = vk::ImageLayout::eUndefined
            };
            imported.image = runtime.device->createImageUnique(
                info, nullptr, runtime.dld);
            created = true;
        }

        if (!created) {
            vk::ExternalMemoryImageCreateInfo externalImage{
                .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT
            };
            const vk::ImageCreateInfo info{
                .pNext = &externalImage,
                .imageType = vk::ImageType::e2D,
                .format = format,
                .extent = { extent.width, extent.height, 1 },
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .tiling = vk::ImageTiling::eLinear,
                .usage = vk::ImageUsageFlagBits::eSampled |
                    vk::ImageUsageFlagBits::eTransferSrc,
                .sharingMode = vk::SharingMode::eExclusive,
                .initialLayout = vk::ImageLayout::eUndefined
            };
            imported.image = runtime.device->createImageUnique(
                info, nullptr, runtime.dld);
        }

        const auto requirements =
            runtime.device->getImageMemoryRequirements(
                *imported.image, runtime.dld);
        bool allocated = false;
        for (uint32_t type = 0; type < 32; type++) {
            if (!(requirements.memoryTypeBits & (1U << type)))
                continue;
            vk::MemoryDedicatedAllocateInfo dedicated{
                .image = *imported.image
            };
            vk::ImportMemoryFdInfoKHR import{
                .pNext = &dedicated,
                .handleType = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT,
                .fd = imported.fd
            };
            const vk::MemoryAllocateInfo allocation{
                .pNext = &import,
                .allocationSize = requirements.size,
                .memoryTypeIndex = type
            };
            try {
                imported.memory = runtime.device->allocateMemoryUnique(
                    allocation, nullptr, runtime.dld);
                allocated = true;
                break;
            } catch (const vk::SystemError&) {
                continue;
            }
        }
        if (!allocated) {
            error = "cannot import DMA-BUF memory";
            return false;
        }
        runtime.device->bindImageMemory(
            *imported.image, *imported.memory, 0, runtime.dld);
        return true;
    }

    void imageBarrier(
        vk::Image image,
        vk::ImageLayout oldLayout,
        vk::ImageLayout newLayout,
        vk::AccessFlags srcAccess,
        vk::AccessFlags dstAccess,
        vk::PipelineStageFlags srcStage,
        vk::PipelineStageFlags dstStage
    ) {
        const vk::ImageMemoryBarrier barrier{
            .srcAccessMask = srcAccess,
            .dstAccessMask = dstAccess,
            .oldLayout = oldLayout,
            .newLayout = newLayout,
            .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
            .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
            .image = image,
            .subresourceRange = {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .levelCount = 1,
                .layerCount = 1
            }
        };
        command->pipelineBarrier(
            srcStage, dstStage, {}, {}, {}, { barrier }, runtime.dld);
    }

    bool ensureStaging(std::string& error)
    {
        if (staging)
            return true;
        stagingSize = static_cast<vk::DeviceSize>(extent.width) *
            extent.height * 4;
        auto [buffer, memory] = vkutil::createBuffer(
            runtime.dld, *runtime.device, runtime.physical,
            vk::BufferUsageFlagBits::eTransferSrc, nullptr,
            static_cast<size_t>(stagingSize));
        staging = std::move(buffer);
        stagingMemory = std::move(memory);
        if (runtime.device->mapMemory(
                *stagingMemory, 0, stagingSize, {}, &stagingMapped,
                runtime.dld) != vk::Result::eSuccess) {
            error = "cannot map the PipeWire staging buffer";
            return false;
        }
        return true;
    }

    bool copySharedMemory(
        const spa_data& data,
        uint32_t offset,
        int32_t stride,
        std::string& error
    ) {
        if (!ensureStaging(error))
            return false;
        const uint32_t rowBytes = extent.width * 4;
        const uint64_t sourceSize = static_cast<uint64_t>(stride) *
            extent.height;

        const uint8_t* source = nullptr;
        bool mapped = false;
        if (data.type == SPA_DATA_MemPtr) {
            source = static_cast<const uint8_t*>(data.data);
            if (source)
                source += offset;
        } else if (data.type == SPA_DATA_MemFd && data.fd >= 0) {
            void* address = ::mmap(nullptr,
                static_cast<size_t>(sourceSize), PROT_READ, MAP_SHARED,
                static_cast<int>(data.fd), 0);
            if (address == MAP_FAILED) {
                error = "cannot map the PipeWire frame";
                return false;
            }
            source = static_cast<const uint8_t*>(address) + offset;
            mapped = true;
        }
        if (!source) {
            error = "PipeWire provided no readable frame data";
            return false;
        }

        auto* destination = static_cast<uint8_t*>(stagingMapped);
        for (uint32_t y = 0; y < extent.height; y++) {
            std::memcpy(destination + static_cast<size_t>(y) * rowBytes,
                source + static_cast<size_t>(y) * stride, rowBytes);
        }
        if (mapped)
            ::munmap(const_cast<uint8_t*>(source - offset),
                static_cast<size_t>(sourceSize));
        return true;
    }

    bool copyFrame(Queued& queued, int index, std::string& error)
    {
        spa_buffer* buffer = queued.buffer->buffer;
        if (!buffer || buffer->n_datas == 0) {
            error = "PipeWire provided an empty buffer";
            return false;
        }
        spa_data* selected = nullptr;
        for (uint32_t plane = 0; plane < buffer->n_datas; plane++) {
            auto& candidate = buffer->datas[plane];
            if (candidate.type == SPA_DATA_DmaBuf ||
                candidate.type == SPA_DATA_MemFd ||
                candidate.type == SPA_DATA_MemPtr) {
                selected = &candidate;
                break;
            }
        }
        if (!selected) {
            error = "PipeWire provided no readable image plane";
            return false;
        }
        spa_data& data = *selected;
        const int32_t stride = data.chunk && data.chunk->stride > 0
            ? data.chunk->stride
            : static_cast<int32_t>(extent.width * 4);
        const uint32_t offset = data.mapoffset +
            (data.chunk ? data.chunk->offset : 0);
        const uint32_t size = data.chunk && data.chunk->size != 0
            ? data.chunk->size
            : static_cast<uint32_t>(stride) * extent.height;

        ImportedImage imported;
        bool isDmaBuf = false;
        if (data.type == SPA_DATA_DmaBuf && data.fd >= 0) {
            if (!importDmaBuf(data, offset, stride, size, imported, error))
                return false;
            isDmaBuf = true;
        } else if (!copySharedMemory(data, offset, stride, error)) {
            return false;
        }
        if (!reportedMode) {
            std::cout << "capture: " << (isDmaBuf ? "DMA-BUF" : "shared memory")
                      << " " << extent.width << "x" << extent.height
                      << " " << formatName(spaFormat)
                      << " planes=" << buffer->n_datas
                      << " stride=" << stride
                      << " offset=" << offset
                      << " modifier=0x" << std::hex << modifier
                      << std::dec << "\n";
            reportedMode = true;
        }

        command->reset({}, runtime.dld);
        command->begin(
            { .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit },
            runtime.dld);

        if (isDmaBuf) {
            imageBarrier(*imported.image,
                vk::ImageLayout::eUndefined,
                vk::ImageLayout::eTransferSrcOptimal,
                {}, vk::AccessFlagBits::eTransferRead,
                vk::PipelineStageFlagBits::eTopOfPipe,
                vk::PipelineStageFlagBits::eTransfer);
        } else {
            imageBarrier(*ring[index].image,
                initialized[index]
                    ? vk::ImageLayout::eShaderReadOnlyOptimal
                    : vk::ImageLayout::eUndefined,
                vk::ImageLayout::eTransferDstOptimal,
                initialized[index]
                    ? vk::AccessFlags{vk::AccessFlagBits::eShaderRead}
                    : vk::AccessFlags{},
                vk::AccessFlagBits::eTransferWrite,
                initialized[index]
                    ? vk::PipelineStageFlagBits::eComputeShader
                    : vk::PipelineStageFlagBits::eTopOfPipe,
                vk::PipelineStageFlagBits::eTransfer);
            const vk::BufferImageCopy copy{
                .imageSubresource = {
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .mipLevel = 0,
                    .baseArrayLayer = 0,
                    .layerCount = 1
                },
                .imageExtent = { extent.width, extent.height, 1 }
            };
            command->copyBufferToImage(*staging,
                *ring[index].image,
                vk::ImageLayout::eTransferDstOptimal, { copy },
                runtime.dld);
        }

        if (isDmaBuf) {
            imageBarrier(*ring[index].image,
                initialized[index]
                    ? vk::ImageLayout::eShaderReadOnlyOptimal
                    : vk::ImageLayout::eUndefined,
                vk::ImageLayout::eTransferDstOptimal,
                initialized[index]
                    ? vk::AccessFlags{vk::AccessFlagBits::eShaderRead}
                    : vk::AccessFlags{},
                vk::AccessFlagBits::eTransferWrite,
                initialized[index]
                    ? vk::PipelineStageFlagBits::eComputeShader
                    : vk::PipelineStageFlagBits::eTopOfPipe,
                vk::PipelineStageFlagBits::eTransfer);

            const vk::ImageCopy copy{
                .srcSubresource = {
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .mipLevel = 0,
                    .baseArrayLayer = 0,
                    .layerCount = 1
                },
                .dstSubresource = {
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .mipLevel = 0,
                    .baseArrayLayer = 0,
                    .layerCount = 1
                },
                .extent = { extent.width, extent.height, 1 }
            };
            command->copyImage(*imported.image,
                vk::ImageLayout::eTransferSrcOptimal,
                *ring[index].image,
                vk::ImageLayout::eTransferDstOptimal, { copy },
                runtime.dld);
        }

        imageBarrier(*ring[index].image,
            vk::ImageLayout::eTransferDstOptimal,
            vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::AccessFlagBits::eTransferWrite,
            vk::AccessFlagBits::eShaderRead,
            vk::PipelineStageFlagBits::eTransfer,
            vk::PipelineStageFlagBits::eComputeShader);
        command->end(runtime.dld);

        runtime.device->resetFences(*fence, runtime.dld);
        runtime.queue.submit({{
            .commandBufferCount = 1,
            .pCommandBuffers = &*command
        }}, *fence, runtime.dld);
        const auto result = runtime.device->waitForFences(
            *fence, true, UINT64_MAX, runtime.dld);
        if (result != vk::Result::eSuccess) {
            error = "PipeWire capture fence wait failed";
            return false;
        }
        initialized[index] = true;
        imported.reset();
        return true;
    }

    bool start(std::string& error)
    {
        if (!portal.open(error))
            return false;
        if (!portal.createSession(error))
            return false;
        if (!portal.selectSources(capture == "window", error))
            return false;
        if (!portal.start(error))
            return false;
        if (portal.node == 0) {
            error = "the portal returned no PipeWire node";
            return false;
        }

        int remote = -1;
        if (!portal.openRemote(remote, error))
            return false;

        pw_init(nullptr, nullptr);
        pipewireInitialized = true;
        loop = pw_thread_loop_new("openfg-capture", nullptr);
        if (!loop) {
            error = "cannot create the PipeWire thread loop";
            ::close(remote);
            return false;
        }

        pw_thread_loop_lock(loop);
        context = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
        if (!context) {
            pw_thread_loop_unlock(loop);
            error = "cannot create the PipeWire context";
            ::close(remote);
            return false;
        }
        core = pw_context_connect_fd(context, remote, nullptr, 0);
        if (!core) {
            pw_thread_loop_unlock(loop);
            error = "cannot connect to the portal PipeWire remote";
            return false;
        }

        pw_properties* properties = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Video",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Screen",
            PW_KEY_APP_NAME, "openfg-scaler",
            nullptr);
        stream = pw_stream_new(core, "openfg-scaler", properties);
        if (!stream) {
            pw_thread_loop_unlock(loop);
            error = "cannot create the PipeWire stream";
            return false;
        }

        events = {};
        events.version = PW_VERSION_STREAM_EVENTS;
        events.state_changed = &Impl::onStateChanged;
        events.param_changed = &Impl::onParamChanged;
        events.process = &Impl::onProcess;
        pw_stream_add_listener(stream, &hook, &events, this);

        uint8_t podBuffer[8192];
        spa_pod_builder builder =
            SPA_POD_BUILDER_INIT(podBuffer, sizeof(podBuffer));
        const spa_pod* params[] = { buildFormat(builder) };
        const int connectResult = pw_stream_connect(
            stream, PW_DIRECTION_INPUT, portal.node,
            static_cast<pw_stream_flags>(
                PW_STREAM_FLAG_AUTOCONNECT |
                PW_STREAM_FLAG_MAP_BUFFERS),
            params, 1);
        pw_thread_loop_unlock(loop);
        if (connectResult < 0) {
            error = "cannot connect the PipeWire capture stream";
            return false;
        }
        if (pw_thread_loop_start(loop) < 0) {
            error = "cannot start the PipeWire capture loop";
            return false;
        }

        std::unique_lock lock{mutex};
        if (!condition.wait_for(lock, std::chrono::seconds(15), [this] {
                return formatReady || failed || stopping;
            })) {
            error = "timed out waiting for the PipeWire stream format";
            return false;
        }
        if (failed) {
            error = this->error;
            return false;
        }
        if (!formatReady) {
            error = "the PipeWire stream did not negotiate a format";
            return false;
        }
        lock.unlock();

        if (!createRing(error))
            return false;
        return true;
    }

    void stop()
    {
        {
            std::lock_guard lock{mutex};
            if (stopping)
                return;
            stopping = true;
            condition.notify_all();
        }
        if (loop)
            pw_thread_loop_stop(loop);
        if (stream) {
            pw_stream_disconnect(stream);
            pw_stream_destroy(stream);
            stream = nullptr;
        }
        if (context) {
            pw_context_destroy(context);
            context = nullptr;
            core = nullptr;
        }
        if (loop) {
            pw_thread_loop_destroy(loop);
            loop = nullptr;
        }
        if (pipewireInitialized) {
            pw_deinit();
            pipewireInitialized = false;
        }
        portal.close();
        runtime.device->waitIdle(runtime.dld);

        if (stagingMapped) {
            runtime.device->unmapMemory(*stagingMemory, runtime.dld);
            stagingMapped = nullptr;
        }
        staging = {};
        stagingMemory = {};
        fence = {};
        command = {};
        commandPool = {};
        for (auto& image : ring)
            image = {};
        ringReady = false;
        formatReady = false;
    }

    bool next(Frame& frame, std::string& error)
    {
        if (returned >= 0) {
            busy[returned] = false;
            returned = -1;
        }

        Queued queued;
        {
            std::unique_lock lock{mutex};
            if (!condition.wait_for(lock, std::chrono::seconds(5), [this] {
                    return failed || stopping || !queue.empty();
                })) {
                error = "timed out waiting for a PipeWire frame";
                return false;
            }
            if (failed) {
                error = this->error;
                return false;
            }
            if (queue.empty()) {
                error = "PipeWire capture stopped";
                return false;
            }
            queued = queue.front();
            queue.pop_front();
        }

        if (!ringReady) {
            pw_stream_queue_buffer(stream, queued.buffer);
            error = "the PipeWire capture ring is not ready";
            return false;
        }

        int index = -1;
        {
            std::lock_guard lock{mutex};
            for (int candidate = 0; candidate < 3; candidate++) {
                if (!busy[candidate]) {
                    index = candidate;
                    busy[candidate] = true;
                    break;
                }
            }
        }
        if (index < 0) {
            pw_stream_queue_buffer(stream, queued.buffer);
            error = "all PipeWire capture buffers are busy";
            return false;
        }

        if (!copyFrame(queued, index, error)) {
            {
                std::lock_guard lock{mutex};
                busy[index] = false;
            }
            pw_stream_queue_buffer(stream, queued.buffer);
            return false;
        }
        pw_stream_queue_buffer(stream, queued.buffer);

        frame.image = *ring[index].image;
        frame.view = *ring[index].view;
        frame.extent = extent;
        frame.format = format;
        frame.sequence = ++sequence;
        frame.timestampNs = queued.timestamp;
        returned = index;
        return true;
    }
};

PipeWireSource::PipeWireSource(
    Runtime& runtime,
    vk::Extent2D wanted,
    std::string capture
)
    : impl_{std::make_unique<Impl>(runtime, wanted, std::move(capture))}
{
}

PipeWireSource::~PipeWireSource() = default;

bool PipeWireSource::start(std::string& error)
{
    return impl_->start(error);
}

bool PipeWireSource::next(Frame& frame, std::string& error)
{
    return impl_->next(frame, error);
}

void PipeWireSource::stop()
{
    impl_->stop();
}

std::string PipeWireSource::description() const
{
    if (!impl_->description.empty())
        return impl_->description;
    return "pipewire " + impl_->capture;
}

} // namespace openfg::scaler
