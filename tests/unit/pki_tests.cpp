// Device PKI: CSR signing by the CA, verification by the controller, proofs, CRL.
//
// The CA is generated per test run with openssl into a temporary directory,
// exactly as scripts/gen-device-ca.sh does.

#include <gtest/gtest.h>

#include <pki/CertVerifier.hpp>
#include <pki/CertificateAuthority.hpp>
#include <pki/DeviceKey.hpp>
#include <pki/FactoryChannel.hpp>
#include <pki/Handshake.hpp>
#include <pki/PufTable.hpp>

#include <botan/auto_rng.h>
#include <botan/data_src.h>
#include <botan/ec_group.h>
#include <botan/ecdsa.h>
#include <botan/rsa.h>
#include <botan/x509_crl.h>
#include <botan/x509self.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace pgdemo::pki;

namespace {

std::string make_ca(const fs::path& dir, const std::string& cn)
{
    fs::create_directories(dir);
    const auto key = (dir / "ca.key").string();
    const auto crt = (dir / "ca.crt").string();
    const std::string cmd =
        "openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-384 -out " + key +
        " 2>/dev/null && openssl req -new -x509 -key " + key + " -sha384 -days 30 -subj '/CN=" + cn +
        "' -addext basicConstraints=critical,CA:TRUE,pathlen:0 -addext keyUsage=critical,keyCertSign,cRLSign"
        " -addext subjectKeyIdentifier=hash -out " +
        crt + " 2>/dev/null";
    if (std::system(cmd.c_str()) != 0) throw std::runtime_error("openssl failed");
    return crt;
}

std::string read_file(const fs::path& p)
{
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string expect_code(auto&& fn)
{
    try
    {
        fn();
    }
    catch (const PkiError& e)
    {
        return e.code;
    }
    return "no error";
}

class PkiTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        dir_ = fs::temp_directory_path() / ("pgdemo-pki-" + std::to_string(::getpid()));
        make_ca(dir_ / "a", "test root A");
        make_ca(dir_ / "b", "test root B");
        ca_ = std::make_unique<CertificateAuthority>((dir_ / "a" / "ca.key").string(),
                                                     (dir_ / "a" / "ca.crt").string());
        other_ = std::make_unique<CertificateAuthority>((dir_ / "b" / "ca.key").string(),
                                                        (dir_ / "b" / "ca.crt").string());
    }
    static void TearDownTestSuite()
    {
        ca_.reset();
        other_.reset();
        fs::remove_all(dir_);
    }

    static inline fs::path dir_;
    static inline std::unique_ptr<CertificateAuthority> ca_;
    static inline std::unique_ptr<CertificateAuthority> other_;
};

TEST_F(PkiTest, SignsACsrWithTheFixedClientProfile)
{
    const auto key = DeviceKey::generate();
    const auto cert = ca_->sign_csr(key.csr_pem("sensor-a"), "sensor-a");
    EXPECT_EQ(cert.subject_cn, "sensor-a");
    EXPECT_EQ(cert.fingerprint.size(), 64u);
    EXPECT_GE(cert.serial.size(), 30u);
    EXPECT_LT(cert.not_before, std::chrono::system_clock::now());
    EXPECT_GT(cert.not_after, std::chrono::system_clock::now() + std::chrono::days(364));

    Botan::DataSource_Memory src(cert.pem);
    const Botan::X509_Certificate x(src);
    EXPECT_FALSE(x.is_CA_cert());
    EXPECT_TRUE(x.allowed_usage(Botan::Usage_Type::TLS_CLIENT_AUTH));
    EXPECT_FALSE(x.allowed_usage(Botan::Usage_Type::TLS_SERVER_AUTH));
    EXPECT_EQ(fs::path(dir_ / "a" / "ca.crt").empty(), false);
    EXPECT_EQ(ca_->fingerprint().size(), 64u);
}

TEST_F(PkiTest, RejectsACsrForAnotherName)
{
    const auto key = DeviceKey::generate();
    EXPECT_EQ(expect_code([&] { ca_->sign_csr(key.csr_pem("sensor-b"), "sensor-a"); }), "CSR_SUBJECT");
}

TEST_F(PkiTest, RejectsWeakOrForeignKeyTypes)
{
    Botan::AutoSeeded_RNG rng;
    Botan::X509_Cert_Options opts;
    opts.common_name = "sensor-a";

    const Botan::RSA_PrivateKey rsa(rng, 2048);
    const auto rsa_csr = Botan::X509::create_cert_req(opts, rsa, "SHA-256", rng).PEM_encode();
    EXPECT_EQ(expect_code([&] { ca_->sign_csr(rsa_csr, "sensor-a"); }), "CSR_KEY");

    const Botan::ECDSA_PrivateKey p256(rng, Botan::EC_Group::from_name("secp256r1"));
    const auto p256_csr = Botan::X509::create_cert_req(opts, p256, "SHA-256", rng).PEM_encode();
    EXPECT_EQ(expect_code([&] { ca_->sign_csr(p256_csr, "sensor-a"); }), "CSR_KEY");
}

