#include "conninfo.hpp"

#include "libpq_handles.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <format>
#include <memory>
#include <netdb.h>

namespace infra::postgres {

namespace {

std::vector<std::string> split_list(std::string_view list) {
    std::vector<std::string> out;
    if (list.empty()) {
        return out;
    }
    for (;;) {
        const std::size_t comma = list.find(',');
        out.emplace_back(list.substr(0, comma));
        if (comma == std::string_view::npos) {
            return out;
        }
        list.remove_prefix(comma + 1);
    }
}

void append_item(std::string& list, std::string_view item, bool first) {
    if (!first) {
        list += ',';
    }
    list += item;
}

struct AddrInfoFreer {
    void operator()(addrinfo* info) const noexcept { ::freeaddrinfo(info); }
};

std::vector<std::string> lookup(const std::string& name) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* raw = nullptr;
    if (::getaddrinfo(name.c_str(), nullptr, &hints, &raw) != 0) {
        return {};
    }
    const std::unique_ptr<addrinfo, AddrInfoFreer> list{raw};
    std::vector<std::string> out;
    for (const addrinfo* ai = list.get(); ai != nullptr; ai = ai->ai_next) {
        std::array<char, INET6_ADDRSTRLEN> text{};
        const void* addr = nullptr;
        if (ai->ai_family == AF_INET) {
            addr = &reinterpret_cast<const sockaddr_in*>(ai->ai_addr)->sin_addr;
        } else if (ai->ai_family == AF_INET6) {
            addr = &reinterpret_cast<const sockaddr_in6*>(ai->ai_addr)->sin6_addr;
        } else {
            continue;
        }
        if (::inet_ntop(ai->ai_family, addr, text.data(), text.size()) == nullptr) {
            continue;
        }
        std::string address{text.data()};
        if (std::ranges::find(out, address) == out.end()) {
            out.push_back(std::move(address));
        }
    }
    return out;
}

} // namespace

HostForm classify_host(std::string_view host) noexcept {
    // libpq reads an empty host as the default socket directory, a leading '/' as a socket
    // directory and a leading '@' as an abstract socket name.
    if (host.empty() || host.front() == '/' || host.front() == '@') {
        return HostForm::Socket;
    }
    std::array<char, INET6_ADDRSTRLEN> text{};
    if (host.size() >= text.size()) {
        return HostForm::Name;
    }
    std::ranges::copy(host, text.begin());
    std::array<unsigned char, sizeof(in6_addr)> binary{};
    if (::inet_pton(AF_INET, text.data(), binary.data()) == 1 ||
        ::inet_pton(AF_INET6, text.data(), binary.data()) == 1) {
        return HostForm::Numeric;
    }
    return HostForm::Name;
}

std::expected<ConnTarget, std::string> parse_conninfo(const std::string& conninfo) {
    char* error = nullptr;
    const ConninfoHandle options{PQconninfoParse(conninfo.c_str(), &error)};
    if (!options) {
        std::string message = error != nullptr ? error : "connection string does not parse";
        PQfreemem(error);
        while (!message.empty() && message.back() == '\n') {
            message.pop_back();
        }
        return std::unexpected(std::move(message));
    }
    ConnTarget target;
    for (const PQconninfoOption* o = options.get(); o->keyword != nullptr; ++o) {
        if (o->val == nullptr) {
            continue;
        }
        const std::string_view keyword = o->keyword;
        if (keyword == "host") {
            target.hosts = split_list(o->val);
        } else if (keyword == "port") {
            target.ports = split_list(o->val);
        } else if (keyword == "hostaddr") {
            target.has_hostaddr = *o->val != '\0';
        } else if (keyword == "options") {
            target.options = o->val;
        }
    }
    return target;
}

std::optional<Endpoints> expand_endpoints(const ConnTarget& target,
                                          std::span<const std::vector<std::string>> addresses) {
    if (addresses.size() != target.hosts.size() ||
        (target.ports.size() > 1 && target.ports.size() != target.hosts.size())) {
        return std::nullopt;
    }
    Endpoints out;
    bool first = true;
    for (std::size_t i = 0; i < target.hosts.size(); ++i) {
        const std::string& host = target.hosts[i];
        std::string_view port;
        if (!target.ports.empty()) {
            port = target.ports.size() == 1 ? target.ports.front() : target.ports[i];
        }
        if (classify_host(host) != HostForm::Name) {
            append_item(out.host, host, first);
            append_item(out.hostaddr, "", first);
            append_item(out.port, port, first);
            first = false;
            continue;
        }
        if (addresses[i].empty()) {
            return std::nullopt;
        }
        for (const std::string& address : addresses[i]) {
            append_item(out.host, host, first);
            append_item(out.hostaddr, address, first);
            append_item(out.port, port, first);
            first = false;
        }
    }
    return out;
}

ConnectPlan::ConnectPlan(std::string conninfo, std::string application_name,
                         core::Millis statement_timeout)
    : conninfo_(std::move(conninfo)), application_name_(std::move(application_name)) {
    auto parsed = parse_conninfo(conninfo_);
    if (parsed) {
        target_ = std::move(*parsed);
        lookup_ = !target_.has_hostaddr && std::ranges::any_of(target_.hosts, [](const auto& h) {
            return classify_host(h) == HostForm::Name;
        });
    }
    // The server's own keepalive decides how long a vanished client keeps its session, and with
    // it its advisory locks: 10 s idle, then 3 probes 5 s apart, instead of the kernel's two
    // hours. The connection string's own options come last so that they win.
    options_ = std::format("-c statement_timeout={} -c tcp_keepalives_idle=10 "
                           "-c tcp_keepalives_interval=5 -c tcp_keepalives_count=3",
                           statement_timeout.count());
    if (!target_.options.empty()) {
        options_ += ' ';
        options_ += target_.options;
    }
}

std::optional<Endpoints> ConnectPlan::resolve() const {
    std::vector<std::vector<std::string>> addresses(target_.hosts.size());
    for (std::size_t i = 0; i < target_.hosts.size(); ++i) {
        if (classify_host(target_.hosts[i]) == HostForm::Name) {
            addresses[i] = lookup(target_.hosts[i]);
        }
    }
    return expand_endpoints(target_, addresses);
}

ConnectPlan::Arrays ConnectPlan::arrays(const Endpoints* endpoints) const {
    Arrays out;
    const auto add = [&out](const char* keyword, const char* value) {
        out.keywords.push_back(keyword);
        out.values.push_back(value);
    };
    // Entries before dbname are defaults the connection string may override; entries after it
    // override the string (libpq applies the arrays in order, last non-empty value wins).
    add("fallback_application_name", application_name_.c_str());
    // The client-side twin of the server keepalive above: a session whose server vanished is
    // noticed in 25 s rather than hours.
    add("keepalives_idle", "10");
    add("keepalives_interval", "5");
    add("keepalives_count", "3");
    add("dbname", conninfo_.c_str());
    add("options", options_.c_str());
    if (endpoints != nullptr) {
        add("host", endpoints->host.c_str());
        add("hostaddr", endpoints->hostaddr.c_str());
        add("port", endpoints->port.c_str());
    }
    add(nullptr, nullptr);
    return out;
}

} // namespace infra::postgres
