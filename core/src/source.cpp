#include "rumi/rumi.hpp"

#include <algorithm>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <utility>

namespace rumi {
namespace {

thread_local std::shared_ptr<karu_client> cached_transport_client;

std::unexpected<std::string>
transport_error(std::string_view action, const char* uri,
                karu_status status)
{
    const char* detail = karu_last_error();
    std::string message(action);
    if (uri && *uri) {
        message += ' ';
        message += uri;
    }
    message += ": ";
    message += detail && *detail ? detail : karu_status_string(status);
    return std::unexpected(std::move(message));
}

}  // namespace


TransportSession::~TransportSession() = default;

karu_client* TransportSession::client() noexcept
{
    if (initialized_) return client_.get();
    initialized_ = true;

    karu_config* config = nullptr;
    status_ = karu_config_create(&config);
    if (status_ != KARU_OK) return nullptr;

    if (cached_transport_client) {
        int matches = 0;
        status_ = karu_client_matches_config(cached_transport_client.get(),
                                             config, &matches);
        if (status_ != KARU_OK) {
            karu_config_free(config);
            return nullptr;
        }
        if (matches) {
            client_ = cached_transport_client;
            karu_config_free(config);
            return client_.get();
        }
    }

    karu_client* created = nullptr;
    status_ = karu_client_create(config, &created);
    karu_config_free(config);
    if (status_ != KARU_OK) return nullptr;

    try {
        client_ = std::shared_ptr<karu_client>(created, karu_client_free);
    } catch (...) {
        karu_client_free(created);
        status_ = KARU_ERR_NOMEM;
        return nullptr;
    }
    cached_transport_client = client_;
    return client_.get();
}


std::expected<std::unique_ptr<TransportSource>, std::string>
TransportSource::open(const char* path) noexcept
{
    if (!path) {
        return std::unexpected(std::string("could not resolve: path is null"));
    }
    std::unique_ptr<TransportSource> source(
        new (std::nothrow) TransportSource);
    if (!source) {
        return std::unexpected(std::string("out of memory opening ") + path);
    }

    const karu_status status = karu_resolve(path, &source->locator_);
    if (status != KARU_OK) return transport_error("could not resolve", path, status);
    source->remote_ = karu_locator_is_remote(source->locator_) != 0;
    return source;
}

TransportSource::~TransportSource()
{
    karu_locator_free(locator_);
}

std::size_t
TransportSource::read(TransportSession& transport, std::uint64_t offset,
                      std::size_t count, void* buffer) noexcept
{
    if (count == 0) return 0;
    karu_client* client = transport.client();
    if (!client) return 0;

    const karu_req request{
        locator_, offset, static_cast<std::uint64_t>(count), buffer,
        nullptr, nullptr,
    };
    return karu_client_fetch(client, &request, 1) == KARU_OK ? count : 0;
}

std::expected<std::uint64_t, std::string>
TransportSource::size(TransportSession& transport) const
{
    karu_client* client = transport.client();
    if (!client) {
        return transport_error("could not initialize transport for",
                               karu_locator_uri(locator_), transport.status());
    }

    std::uint64_t result = 0;
    const karu_status status = karu_client_size(client, locator_, &result);
    if (status != KARU_OK) {
        return transport_error("could not open", karu_locator_uri(locator_),
                               status);
    }
    return result;
}

const karu_locator* TransportSource::remote_locator() const noexcept
{
    return remote_ ? locator_ : nullptr;
}


std::size_t
MemorySource::read(TransportSession&, std::uint64_t offset,
                   std::size_t count, void* buffer) noexcept
{
    if (offset >= size_) return 0;
    const std::size_t n = std::min<std::uint64_t>(count, size_ - offset);
    std::memcpy(buffer, data_ + offset, n);
    return n;
}

}  // namespace rumi
