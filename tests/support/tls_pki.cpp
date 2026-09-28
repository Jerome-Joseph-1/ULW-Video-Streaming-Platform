#include "tls_pki.hpp"

#include <array>
#include <cstdlib>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <stdexcept>
#include <string>

namespace ulw::test {

namespace {

struct KeyFree {
    void operator()(EVP_PKEY* key) const noexcept { EVP_PKEY_free(key); }
};
struct CertFree {
    void operator()(X509* cert) const noexcept { X509_free(cert); }
};
struct ExtFree {
    void operator()(X509_EXTENSION* ext) const noexcept { X509_EXTENSION_free(ext); }
};
struct BioFree {
    void operator()(BIO* bio) const noexcept { BIO_free(bio); }
};
using KeyPtr = std::unique_ptr<EVP_PKEY, KeyFree>;
using CertPtr = std::unique_ptr<X509, CertFree>;

void require(bool ok, const char* what) {
    if (!ok) {
        throw std::runtime_error(std::string("test pki: ") + what);
    }
}

// P-256: signing and handshakes with it take microseconds, where RSA key generation would
// take a noticeable part of a second per identity.
KeyPtr generate_key() {
    KeyPtr key{EVP_EC_gen("P-256")};
    require(key != nullptr, "key generation");
    return key;
}

void add_name(X509_NAME* name, std::string_view common_name) {
    const std::string cn(common_name);
    // X509_NAME takes its text as unsigned bytes.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto* bytes = reinterpret_cast<const unsigned char*>(cn.c_str());
    require(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, bytes, -1, -1, 0) == 1, "name");
}

void add_extension(X509* cert, X509* issuer, int nid, const char* value) {
    X509V3_CTX ctx{};
    X509V3_set_ctx(&ctx, issuer, cert, nullptr, nullptr, 0);
    const std::unique_ptr<X509_EXTENSION, ExtFree> ext{
        X509V3_EXT_conf_nid(nullptr, &ctx, nid, value)};
    require(ext != nullptr && X509_add_ext(cert, ext.get(), -1) == 1, "extension");
}

// Valid from an hour ago, so a clock slightly behind the one that signed it still accepts it,
// for a day, which outlasts any test run.
CertPtr make_cert(EVP_PKEY* key, std::string_view common_name, long serial, X509* issuer,
                  EVP_PKEY* issuer_key) {
    CertPtr cert{X509_new()};
    require(cert != nullptr, "certificate");
    X509* c = cert.get();
    constexpr long kHour = 3600;
    constexpr long kDay = 24 * kHour;
    require(X509_set_version(c, X509_VERSION_3) == 1 &&
                ASN1_INTEGER_set(X509_get_serialNumber(c), serial) == 1 &&
                X509_gmtime_adj(X509_getm_notBefore(c), -kHour) != nullptr &&
                X509_gmtime_adj(X509_getm_notAfter(c), kDay) != nullptr &&
                X509_set_pubkey(c, key) == 1,
            "certificate fields");
    add_name(X509_get_subject_name(c), common_name);
    X509* signer = issuer == nullptr ? c : issuer;
    require(X509_set_issuer_name(c, X509_get_subject_name(signer)) == 1, "issuer");
    if (issuer == nullptr) {
        add_extension(c, signer, NID_basic_constraints, "critical,CA:TRUE");
        add_extension(c, signer, NID_key_usage, "critical,keyCertSign,cRLSign");
    } else {
        add_extension(c, signer, NID_basic_constraints, "critical,CA:FALSE");
        add_extension(c, signer, NID_subject_alt_name, "DNS:localhost,IP:127.0.0.1");
        add_extension(c, signer, NID_ext_key_usage, "serverAuth");
    }
    require(X509_sign(c, issuer_key, EVP_sha256()) > 0, "signature");
    return cert;
}

void write_cert(const std::filesystem::path& path, X509* cert) {
    const std::unique_ptr<BIO, BioFree> out{BIO_new_file(path.c_str(), "w")};
    require(out != nullptr && PEM_write_bio_X509(out.get(), cert) == 1, "write certificate");
}

void write_key(const std::filesystem::path& path, EVP_PKEY* key) {
    const std::unique_ptr<BIO, BioFree> out{BIO_new_file(path.c_str(), "w")};
    require(out != nullptr && PEM_write_bio_PrivateKey(out.get(), key, nullptr, nullptr, 0, nullptr,
                                                       nullptr) == 1,
            "write key");
}

} // namespace

struct TestPki::Keys {
    KeyPtr ca_key;
    CertPtr ca_cert;
};

TestPki& TestPki::shared() {
    static TestPki pki;
    return pki;
}

TestPki::TestPki() : keys_(std::make_unique<Keys>()) {
    std::string tmpl = (std::filesystem::temp_directory_path() / "ulw-pki-XXXXXX").string();
    require(::mkdtemp(tmpl.data()) != nullptr, "temporary directory");
    dir_ = tmpl;
    keys_->ca_key = generate_key();
    keys_->ca_cert =
        make_cert(keys_->ca_key.get(), "ulw test ca", serial_++, nullptr, keys_->ca_key.get());
    write_cert(dir_ / "ca.pem", keys_->ca_cert.get());
    server_ = issue("server", "localhost");
}

TestPki::~TestPki() {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
}

net::TlsFiles TestPki::issue(std::string_view stem, std::string_view common_name) {
    const std::scoped_lock lock(mutex_);
    const KeyPtr key = generate_key();
    const CertPtr cert =
        make_cert(key.get(), common_name, serial_++, keys_->ca_cert.get(), keys_->ca_key.get());
    net::TlsFiles files{.certificate_chain = (dir_ / (std::string(stem) + "-chain.pem")).string(),
                        .private_key = (dir_ / (std::string(stem) + "-key.pem")).string()};
    write_cert(files.certificate_chain, cert.get());
    write_key(files.private_key, key.get());
    return files;
}

SslCtxPtr TestPki::client_context(int max_version) const {
    SslCtxPtr ctx{SSL_CTX_new(TLS_client_method())};
    require(ctx != nullptr, "client context");
    SSL_CTX* c = ctx.get();
    require(SSL_CTX_load_verify_file(c, (dir_ / "ca.pem").c_str()) == 1, "trust the test ca");
    SSL_CTX_set_verify(c, SSL_VERIFY_PEER, nullptr);
    require(X509_VERIFY_PARAM_set1_host(SSL_CTX_get0_param(c), "localhost", 0) == 1, "host");
    require(SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION) == 1, "minimum version");
    if (max_version != 0) {
        require(SSL_CTX_set_max_proto_version(c, max_version) == 1, "maximum version");
    }
    // A test client keeps its session so that a second connection can offer it back.
    SSL_CTX_set_session_cache_mode(c, SSL_SESS_CACHE_CLIENT);
    return ctx;
}

std::string peer_common_name(const SSL* ssl) {
    const CertPtr cert{SSL_get1_peer_certificate(ssl)};
    if (!cert) {
        return {};
    }
    std::array<char, 256> cn{};
    const int n = X509_NAME_get_text_by_NID(X509_get_subject_name(cert.get()), NID_commonName,
                                            cn.data(), static_cast<int>(cn.size()));
    return n > 0 ? std::string(cn.data(), static_cast<std::size_t>(n)) : std::string();
}

} // namespace ulw::test
