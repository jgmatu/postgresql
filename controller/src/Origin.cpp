#include "Origin.hpp"

#include <pki/CertVerifier.hpp>
#include <pki/Certificate.hpp>
#include <pki/DeviceKey.hpp>
#include <pki/FactoryChannel.hpp>
#include <pki/SecurityPolicy.hpp>

#include <db/DatabaseManager.hpp>
#include <log/Log.hpp>

#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>

namespace pgdemo::controller {

namespace {

constexpr std::string_view kLog = "controller";

std::string read_file(const std::string& path)
{
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot read " + path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::span<const std::uint8_t> bytes_of(std::string_view text)
{
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

void clear_origin(FleetOptions& fleet)
{
    pki::clear_ros_security();
    fleet.puf_file.clear();
    fleet.factory_root.clear();
    fleet.factory_addr.clear();
    fleet.factory_fingerprint.clear();
    fleet.manager_cert.clear();
    fleet.manager_key.clear();
}

} // namespace

void start_origin(db::DatabaseManager& db, FleetOptions& fleet, const OriginFiles& files)
{
    namespace fs = std::filesystem;
    clear_origin(fleet);
    if (!fs::is_regular_file(files.manager_key) || !fs::is_regular_file(files.factory_cert))
    {
        log::debug(kLog, "factory model off (manager key or factory certificate is absent)");
        return;
    }

    try
    {
        const auto root_pem = read_file(files.factory_cert);
        const auto root = pki::inspect_certificate(root_pem);
        db.ensure_factory_root("CN=" + root.subject_cn, root.fingerprint, root.pem, pki::format_time(root.not_before),
                               pki::format_time(root.not_after));

        const auto cert_path = fs::path(files.manager_key).replace_extension("crt");
        if (!fs::is_regular_file(cert_path))
            throw std::runtime_error("manager root " + cert_path.string() + " is absent");
        const auto manager_pem = read_file(cert_path.string());
        if (!pki::self_signed_root(manager_pem, pki::kManagerCommonName))
            throw std::runtime_error("manager root is not a self-signed ECDSA-SHA256 CA");
        const auto info = pki::inspect_certificate(manager_pem);
        const auto stored = db.manager_certificate_present() ? db.load_manager_certificate_pem() : std::nullopt;
        if (!stored || *stored != info.pem)
        {
            db.register_manager_certificate(info.serial, info.fingerprint, info.subject_cn,
                                            pki::format_time(info.not_before), pki::format_time(info.not_after),
                                            info.pem);
            log::info(kLog, "manager root registered");
        }
        auto key = pki::DeviceKey::load(files.manager_key);

        const auto serials = db.load_manager_revoked_serials();
        const auto payload = pki::manager_crl_payload(serials);
        db.replace_manager_crl(payload, pki::to_hex(key.sign(bytes_of(payload))));

        const auto governance = pki::request_signed_document(files.factory_host, files.factory_port, "governance", "",
                                                             pki::governance_document());
        const auto permissions = pki::request_signed_document(files.factory_host, files.factory_port, "permissions",
                                                              pki::kManagerCommonName,
                                                              pki::permissions_document(pki::kManagerCommonName));
        const auto keystore = fs::absolute(fleet.devices_dir.parent_path() / "keystore");
        constexpr std::string_view kEnclave = "/pgdemo_controller";
        pki::write_enclave(pki::enclave_directory(keystore, kEnclave), read_file(files.manager_key), manager_pem,
                           pki::identity_ca_bundle(root_pem, manager_pem), root_pem, governance, permissions);
        pki::enable_ros_security(keystore, kEnclave);

        fleet.puf_file = fleet.devices_dir / "devices.puf";
        fleet.factory_root = fs::absolute(files.factory_cert);
        fleet.factory_addr = files.factory_host + ":" + std::to_string(files.factory_port);
        fleet.factory_fingerprint = root.fingerprint;
        fleet.manager_cert = fs::absolute(cert_path);
        fleet.manager_key = files.manager_key;
        log::info(kLog, "SROS2 enclave " + pki::enclave_directory(keystore, kEnclave).string());
    }
    catch (const std::exception& e)
    {
        clear_origin(fleet);
        log::warn(kLog, std::string("factory model off: ") + e.what());
    }
}

} // namespace pgdemo::controller
