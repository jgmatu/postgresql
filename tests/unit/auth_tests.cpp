#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include <botan/auto_rng.h>
#include <botan/ec_group.h>
#include <botan/ecdsa.h>
#include <botan/pkcs8.h>
#include <botan/x509self.h>

#include <auth/JwtManager.hpp>
#include <auth/PasswordHasher.hpp>

namespace fs = std::filesystem;
using pgdemo::auth::JwtManager;
using pgdemo::auth::PasswordHasher;

namespace {

/// Cheap parameters: the tests check behaviour, not cost.
const PasswordHasher::Params kFast{8192, 1, 1};

struct KeyPair
{
    fs::path key;
    fs::path cert;
};

KeyPair write_keypair(const fs::path& dir, const std::string& name)
{
    Botan::AutoSeeded_RNG rng;
    Botan::ECDSA_PrivateKey key(rng, Botan::EC_Group::from_name("secp384r1"));
    Botan::X509_Cert_Options opts("pgdemo-test");
    const auto cert = Botan::X509::create_self_signed_cert(opts, key, "SHA-384", rng);

    KeyPair kp{dir / (name + ".key"), dir / (name + ".crt")};
    std::ofstream(kp.key) << Botan::PKCS8::PEM_encode(key);
    std::ofstream(kp.cert) << cert.PEM_encode();
    return kp;
}

std::string segment(const std::string& token, int index)
{
    std::size_t start = 0;
    for (int i = 0; i < index; ++i) start = token.find('.', start) + 1;
    const auto end = token.find('.', start);
    return token.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

class JwtTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        dir_ = fs::temp_directory_path() / ("pgdemo-jwt-" + std::to_string(::getpid()));
        fs::create_directories(dir_);
        a_ = write_keypair(dir_, "a");
        b_ = write_keypair(dir_, "b");
    }
    static void TearDownTestSuite() { fs::remove_all(dir_); }

    static inline fs::path dir_;
    static inline KeyPair a_;
    static inline KeyPair b_;
};

} // namespace

// ------------------------------------------------------------- PasswordHasher

TEST(PasswordHasher, ProducesArgon2idPhcString)
{
    const auto h = PasswordHasher::hash("correct horse battery staple", kFast);
    EXPECT_TRUE(h.starts_with("$argon2id$v=19$m=8192,t=1,p=1$")) << h;
}

TEST(PasswordHasher, VerifiesOnlyTheRightPassword)
{
    const auto h = PasswordHasher::hash("s3cret-password", kFast);
    EXPECT_TRUE(PasswordHasher::verify("s3cret-password", h));
    EXPECT_FALSE(PasswordHasher::verify("s3cret-passwore", h));
    EXPECT_FALSE(PasswordHasher::verify("", h));
}

TEST(PasswordHasher, SaltMakesEveryHashDifferent)
{
    EXPECT_NE(PasswordHasher::hash("same", kFast), PasswordHasher::hash("same", kFast));
}

TEST(PasswordHasher, MalformedHashIsRejectedWithoutThrowing)
{
    EXPECT_FALSE(PasswordHasher::verify("x", ""));
    EXPECT_FALSE(PasswordHasher::verify("x", "plaintext"));
    EXPECT_FALSE(PasswordHasher::verify("x", "$argon2id$v=19$m=8192,t=1,p=1$bad$bad"));
}

TEST(PasswordHasher, DummyVerificationDoesNotThrow)
{
    EXPECT_NO_THROW(PasswordHasher::verify_dummy("anything"));
}

// ----------------------------------------------------------------- JwtManager

TEST_F(JwtTest, RoundTrip)
{
    JwtManager jwt(a_.key.string(), a_.cert.string());
    const auto token = jwt.generate("alice", "normal", 60);
    const auto claims = jwt.verify(token);
    ASSERT_TRUE(claims);
    EXPECT_EQ(claims->sub, "alice");
    EXPECT_EQ(claims->role, "normal");
    EXPECT_EQ(claims->exp - claims->iat, 60u);
}

TEST_F(JwtTest, SignatureIsRawP384)
{
    JwtManager jwt(a_.key.string(), a_.cert.string());
    const auto sig = JwtManager::base64url_decode(segment(jwt.generate("a", "admin"), 2));
    EXPECT_EQ(sig.size(), 96u); // r || s, 48 bytes each (RFC 7518 section 3.4)
}

TEST_F(JwtTest, RejectsTokenFromAnotherKey)
{
    JwtManager a(a_.key.string(), a_.cert.string());
    JwtManager b(b_.key.string(), b_.cert.string());
    EXPECT_FALSE(a.verify(b.generate("mallory", "admin")));
}

TEST_F(JwtTest, RejectsTamperedPayload)
{
    JwtManager jwt(a_.key.string(), a_.cert.string());
    const auto token = jwt.generate("alice", "normal");
    const auto forged_payload = JwtManager::base64url_encode(
        std::string(R"({"iss":"pgdemo","sub":"alice","role":"admin","iat":1,"exp":99999999999})"));
    const auto forged = segment(token, 0) + "." + forged_payload + "." + segment(token, 2);
    EXPECT_FALSE(jwt.verify(forged));
}

TEST_F(JwtTest, RejectsAlgNone)
{
    JwtManager jwt(a_.key.string(), a_.cert.string());
    const auto header = JwtManager::base64url_encode(std::string(R"({"alg":"none","typ":"JWT"})"));
    const auto payload = JwtManager::base64url_encode(
        std::string(R"({"iss":"pgdemo","sub":"alice","role":"admin","iat":1,"exp":99999999999})"));
    EXPECT_FALSE(jwt.verify(header + "." + payload + "."));
}

TEST_F(JwtTest, RejectsExpiredToken)
{
    JwtManager jwt(a_.key.string(), a_.cert.string());
    EXPECT_FALSE(jwt.verify(jwt.generate("alice", "normal", 0)));
}

TEST_F(JwtTest, RejectsMalformedTokens)
{
    JwtManager jwt(a_.key.string(), a_.cert.string());
    EXPECT_FALSE(jwt.verify(""));
    EXPECT_FALSE(jwt.verify("a.b"));
    EXPECT_FALSE(jwt.verify("a.b.c.d"));
    EXPECT_FALSE(jwt.verify("!!!.???.***"));
}

TEST_F(JwtTest, MissingKeyFileThrows)
{
    EXPECT_THROW(JwtManager((dir_ / "missing.key").string(), a_.cert.string()), std::runtime_error);
}

TEST(Base64Url, RoundTripWithoutPadding)
{
    for (const std::string s : {"", "f", "fo", "foo", "foob", "fooba", "foobar", "\xff\xfe\xfd"})
    {
        const auto enc = JwtManager::base64url_encode(s);
        EXPECT_EQ(enc.find('='), std::string::npos);
        EXPECT_EQ(enc.find('+'), std::string::npos);
        EXPECT_EQ(enc.find('/'), std::string::npos);
        const auto dec = JwtManager::base64url_decode(enc);
        EXPECT_EQ(std::string(dec.begin(), dec.end()), s);
    }
}
