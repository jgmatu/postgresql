#include <auth/PasswordHasher.hpp>

#include <botan/argon2fmt.h>
#include <botan/auto_rng.h>

namespace pgdemo::auth {

std::string PasswordHasher::hash(std::string_view password, const Params& params)
{
    Botan::AutoSeeded_RNG rng;
    return Botan::argon2_generate_pwhash(password.data(), password.size(), rng, params.parallelism,
                                         params.memory_kib, params.iterations, /*Argon2id*/ 2);
}

bool PasswordHasher::verify(std::string_view password, std::string_view phc_hash) noexcept
{
    try
    {
        return Botan::argon2_check_pwhash(password.data(), password.size(), phc_hash);
    }
    catch (...)
    {
        return false;
    }
}

void PasswordHasher::verify_dummy(std::string_view password) noexcept
{
    // Same parameters as hash(); the salt and digest are arbitrary.
    static const std::string kDummy = [] {
        try
        {
            return hash("pgdemo-timing-equaliser");
        }
        catch (...)
        {
            return std::string();
        }
    }();
    (void)verify(password, kDummy);
}

} // namespace pgdemo::auth
