#include "conninfo.hpp"

#include <algorithm>
#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <vector>

namespace {

using infra::postgres::classify_host;
using infra::postgres::ConnectPlan;
using infra::postgres::ConnTarget;
using infra::postgres::expand_endpoints;
using infra::postgres::HostForm;
using infra::postgres::parse_conninfo;
using infra::postgres::session_options;

TEST(ClassifyHost, SocketsNumericAddressesAndNames) {
    EXPECT_EQ(classify_host(""), HostForm::Socket);
    EXPECT_EQ(classify_host("/var/run/postgresql"), HostForm::Socket);
    EXPECT_EQ(classify_host("@pgsock"), HostForm::Socket);
    EXPECT_EQ(classify_host("127.0.0.1"), HostForm::Numeric);
    EXPECT_EQ(classify_host("::1"), HostForm::Numeric);
    EXPECT_EQ(classify_host("fd00:5::17"), HostForm::Numeric);
    EXPECT_EQ(classify_host("localhost"), HostForm::Name);
    EXPECT_EQ(classify_host("db.internal"), HostForm::Name);
    EXPECT_EQ(classify_host("256.1.1.1"), HostForm::Name);
}

TEST(ParseConninfo, ReadsHostPortListsAndOptionsFromAUrl) {
    const auto target =
        parse_conninfo("postgresql://ulw:secret@db-a.internal:5433,10.0.0.2:5434/ulw"
                       "?options=-c%20work_mem%3D4MB");
    ASSERT_TRUE(target) << target.error();
    EXPECT_EQ(target->hosts, (std::vector<std::string>{"db-a.internal", "10.0.0.2"}));
    EXPECT_EQ(target->ports, (std::vector<std::string>{"5433", "5434"}));
    EXPECT_EQ(target->options, "-c work_mem=4MB");
    EXPECT_FALSE(target->has_hostaddr);
}

TEST(ParseConninfo, NoticesAnExplicitHostaddr) {
    const auto target = parse_conninfo("host=db.internal hostaddr=10.0.0.5 dbname=ulw");
    ASSERT_TRUE(target);
    EXPECT_TRUE(target->has_hostaddr);
}

TEST(ParseConninfo, ReportsAStringLibpqCannotParse) {
    EXPECT_FALSE(parse_conninfo("host='unterminated"));
    EXPECT_FALSE(parse_conninfo("postgresql://[::1"));
}

TEST(ExpandEndpoints, NamesBecomeOneEntryPerAddressAndOthersStayAsGiven) {
    const ConnTarget target{.hosts = {"db.internal", "10.0.0.2", "/tmp"},
                            .ports = {"5432"},
                            .has_hostaddr = false,
                            .options = {}};
    const std::vector<std::vector<std::string>> addresses{{"10.0.0.5", "fd00::5"}, {}, {}};
    const auto endpoints = expand_endpoints(target, addresses);
    ASSERT_TRUE(endpoints);
    EXPECT_EQ(endpoints->host, "db.internal,db.internal,10.0.0.2,/tmp");
    EXPECT_EQ(endpoints->hostaddr, "10.0.0.5,fd00::5,,");
    EXPECT_EQ(endpoints->port, "5432,5432,5432,5432");
}

TEST(ExpandEndpoints, EachHostKeepsItsOwnPort) {
    const ConnTarget target{.hosts = {"a.internal", "b.internal"},
                            .ports = {"5433", "5434"},
                            .has_hostaddr = false,
                            .options = {}};
    const std::vector<std::vector<std::string>> addresses{{"10.0.0.1", "10.0.0.2"}, {"10.0.0.3"}};
    const auto endpoints = expand_endpoints(target, addresses);
    ASSERT_TRUE(endpoints);
    EXPECT_EQ(endpoints->port, "5433,5433,5434");
}

TEST(ExpandEndpoints, FailsWhenANameHasNoAddress) {
    const ConnTarget target{
        .hosts = {"10.0.0.2", "gone.internal"}, .ports = {}, .has_hostaddr = false, .options = {}};
    const std::vector<std::vector<std::string>> addresses{{}, {}};
    EXPECT_FALSE(expand_endpoints(target, addresses));
}

TEST(ExpandEndpoints, FailsWhenPortsDoNotMatchHosts) {
    const ConnTarget target{
        .hosts = {"a", "b", "c"}, .ports = {"1", "2"}, .has_hostaddr = false, .options = {}};
    const std::vector<std::vector<std::string>> addresses{{"10.0.0.1"}, {"10.0.0.2"}, {"10.0.0.3"}};
    EXPECT_FALSE(expand_endpoints(target, addresses));
}

TEST(ConnectPlan, LooksUpOnlyNamesWithoutAHostaddr) {
    const core::Millis timeout{5000};
    EXPECT_TRUE(ConnectPlan("postgresql://u@db.internal/ulw", "t", timeout).needs_lookup());
    EXPECT_FALSE(ConnectPlan("postgresql://u@127.0.0.1/ulw", "t", timeout).needs_lookup());
    EXPECT_FALSE(ConnectPlan("host=/tmp dbname=ulw", "t", timeout).needs_lookup());
    EXPECT_FALSE(
        ConnectPlan("host=db.internal hostaddr=10.0.0.5 dbname=ulw", "t", timeout).needs_lookup());
}

TEST(ConnectPlan, ResolvesANameIntoAddressesForLibpq) {
    const ConnectPlan plan("host=localhost port=5432 dbname=ulw", "t", core::Millis{5000});
    ASSERT_TRUE(plan.needs_lookup());
    const auto endpoints = plan.resolve();
    ASSERT_TRUE(endpoints);
    // One entry per address, each naming the host for TLS and handing libpq a numeric address,
    // which it connects to without a lookup of its own.
    std::string_view hostaddrs = endpoints->hostaddr;
    std::size_t entries = 0;
    for (;;) {
        const std::size_t comma = hostaddrs.find(',');
        EXPECT_EQ(classify_host(hostaddrs.substr(0, comma)), HostForm::Numeric) << hostaddrs;
        ++entries;
        if (comma == std::string_view::npos) {
            break;
        }
        hostaddrs.remove_prefix(comma + 1);
    }
    EXPECT_TRUE(endpoints->host.starts_with("localhost"));
    EXPECT_EQ(std::ranges::count(endpoints->host, ','), static_cast<std::ptrdiff_t>(entries) - 1);
}

std::ptrdiff_t index_of(const std::vector<const char*>& keywords, std::string_view keyword) {
    const auto it =
        std::ranges::find_if(keywords, [&](const char* k) { return k != nullptr && keyword == k; });
    return it == keywords.end() ? -1 : it - keywords.begin();
}

TEST(ConnectPlan, DefaultsPrecedeTheStringAndOverridesFollowIt) {
    const ConnectPlan plan("postgresql://u@db.internal/ulw?options=-c%20work_mem%3D4MB",
                           "ulw-catalog", core::Millis{4000});
    const infra::postgres::Endpoints endpoints{
        .host = "db.internal", .hostaddr = "10.0.0.5", .port = ""};
    const auto arrays = plan.arrays(&endpoints);
    ASSERT_EQ(arrays.keywords.size(), arrays.values.size());
    EXPECT_EQ(arrays.keywords.back(), nullptr);
    const auto dbname = index_of(arrays.keywords, "dbname");
    ASSERT_GE(dbname, 0);
    // libpq lets later entries win: keepalive defaults stay overridable by the string, while
    // the options and the resolved address replace what the string says.
    EXPECT_LT(index_of(arrays.keywords, "keepalives_idle"), dbname);
    EXPECT_LT(index_of(arrays.keywords, "fallback_application_name"), dbname);
    const auto options = index_of(arrays.keywords, "options");
    const auto hostaddr = index_of(arrays.keywords, "hostaddr");
    EXPECT_GT(options, dbname);
    EXPECT_GT(hostaddr, dbname);
    EXPECT_STREQ(arrays.values[static_cast<std::size_t>(hostaddr)], "10.0.0.5");
    const std::string merged = arrays.values[static_cast<std::size_t>(options)];
    EXPECT_NE(merged.find("-c statement_timeout=4000"), std::string::npos);
    // The string's own options come last, so they win over ours.
    EXPECT_TRUE(merged.ends_with(" -c work_mem=4MB")) << merged;
}

TEST(ConnectPlan, WithoutEndpointsLeavesTheHostToTheString) {
    const ConnectPlan plan("postgresql://u@127.0.0.1/ulw", "t", core::Millis{4000});
    const auto arrays = plan.arrays(nullptr);
    EXPECT_EQ(index_of(arrays.keywords, "hostaddr"), -1);
    EXPECT_EQ(index_of(arrays.keywords, "host"), -1);
}

TEST(SessionOptions, BoundsStatementsAndIdleTransactionsAlike) {
    const std::string options = session_options(core::Millis{9900}, "");
    EXPECT_NE(options.find("-c statement_timeout=9900"), std::string::npos);
    EXPECT_NE(options.find("-c idle_in_transaction_session_timeout=9900"), std::string::npos);
    EXPECT_NE(options.find("-c tcp_keepalives_idle=10"), std::string::npos);
}

TEST(SessionOptions, ZeroTimeoutLeavesStatementsUnboundedButKeepsKeepalives) {
    const std::string options = session_options(core::Millis{0}, "-c work_mem=4MB");
    EXPECT_EQ(options.find("statement_timeout"), std::string::npos);
    EXPECT_EQ(options.find("idle_in_transaction"), std::string::npos);
    EXPECT_NE(options.find("-c tcp_keepalives_count=3"), std::string::npos);
    EXPECT_TRUE(options.ends_with(" -c work_mem=4MB")) << options;
}

} // namespace
