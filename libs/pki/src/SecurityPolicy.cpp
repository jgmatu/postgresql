#include <pki/SecurityPolicy.hpp>

#include <sys/stat.h>

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace pgdemo::pki {

namespace {

constexpr std::string_view kGovernance = R"(<?xml version="1.0" encoding="UTF-8"?>
<dds xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:noNamespaceSchemaLocation="http://www.omg.org/spec/DDS-SECURITY/20170901/omg_shared_ca_governance.xsd">
<domain_access_rules>
<domain_rule>
<domains>
<id_range>
<min>0</min>
<max>232</max>
</id_range>
</domains>
<allow_unauthenticated_participants>false</allow_unauthenticated_participants>
<enable_join_access_control>true</enable_join_access_control>
<discovery_protection_kind>SIGN</discovery_protection_kind>
<liveliness_protection_kind>SIGN</liveliness_protection_kind>
<rtps_protection_kind>SIGN</rtps_protection_kind>
<topic_access_rules>
<topic_rule>
<topic_expression>*</topic_expression>
<enable_discovery_protection>true</enable_discovery_protection>
<enable_liveliness_protection>true</enable_liveliness_protection>
<enable_read_access_control>true</enable_read_access_control>
<enable_write_access_control>true</enable_write_access_control>
<metadata_protection_kind>SIGN</metadata_protection_kind>
<data_protection_kind>SIGN</data_protection_kind>
</topic_rule>
</topic_access_rules>
</domain_rule>
</domain_access_rules>
</dds>
)";

bool device_name(std::string_view cn)
{
    if (cn.empty() || cn.size() > 64) return false;
    for (const unsigned char c : cn)
        if (!(std::isalnum(c) || c == '_' || c == '.' || c == '-')) return false;
    return true;
}

void write_exact(const std::filesystem::path& path, std::string_view data, mode_t mode)
{
    std::filesystem::create_directories(path.parent_path());
    const auto tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc | std::ios::binary);
        if (!out) throw std::runtime_error("cannot write " + tmp);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
    }
    std::filesystem::rename(tmp, path);
    ::chmod(path.c_str(), mode);
}

} // namespace

std::string governance_document() { return std::string(kGovernance); }

std::string permissions_document(std::string_view cn)
{
    if (!device_name(cn)) throw std::invalid_argument("permissions subject is not a device name");
    const std::string name(cn);
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<dds xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
           "xsi:noNamespaceSchemaLocation=\"http://www.omg.org/spec/DDS-SECURITY/20170901/"
           "omg_shared_ca_permissions.xsd\">\n"
           "<permissions>\n"
           "<grant name=\"" +
           name +
           "\">\n"
           "<subject_name>CN=" +
           name +
           "</subject_name>\n"
           "<validity>\n"
           "<not_before>2020-01-01T00:00:00</not_before>\n"
           "<not_after>2099-01-01T00:00:00</not_after>\n"
           "</validity>\n"
           "<allow_rule>\n"
           "<domains>\n"
           "<id_range>\n"
           "<min>0</min>\n"
           "<max>232</max>\n"
           "</id_range>\n"
           "</domains>\n"
           "<publish>\n"
           "<topics>\n"
           "<topic>ros_discovery_info</topic>\n"
           "<topic>rq/*Request</topic>\n"
           "<topic>rr/*Reply</topic>\n"
           "<topic>rt/*</topic>\n"
           "</topics>\n"
           "</publish>\n"
           "<subscribe>\n"
           "<topics>\n"
           "<topic>ros_discovery_info</topic>\n"
           "<topic>rq/*Request</topic>\n"
           "<topic>rr/*Reply</topic>\n"
           "<topic>rt/*</topic>\n"
           "</topics>\n"
           "</subscribe>\n"
           "</allow_rule>\n"
           "<default>DENY</default>\n"
           "</grant>\n"
           "</permissions>\n"
           "</dds>\n";
}

std::filesystem::path enclave_directory(const std::filesystem::path& keystore, std::string_view enclave)
{
    if (enclave.empty() || enclave.front() != '/') throw std::invalid_argument("enclave name must be a ROS name");
    return keystore / "enclaves" / std::string(enclave.substr(1));
}

std::string identity_ca_bundle(std::string_view factory_pem, std::string_view manager_pem)
{
    std::string out(factory_pem);
    if (out.empty() || out.back() != '\n') out.push_back('\n');
    out.append(manager_pem);
    if (out.empty() || out.back() != '\n') out.push_back('\n');
    return out;
}

void write_enclave(const std::filesystem::path& directory, std::string_view key_pem, std::string_view cert_pem,
                   std::string_view identity_ca_pem, std::string_view permissions_ca_pem,
                   std::string_view governance_p7s, std::string_view permissions_p7s)
{
    std::filesystem::create_directories(directory);
    ::chmod(directory.c_str(), 0700);
    write_exact(directory / "key.pem", key_pem, 0600);
    write_exact(directory / "cert.pem", cert_pem, 0644);
    write_exact(directory / "identity_ca.cert.pem", identity_ca_pem, 0644);
    write_exact(directory / "permissions_ca.cert.pem", permissions_ca_pem, 0644);
    write_exact(directory / "governance.p7s", governance_p7s, 0644);
    write_exact(directory / "permissions.p7s", permissions_p7s, 0644);
}

void enable_ros_security(const std::filesystem::path& keystore, std::string_view enclave)
{
    if (::setenv("ROS_SECURITY_ENABLE", "true", 1) != 0 || ::setenv("ROS_SECURITY_STRATEGY", "Enforce", 1) != 0 ||
        ::setenv("ROS_SECURITY_KEYSTORE", keystore.c_str(), 1) != 0 ||
        ::setenv("ROS_SECURITY_ENCLAVE_OVERRIDE", std::string(enclave).c_str(), 1) != 0)
        throw std::runtime_error("cannot export ROS_SECURITY_*");
}

void clear_ros_security()
{
    ::unsetenv("ROS_SECURITY_ENABLE");
    ::unsetenv("ROS_SECURITY_STRATEGY");
    ::unsetenv("ROS_SECURITY_KEYSTORE");
    ::unsetenv("ROS_SECURITY_ENCLAVE_OVERRIDE");
}

} // namespace pgdemo::pki
