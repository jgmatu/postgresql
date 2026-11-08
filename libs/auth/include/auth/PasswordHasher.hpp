#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace pgdemo::auth {

/**
 * @brief Argon2id password hashing (Botan), PHC string format.
 *
 * Output: `$argon2id$v=19$m=65536,t=3,p=1$<salt b64>$<hash b64>`. The parameters
 * travel inside the string, so stored hashes stay verifiable after a parameter
 * change.
 */
class PasswordHasher
{
public:
    /// Argon2id cost parameters, written into every hash.
    struct Params
    {
        std::size_t memory_kib{65536}; ///< m: 64 MiB
        std::size_t iterations{3};     ///< t
        std::size_t parallelism{1};    ///< p
    };

    /// Hash with a fresh 16-byte random salt.
    static std::string hash(std::string_view password, const Params& params);
    /// Hash with the default `Params`.
    static std::string hash(std::string_view password) { return hash(password, Params{}); }

    /// False for a wrong password or a malformed hash. Never throws.
    static bool verify(std::string_view password, std::string_view phc_hash) noexcept;

    /**
     * @brief Spend the same work as a real verification, against a fixed hash.
     *
     * Called when the user does not exist, so the login response time does not
     * reveal whether a username is registered.
     */
    static void verify_dummy(std::string_view password) noexcept;
};

} // namespace pgdemo::auth