TEST_F(PkiTest, RejectsGarbageAndCaRequests)
{
    EXPECT_EQ(expect_code([&] { ca_->sign_csr("not a csr", "sensor-a"); }), "CSR_MALFORMED");

    Botan::AutoSeeded_RNG rng;
    const Botan::ECDSA_PrivateKey key(rng, Botan::EC_Group::from_name("secp384r1"));
    Botan::X509_Cert_Options opts;
    opts.common_name = "sensor-a";
    opts.CA_key();
    const auto csr = Botan::X509::create_cert_req(opts, key, "SHA-384", rng).PEM_encode();
    EXPECT_EQ(expect_code([&] { ca_->sign_csr(csr, "sensor-a"); }), "CSR_CA");
}

TEST_F(PkiTest, VerifierAcceptsTheChainAndTheProof)
{
    const auto key = DeviceKey::generate();
    const auto cert = ca_->sign_csr(key.csr_pem("sensor-a"), "sensor-a");
    const CertVerifier verifier({ca_->certificate_pem()});
    EXPECT_EQ(verifier.verify(cert.pem, "sensor-a", {}).fingerprint, cert.fingerprint);

    const auto nonce = random_bytes(32);
    const auto msg = proof_message(nonce, "sensor-a", cert.fingerprint);
    const auto sig = key.sign(msg);
    EXPECT_EQ(sig.size(), 96u);
    EXPECT_TRUE(CertVerifier::verify_signature(cert.pem, msg, sig));
}

TEST_F(PkiTest, ProofFromAnotherKeyOrForAnotherNonceFails)
{
    const auto key = DeviceKey::generate();
    const auto thief = DeviceKey::generate();
    const auto cert = ca_->sign_csr(key.csr_pem("sensor-a"), "sensor-a");
    const auto msg = proof_message(random_bytes(32), "sensor-a", cert.fingerprint);
    EXPECT_FALSE(CertVerifier::verify_signature(cert.pem, msg, thief.sign(msg)));

    const auto replay = proof_message(random_bytes(32), "sensor-a", cert.fingerprint);
    EXPECT_FALSE(CertVerifier::verify_signature(cert.pem, replay, key.sign(msg)));
}

TEST_F(PkiTest, VerifierRejectsUnknownCaRevokedSerialWrongNameAndExpiry)
{
    const auto key = DeviceKey::generate();
    const auto cert = ca_->sign_csr(key.csr_pem("sensor-a"), "sensor-a", std::chrono::days(2));
    const auto foreign = other_->sign_csr(key.csr_pem("sensor-a"), "sensor-a");
    const CertVerifier verifier({ca_->certificate_pem()});

    EXPECT_EQ(expect_code([&] { verifier.verify(foreign.pem, "sensor-a", {}); }), "CERT_CHAIN");
    EXPECT_EQ(expect_code([&] { verifier.verify(cert.pem, "sensor-a", {cert.serial}); }), "CERT_REVOKED");
    EXPECT_EQ(expect_code([&] { verifier.verify(cert.pem, "sensor-b", {}); }), "CERT_SUBJECT");
    EXPECT_EQ(expect_code([&] {
                  verifier.verify(cert.pem, "sensor-a", {}, std::chrono::system_clock::now() + std::chrono::days(3));
              }),
              "CERT_CHAIN");
    EXPECT_EQ(expect_code([&] { verifier.verify("garbage", "sensor-a", {}); }), "CERT_MALFORMED");
}

TEST_F(PkiTest, CrlListsTheRevokedSerialsAndIsSignedByTheCa)
{
    const auto k1 = DeviceKey::generate();
    const auto k2 = DeviceKey::generate();
    const auto c1 = ca_->sign_csr(k1.csr_pem("sensor-a"), "sensor-a");
    const auto c2 = ca_->sign_csr(k2.csr_pem("sensor-b"), "sensor-b");
    const auto pem = ca_->crl_pem({{c1.pem, "keyCompromise"}, {c2.pem, "certificateHold"}});

    Botan::DataSource_Memory src(pem);
    const Botan::X509_CRL crl(src);
    ASSERT_EQ(crl.get_revoked().size(), 2u);
    EXPECT_EQ(to_hex(crl.get_revoked()[0].serial().magnitude()), c1.serial);
    EXPECT_EQ(crl.get_revoked()[0].reason_code(), Botan::CRL_Code::KeyCompromise);
    EXPECT_EQ(crl.get_revoked()[1].reason_code(), Botan::CRL_Code::CertificateHold);

    Botan::DataSource_Memory ca_src(ca_->certificate_pem());
    const Botan::X509_Certificate ca_cert(ca_src);
    EXPECT_TRUE(crl.check_signature(*ca_cert.subject_public_key()));
}

