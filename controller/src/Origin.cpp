#include "Origin.hpp"

#include <pki/CertVerifier.hpp>
#include <pki/Certificate.hpp>
#include <pki/DeviceKey.hpp>
#include <pki/FactoryChannel.hpp>

#include <db/DatabaseManager.hpp>
#include <log/Log.hpp>

#include <chrono>
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

} // namespace

void start_origin(db::DatabaseManager& db, FleetOptions& fleet, const OriginFiles& files,
                  std::unique_ptr<pki::HandshakeListener>& listener, std::jthread& thread)
{
    namespace fs = std::filesystem;
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

        auto key = pki::DeviceKey::load(files.manager_key);
        std::string manager_pem;
        if (!db.manager_certificate_present())
        {
            const auto presented = key.self_signed_pem(pki::kManagerCommonName, true);
            const auto issued = pki::request_signature(files.factory_host, files.factory_port, "manager",
                                                       pki::kManagerCommonName, presented);
            const auto info = pki::inspect_certificate(issued);
            db.register_manager_certificate(root.fingerprint, info.serial, info.fingerprint, info.subject_cn,
                                            pki::format_time(info.not_before), pki::format_time(info.not_after),
                                            info.pem);
            manager_pem = info.pem;
            log::info(kLog, "manager certificate signed by the factory");
        }
        else
        {
            manager_pem = *db.load_manager_certificate_pem();
        }

        const auto serials = db.load_manager_revoked_serials();
        const auto payload = pki::manager_crl_payload(serials);
        const auto signature = key.sign(bytes_of(payload));
        db.replace_manager_crl(payload, pki::to_hex(signature));

        listener = std::make_unique<pki::HandshakeListener>(read_file(files.manager_key), manager_pem, root_pem);
        fleet.puf_file = fleet.devices_dir / "devices.puf";
        fleet.factory_root = fs::absolute(files.factory_cert);
        fleet.factory_addr = files.factory_host + ":" + std::to_string(files.factory_port);
        fleet.manager_tls = "127.0.0.1:" + std::to_string(listener->port());
        const auto ca_fp = root.fingerprint;
        thread = std::jthread([&, manager_pem, root_pem, ca_fp](std::stop_token stop) {
            while (!stop.stop_requested())
            {
                if (!listener->wait(std::chrono::milliseconds(200))) continue;
                try
                {
                    const auto peer = listener->accept_one();
                    const auto listed = db.load_manager_revoked_serials();
                    const auto crl = db.load_manager_crl();
                    if (!crl) throw pki::PkiError("CERT_CHAIN", "manager CRL is not signed");
                    if (crl->payload != pki::manager_crl_payload(listed))
                        throw pki::PkiError("CERT_CHAIN", "manager CRL does not match the stored serials");
                    pki::verify_factory_pair(peer.pem, manager_pem, root_pem, peer.common_name, crl->payload,
                                             pki::from_hex(crl->signature_hex));
                    const auto info = pki::inspect_certificate(peer.pem);
                    if (!db.register_factory_worker(info.subject_cn, ca_fp, info.serial, info.fingerprint,
                                                    pki::format_time(info.not_before), pki::format_time(info.not_after),
                                                    info.pem))
                        log::info(kLog, peer.common_name + " authenticated; no device row to attach the certificate");
                    else
                        log::info(kLog, peer.common_name + " authenticated");
                }
                catch (const std::exception& e)
                {
                    log::warn(kLog, std::string("handshake rejected: ") + e.what());
                }
            }
        });
        log::info(kLog, "mutual handshake on " + fleet.manager_tls);
    }
    catch (const std::exception& e)
    {
        if (thread.joinable())
        {
            thread.request_stop();
            thread.join();
        }
        listener.reset();
        fleet.puf_file.clear();
        fleet.factory_root.clear();
        fleet.factory_addr.clear();
        fleet.manager_tls.clear();
        log::warn(kLog, std::string("factory model not started: ") + e.what());
    }
}

} // namespace pgdemo::controller
