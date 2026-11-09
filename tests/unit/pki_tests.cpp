// Device PKI: CSR signing by the CA, verification by the controller, proofs, CRL.
//
// The CA is generated per test run with openssl into a temporary directory,
// exactly as scripts/gen-device-ca.sh does.

#include <gtest/gtest.h>

#include <pki/CertVerifier.hpp>
#include <pki/CertificateAuthority.hpp>
#include <pki/DeviceKey.hpp>

#include <botan/auto_rng.h>
#include <botan/data_src.h>
#include <botan/ec_group.h>
#include <botan/ecdsa.h>
#include <botan/rsa.h>
#include <botan/x509_crl.h>
#include <botan/x509self.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

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

} // namespace