TEST_F(PkiTest, KeySurvivesSaveAndLoadWithMode0600)
{
    const auto path = (dir_ / "device.key").string();
    const auto key = DeviceKey::generate();
    key.save(path);
    EXPECT_EQ(fs::status(path).permissions() & fs::perms::all, fs::perms::owner_read | fs::perms::owner_write);
    const auto loaded = DeviceKey::load(path);
    const auto cert = ca_->sign_csr(key.csr_pem("sensor-a"), "sensor-a");
    const auto msg = proof_message(random_bytes(32), "sensor-a", cert.fingerprint);
    EXPECT_TRUE(CertVerifier::verify_signature(cert.pem, msg, loaded.sign(msg)));
    EXPECT_FALSE(read_file(path).empty());
}

std::span<const std::uint8_t> bytes_of(std::string_view text)
{
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

TEST_F(PkiTest, SameSeedDerivesTheSamePkcs8)
{
    std::array<std::uint8_t, 32> seed{};
    for (std::size_t i = 0; i < seed.size(); ++i) seed[i] = static_cast<std::uint8_t>(i + 1);
    const auto a = (dir_ / "seed-a.key").string();
    const auto b = (dir_ / "seed-b.key").string();
    DeviceKey::from_seed(seed).save(a);
    DeviceKey::from_seed(seed).save(b);
    EXPECT_EQ(read_file(a), read_file(b));
    seed[0] = static_cast<std::uint8_t>(seed[0] ^ 0xff);
    const auto c = (dir_ / "seed-c.key").string();
    DeviceKey::from_seed(seed).save(c);
    EXPECT_NE(read_file(a), read_file(c));
}

TEST_F(PkiTest, PufTableKeepsTheSeedOfADevice)
{
    const auto path = dir_ / "devices.puf";
    PufTable table(path);
    const auto first = table.seed_for(7);
    EXPECT_EQ(table.seed_for(7), first);
    EXPECT_NE(table.seed_for(8), first);
    EXPECT_EQ(fs::status(path).permissions() & fs::perms::all, fs::perms::owner_read | fs::perms::owner_write);
    const auto again = (dir_ / "puf-again.key").string();
    const auto once = (dir_ / "puf-once.key").string();
    DeviceKey::from_seed(first).save(once);
    DeviceKey::from_seed(PufTable(path).seed_for(7)).save(again);
    EXPECT_EQ(read_file(once), read_file(again));
}

TEST_F(PkiTest, FactorySignsTheWorkerAndTheManager)
{
    const auto worker = DeviceKey::generate();
    const auto manager = DeviceKey::generate();
    const auto worker_cert =
        ca_->sign_presented(worker.self_signed_pem("sensor-a"), "sensor-a", CertificateAuthority::PresentedUse::Worker);
    const auto manager_cert = ca_->sign_presented(manager.self_signed_pem(std::string(kManagerCommonName), true),
                                                  std::string(kManagerCommonName),
                                                  CertificateAuthority::PresentedUse::Manager);
    const auto root = ca_->certificate_pem();
    const auto payload = manager_crl_payload({});
    const auto signature = manager.sign(bytes_of(payload));
    verify_factory_pair(worker_cert.pem, manager_cert.pem, root, "sensor-a", payload, signature);

    EXPECT_EQ(expect_code([&] {
                  verify_factory_pair(worker_cert.pem, manager_cert.pem, other_->certificate_pem(), "sensor-a", payload,
                                      signature);
              }),
              "CERT_CHAIN");
    EXPECT_EQ(expect_code([&] {
                  verify_factory_pair(worker_cert.pem, manager_cert.pem, root, "other", payload, signature);
              }),
              "CERT_SUBJECT");
    const auto revoked = manager_crl_payload({worker_cert.serial});
    const auto revoked_sig = manager.sign(bytes_of(revoked));
    EXPECT_EQ(expect_code([&] {
                  verify_factory_pair(worker_cert.pem, manager_cert.pem, root, "sensor-a", revoked, revoked_sig);
              }),
              "CERT_REVOKED");
    EXPECT_EQ(expect_code([&] {
                  verify_factory_pair(worker_cert.pem, manager_cert.pem, root, "sensor-a", payload, worker.sign(bytes_of(payload)));
              }),
              "CERT_CHAIN");

    CertVerifier demo({other_->certificate_pem()});
    EXPECT_EQ(expect_code([&] { demo.verify(worker_cert.pem, "sensor-a", {}); }), "CERT_CHAIN");
}

TEST_F(PkiTest, FactoryChannelReturnsTheSignedCertificate)
{
    const auto worker = DeviceKey::generate();
    const auto presented = worker.self_signed_pem("sensor-a");
    std::atomic<bool> stop{false};
    std::atomic<std::uint16_t> bound{0};
    std::thread server([&] { serve_factory(0, *ca_, stop, &bound); });
    for (int i = 0; i < 200 && bound.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_NE(bound.load(), 0);
    const auto pem = request_signature("127.0.0.1", bound.load(), "worker", "sensor-a", presented);
    stop.store(true);
    server.join();

    const auto manager = DeviceKey::generate();
    const auto manager_cert = ca_->sign_presented(manager.self_signed_pem(std::string(kManagerCommonName), true),
                                                  std::string(kManagerCommonName),
                                                  CertificateAuthority::PresentedUse::Manager);
    const auto payload = manager_crl_payload({});
    verify_factory_pair(pem, manager_cert.pem, ca_->certificate_pem(), "sensor-a", payload, manager.sign(bytes_of(payload)));
    const auto probe = random_bytes(32);
    EXPECT_TRUE(CertVerifier::verify_signature(pem, probe, worker.sign(probe)));
}

TEST_F(PkiTest, HandshakeAuthenticatesTheWorkerAndHonoursTheCrl)
{
    const auto worker = DeviceKey::generate();
    const auto manager = DeviceKey::generate();
    const auto worker_key = (dir_ / "hs-worker.key").string();
    const auto manager_key = (dir_ / "hs-manager.key").string();
    worker.save(worker_key);
    manager.save(manager_key);
    const auto worker_cert =
        ca_->sign_presented(worker.self_signed_pem("sensor-a"), "sensor-a", CertificateAuthority::PresentedUse::Worker);
    const auto manager_cert = ca_->sign_presented(manager.self_signed_pem(std::string(kManagerCommonName), true),
                                                  std::string(kManagerCommonName),
                                                  CertificateAuthority::PresentedUse::Manager);
    const auto root = ca_->certificate_pem();
    const auto payload = manager_crl_payload({});
    const auto signature = manager.sign(bytes_of(payload));
    const auto worker_pem = read_file(worker_key);
    const auto manager_pem = read_file(manager_key);

    HandshakeListener listener(manager_pem, manager_cert.pem, root);
    std::string peer_fingerprint;
    std::exception_ptr failure;
    std::thread accepted([&] {
        try
        {
            const auto peer = listener.accept_one();
            verify_factory_pair(peer.pem, manager_cert.pem, root, "sensor-a", payload, signature);
            peer_fingerprint = inspect_certificate(peer.pem).fingerprint;
        }
        catch (...)
        {
            failure = std::current_exception();
        }
    });
    const auto manager_peer =
        handshake_connect("127.0.0.1", listener.port(), worker_pem, worker_cert.pem, root, kManagerCommonName);
    accepted.join();
    if (failure) std::rethrow_exception(failure);
    EXPECT_EQ(peer_fingerprint, worker_cert.fingerprint);
    EXPECT_EQ(inspect_certificate(manager_peer.pem).fingerprint, manager_cert.fingerprint);

    const auto revoked = manager_crl_payload({worker_cert.serial});
    const auto revoked_sig = manager.sign(bytes_of(revoked));
    std::string revoked_code;
    std::thread rejected([&] {
        try
        {
            const auto peer = listener.accept_one();
            verify_factory_pair(peer.pem, manager_cert.pem, root, "sensor-a", revoked, revoked_sig);
        }
        catch (const PkiError& e)
        {
            revoked_code = e.code;
        }
    });
    (void)handshake_connect("127.0.0.1", listener.port(), worker_pem, worker_cert.pem, root, kManagerCommonName);
    rejected.join();
    EXPECT_EQ(revoked_code, "CERT_REVOKED");

    std::thread rejected_root([&] {
        try
        {
            (void)listener.accept_one();
        }
        catch (...)
        {
        }
    });
    EXPECT_EQ(expect_code([&] {
                  (void)handshake_connect("127.0.0.1", listener.port(), worker_pem, worker_cert.pem,
                                          other_->certificate_pem(), kManagerCommonName);
              }),
              "CERT_CHAIN");
    rejected_root.join();
}

} // namespace
