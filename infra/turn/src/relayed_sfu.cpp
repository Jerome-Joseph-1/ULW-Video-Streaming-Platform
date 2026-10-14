#include "infra/turn/turn.hpp"

#include "cloudflare.hpp"
#include "hmac.hpp"

#include <utility>

namespace infra::turn {

namespace {

using core::ports::IceServer;
using core::ports::IMediaRoom;
using core::ports::ISfu;
using core::ports::ITurnCredentials;
using core::ports::MediaError;
using core::ports::MediaTicket;

class RelayedRoom final : public IMediaRoom {
public:
    RelayedRoom(std::unique_ptr<IMediaRoom> inner, ITurnCredentials& turn) noexcept
        : inner_(std::move(inner)), turn_(turn) {}

    // The room first, then the credentials: a participant the room refuses costs the TURN
    // service nothing, and the credential is never issued to someone who cannot use it. Only
    // the issuer is captured, which outlives every room, so a room destroyed meanwhile is
    // never touched.
    void join(const core::UserId& user, const core::DeviceId& device, core::ports::MediaRole role,
              core::ports::TicketDone done) override {
        inner_->join(user, device, role,
                     [&turn = turn_, user, device, done = std::move(done)](
                         std::expected<MediaTicket, MediaError> ticket) mutable noexcept {
                         if (!ticket) {
                             done(std::unexpected(ticket.error()));
                             return;
                         }
                         turn.issue(user, device,
                                    [ticket = std::move(*ticket), done = std::move(done)](
                                        std::expected<std::vector<IceServer>, MediaError>
                                            servers) mutable noexcept {
                                        if (!servers) {
                                            done(std::unexpected(servers.error()));
                                            return;
                                        }
                                        ticket.ice_servers = std::move(*servers);
                                        done(std::move(ticket));
                                    });
                     });
    }

    void participants(core::ports::ParticipantsDone done) override {
        inner_->participants(std::move(done));
    }

    void relay(const core::UserId& user, const core::DeviceId& device,
               const core::ports::MediaRelay& target, core::ports::RelayDone done) override {
        inner_->relay(user, device, target, std::move(done));
    }

    void close(core::ports::MediaDone done) override { inner_->close(std::move(done)); }

private:
    std::unique_ptr<IMediaRoom> inner_;
    ITurnCredentials& turn_;
};

class RelayedSfu final : public ISfu {
public:
    RelayedSfu(std::unique_ptr<ISfu> inner, ITurnCredentials& turn) noexcept
        : inner_(std::move(inner)), turn_(turn) {}

    void open_room(const core::RoomId& room, core::ports::MediaGeneration generation,
                   core::ports::MediaRoomKind kind, std::uint16_t max_participants,
                   OpenDone done) override {
        inner_->open_room(
            room, generation, kind, max_participants,
            [&turn = turn_, done = std::move(done)](
                std::expected<std::unique_ptr<IMediaRoom>, MediaError> opened) mutable noexcept {
                if (!opened) {
                    done(std::unexpected(opened.error()));
                    return;
                }
                done(std::unique_ptr<IMediaRoom>(
                    std::make_unique<RelayedRoom>(std::move(*opened), turn)));
            });
    }

    void present(const core::RoomId& room, core::ports::MediaGeneration generation,
                 const core::UserId& user, const core::DeviceId& device,
                 core::ports::PresenceDone done) override {
        inner_->present(room, generation, user, device, std::move(done));
    }

private:
    std::unique_ptr<ISfu> inner_;
    ITurnCredentials& turn_;
};

} // namespace

std::unique_ptr<ITurnCredentials> make_credentials(net::IReactor& reactor, curl::Multi& multi,
                                                   const core::ports::IClock& clock,
                                                   Config config) {
    if (config.provider == Provider::Cloudflare) {
        return std::make_unique<CloudflareCredentials>(reactor, multi, std::move(config));
    }
    return std::make_unique<HmacCredentials>(reactor, clock, std::move(config));
}

std::unique_ptr<ISfu> make_relayed_sfu(std::unique_ptr<ISfu> inner, ITurnCredentials& turn) {
    return std::make_unique<RelayedSfu>(std::move(inner), turn);
}

} // namespace infra::turn
