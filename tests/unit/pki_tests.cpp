// Device PKI: CSR signing by the CA, verification by the controller, proofs, CRL.
//
// The CA is generated per test run with Botan into a temporary directory,
// with the same profile scripts/gen-device-ca.sh writes.

#include <gtest/gtest.h>

#include <pki/Bytes.hpp>
#include <pki/CertVerifier.hpp>
#include <pki/CertificateAuthority.hpp>
#include <pki/DeviceKey.hpp>
#include <pki/Handshake.hpp>

#include <botan/auto_rng.h>
#include <botan/data_src.h>
#include <botan/ec_group.h>
#include <botan/ecdsa.h>
#include <botan/pkcs8.h>
#include <botan/pkix_enums.h>
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
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace pgdemo::pki;

namespace {

void make_ca(const fs::path& dir, const std::string& cn, std::string_view hash)
{
    fs::create_directories(dir);
    Botan::AutoSeeded_RNG rng;
    const Botan::ECDSA_PrivateKey key(rng, Botan::EC_Group::from_name("secp384r1"));
    Botan::X509_Cert_Options opts;
    opts.common_name = cn;
    opts.CA_key(0);
    opts.constraints = Botan::Key_Constraints::ca_constraints();
    const auto cert = Botan::X509::create_self_signed_cert(opts, key, std::string(hash), rng);
    std::ofstream key_out(dir / "ca.key");
    key_out << Botan::PKCS8::PEM_encode(key);
    std::ofstream cert_out(dir / "ca.crt");
    cert_out << cert.PEM_encode();
    if (!key_out || !cert_out) throw std::runtime_error("cannot write the test CA");
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
        make_ca(dir_ / "a", "test root A", "SHA-384");
        make_ca(dir_ / "b", "test root B", "SHA-384");
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

TEST_F(PkiTest, PresentedCertificateIsSha256AndTheCsrIsNot)
{
    const auto key = DeviceKey::generate();
    const auto csr = ca_->sign_csr(key.csr_pem("sensor-a"), "sensor-a");
    const auto presented =
        ca_->sign_presented(key.self_signed_pem("sensor-a"), "sensor-a", CertificateAuthority::PresentedUse::Worker);
    EXPECT_FALSE(signed_with_sha256(csr.pem));
    EXPECT_TRUE(signed_with_sha256(presented.pem));
    EXPECT_FALSE(signed_with_sha256("not a certificate"));
}

TEST_F(PkiTest, SelfSignedRootRequiresSha256AndTheExpectedName)
{
    make_ca(dir_ / "sha256", std::string(kManagerCommonName), "SHA-256");
    const auto root = read_file(dir_ / "sha256" / "ca.crt");
    EXPECT_TRUE(self_signed_root(root, kManagerCommonName));
    EXPECT_FALSE(self_signed_root(root, "other"));
    EXPECT_FALSE(self_signed_root(ca_->certificate_pem(), "test root A"));

    const auto leaf = DeviceKey::generate();
    const auto cert = ca_->sign_csr(leaf.csr_pem("sensor-a"), "sensor-a");
    EXPECT_FALSE(self_signed_root(cert.pem, "sensor-a"));
    EXPECT_FALSE(self_signed_root("not a certificate", kManagerCommonName));
}

TEST_F(PkiTest, SmimeSignatureVerifiesWithTheCa)
{
    const std::string document = "hello\nworld\n";
    const auto smime = ca_->sign_document(document);
    const auto in = dir_ / "doc.p7s";
    const auto out = dir_ / "recovered.txt";
    const auto log = dir_ / "smime.log";
    {
        std::ofstream file(in);
        file << smime;
    }
    const std::string cmd = "openssl smime -verify -text -in " + in.string() + " -CAfile " +
                            (dir_ / "a" / "ca.crt").string() + " -out " + out.string() + " >" + log.string() + " 2>&1";
    ASSERT_EQ(std::system(cmd.c_str()), 0) << read_file(log);

    auto recovered = read_file(out);
    for (std::size_t at = 0; (at = recovered.find("\r\n", at)) != std::string::npos;) recovered.replace(at, 2, "\n");
    EXPECT_EQ(recovered, document);

    auto tampered = smime;
    const auto letter = tampered.find("hello");
    ASSERT_NE(letter, std::string::npos);
    tampered[letter] = 'j';
    {
        std::ofstream file(in);
        file << tampered;
    }
    EXPECT_NE(std::system(cmd.c_str()), 0);
}

TEST_F(PkiTest, HexRoundTripsAndRejectsMalformedText)
{
    const std::uint8_t raw[]{0x0a, 0xff};
    EXPECT_EQ(to_hex(raw), "0aff");
    EXPECT_EQ(from_hex("0aff"), (std::vector<std::uint8_t>{0x0a, 0xff}));
    EXPECT_THROW(from_hex("zz"), std::invalid_argument);
    EXPECT_THROW(from_hex("abc"), std::invalid_argument);
}

TEST_F(PkiTest, ProofMessageBindsNonceNameAndFingerprint)
{
    const std::vector<std::uint8_t> nonce{1, 2, 3};
    const auto msg = proof_message(nonce, "sensor-a", "abcd");
    std::vector<std::uint8_t> expect(kProofContext.begin(), kProofContext.end());
    expect.push_back(0);
    expect.insert(expect.end(), nonce.begin(), nonce.end());
    expect.push_back(0);
    const std::string name = "sensor-a";
    expect.insert(expect.end(), name.begin(), name.end());
    expect.push_back(0);
    const std::string fingerprint = "abcd";
    expect.insert(expect.end(), fingerprint.begin(), fingerprint.end());
    EXPECT_EQ(msg, expect);
}

TEST_F(PkiTest, ManagerCrlPayloadIsCanonical)
{
    const std::set<std::string> serials{"0b", "0a"};
    const auto payload = manager_crl_payload(serials);
    EXPECT_EQ(payload, "pgdemo-manager-crl-v1\n0a\n0b\n");
    EXPECT_EQ(manager_crl_serials(payload), serials);
    EXPECT_EQ(expect_code([&] { (void)manager_crl_serials("nope\n"); }), "CERT_CHAIN");
    EXPECT_EQ(expect_code([&] { (void)manager_crl_serials("pgdemo-manager-crl-v1\n0b\n0a\n"); }), "CERT_CHAIN");
}

TEST_F(PkiTest, SeedMustBe32Bytes)
{
    std::array<std::uint8_t, 31> short_seed{};
    EXPECT_THROW(DeviceKey::from_seed(short_seed), std::invalid_argument);
}

TEST_F(PkiTest, UnknownCrlReasonBecomesUnspecified)
{
    const auto key = DeviceKey::generate();
    const auto cert = ca_->sign_csr(key.csr_pem("sensor-a"), "sensor-a");
    const auto pem = ca_->crl_pem({{cert.pem, "not-a-reason"}});
    Botan::DataSource_Memory src(pem);
    const Botan::X509_CRL crl(src);
    ASSERT_EQ(crl.get_revoked().size(), 1u);
    EXPECT_EQ(crl.get_revoked()[0].reason_code(), Botan::CRL_Code::Unspecified);
}

} // namespace
