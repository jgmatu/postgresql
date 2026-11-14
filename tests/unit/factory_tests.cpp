// Factory wire protocol and the SROS2 documents it agrees to sign.

#include <gtest/gtest.h>

#include <factory/FactoryChannel.hpp>
#include <factory/SecurityPolicy.hpp>
#include <pki/Bytes.hpp>
#include <pki/CertVerifier.hpp>
#include <pki/CertificateAuthority.hpp>
#include <pki/DeviceKey.hpp>

#include <botan/auto_rng.h>
#include <botan/ec_group.h>
#include <botan/ecdsa.h>
#include <botan/pkcs8.h>
#include <botan/pkix_enums.h>
#include <botan/x509self.h>

#include <atomic>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace pgdemo::factory;
using namespace pgdemo::pki;

namespace {

void make_ca(const fs::path& dir, const std::string& cn)
{
    fs::create_directories(dir);
    Botan::AutoSeeded_RNG rng;
    const Botan::ECDSA_PrivateKey key(rng, Botan::EC_Group::from_name("secp384r1"));
    Botan::X509_Cert_Options opts;
    opts.common_name = cn;
    opts.CA_key(0);
    opts.constraints = Botan::Key_Constraints::ca_constraints();
    const auto cert = Botan::X509::create_self_signed_cert(opts, key, "SHA-384", rng);
    std::ofstream key_out(dir / "ca.key");
    key_out << Botan::PKCS8::PEM_encode(key);
    std::ofstream cert_out(dir / "ca.crt");
    cert_out << cert.PEM_encode();
    if (!key_out || !cert_out) throw std::runtime_error("cannot write the test CA");
}

std::span<const std::uint8_t> bytes_of(std::string_view text)
{
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

class FactoryTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        dir_ = fs::temp_directory_path() / ("pgdemo-factory-" + std::to_string(::getpid()));
        make_ca(dir_, "test root A");
        ca_ = std::make_unique<CertificateAuthority>((dir_ / "ca.key").string(), (dir_ / "ca.crt").string());
    }
    static void TearDownTestSuite()
    {
        ca_.reset();
        fs::remove_all(dir_);
    }

    static inline fs::path dir_;
    static inline std::unique_ptr<CertificateAuthority> ca_;
};

TEST_F(FactoryTest, ChannelReturnsTheSignedCertificate)
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
    verify_factory_pair(pem, manager_cert.pem, ca_->certificate_pem(), "sensor-a", payload,
                        manager.sign(bytes_of(payload)));
    const auto probe = random_bytes(32);
    EXPECT_TRUE(CertVerifier::verify_signature(pem, probe, worker.sign(probe)));
}

TEST_F(FactoryTest, SignsOnlyThePolicyTemplate)
{
    std::atomic<bool> stop{false};
    std::atomic<std::uint16_t> bound{0};
    std::thread server([&] { serve_factory(0, *ca_, stop, &bound); });
    for (int i = 0; i < 200 && bound.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_NE(bound.load(), 0);

    const auto governance = request_signed_document("127.0.0.1", bound.load(), "governance", "", governance_document());
    EXPECT_NE(governance.find("allow_unauthenticated_participants"), std::string::npos);
    EXPECT_NE(governance.find("multipart/signed"), std::string::npos);
    auto tampered = governance_document();
    const auto at = tampered.find(">false<");
    ASSERT_NE(at, std::string::npos);
    tampered.replace(at, 7, ">true<");
    EXPECT_THROW(
        [&] {
            const auto ignored = request_signed_document("127.0.0.1", bound.load(), "governance", "", tampered);
            static_cast<void>(ignored);
        }(),
        std::runtime_error);

    const auto permissions =
        request_signed_document("127.0.0.1", bound.load(), "permissions", "sensor-a", permissions_document("sensor-a"));
    EXPECT_NE(permissions.find("CN=sensor-a"), std::string::npos);
    EXPECT_THROW(
        [] {
            const auto ignored = permissions_document("not a name");
            static_cast<void>(ignored);
        }(),
        std::invalid_argument);

    stop.store(true);
    server.join();
}

} // namespace
