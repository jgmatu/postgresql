#include "AppBootstrap.hpp"

#include "Files.hpp"

#include "PufTable.hpp"

#include <factory/FactoryChannel.hpp>
#include <factory/SecurityPolicy.hpp>
#include <pki/Bytes.hpp>
#include <pki/CertVerifier.hpp>
#include <pki/Certificate.hpp>
#include <pki/DeviceKey.hpp>

#include <log/Log.hpp>

#include <sys/stat.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>

namespace pgdemo::device {

namespace fs = std::filesystem;

namespace {

/// Serial of a certificate already on disk. Empty when the file does not parse;
/// the startup trace must not stop because of that.
std::string serial_of(const fs::path& path)
{
    try
    {
        return pki::inspect_certificate(read_file(path)).serial;
    }
    catch (const std::exception&)
    {
        return {};
    }
}

std::string with_serial(const std::string& line, const std::string& serial)
{
    return serial.empty() ? line : line + " (serial " + serial + ")";
}

/// A private seed file stands in for the PUF. The first start writes it; later
/// starts read the same bytes. The factory and the controller never see it.
/// True when this call created the file.
bool ensure_seed(const fs::path& dir)
{
    fs::create_directories(dir);
    ::chmod(dir.c_str(), 0700);
    const fs::path seed_path = dir / "seed";
    if (fs::is_regular_file(seed_path)) return false;
    const auto bytes = pki::random_bytes(32);
    {
        const fs::path tmp = seed_path.string() + ".tmp";
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write " + tmp.string());
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) throw std::runtime_error("cannot write " + tmp.string());
    }
    fs::rename(seed_path.string() + ".tmp", seed_path);
    ::chmod(seed_path.c_str(), 0600);
    return true;
}

std::array<std::uint8_t, 32> read_seed(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path.string());
    std::array<std::uint8_t, 32> seed{};
    in.read(reinterpret_cast<char*>(seed.data()), static_cast<std::streamsize>(seed.size()));
    if (in.gcount() != static_cast<std::streamsize>(seed.size()))
        throw std::runtime_error("PUF seed is not 32 bytes");
    return seed;
}

} // namespace

AppBootstrap::AppBootstrap(Options& opts) : opts_(opts) {}

void AppBootstrap::trace(const std::string& message) const
{
    log::info("device " + opts_.name, message);
}

void AppBootstrap::run()
{
    trace(std::format("starting {} type {} payload {}", opts_.mode, opts_.type, opts_.payload));
    fs::create_directories(opts_.dir);
    ::chmod(opts_.dir.c_str(), 0700);
    prepare_identity();
    arm_security();
}

void AppBootstrap::prepare_identity()
{
    const fs::path key_path = opts_.dir / "key.pem";
    const fs::path cert_path = opts_.dir / "cert.pem";
    if (!opts_.puf_dir.empty())
    {
        trace(ensure_seed(opts_.puf_dir) ? "PUF seed created" : "PUF seed already present");
        const fs::path factory_key = opts_.dir / "factory.key";
        const fs::path factory_crt = opts_.dir / "factory.crt";
        if (!fs::exists(factory_key))
        {
            pki::DeviceKey::from_seed(read_seed(opts_.puf_dir / "seed")).save(factory_key.string());
            trace("derived the factory key from its PUF seed");
        }
        else
            trace("factory key already present");
        if (!fs::exists(factory_crt))
        {
            if (opts_.factory.empty() || !fs::exists(factory_key))
                trace("factory certificate absent");
            else
            {
                const auto [host, port] = split_endpoint(opts_.factory);
                auto key = pki::DeviceKey::load(factory_key.string());
                const auto pem =
                    factory::request_signature(host, port, "worker", opts_.name, key.self_signed_pem(opts_.name));
                write_file(factory_crt, pem);
                trace(with_serial("factory signed the certificate", pki::inspect_certificate(pem).serial));
            }
        }
        else
            trace(with_serial("factory certificate already present", serial_of(factory_crt)));
        return;
    }

    trace("no PUF directory");
    if (opts_.device_id && !opts_.puf.empty() && !fs::exists(key_path))
    {
        const auto seed = PufTable(opts_.puf).seed_for(*opts_.device_id);
        pki::DeviceKey::from_seed(seed).save(key_path.string());
        trace("derived the key from devices.puf");
    }
    else if (fs::exists(key_path))
        trace("key.pem already present");
    else
        trace("key.pem absent");

    if (!fs::exists(cert_path))
    {
        if (opts_.factory.empty() || !fs::exists(key_path))
            trace("cert.pem absent");
        else
        {
            const auto [host, port] = split_endpoint(opts_.factory);
            auto key = pki::DeviceKey::load(key_path.string());
            const auto pem = factory::request_signature(host, port, "worker", opts_.name, key.self_signed_pem(opts_.name));
            write_file(cert_path, pem);
            trace(with_serial("factory signed the certificate", pki::inspect_certificate(pem).serial));
        }
    }
    else
        trace(with_serial("cert.pem already present", serial_of(cert_path)));
}

void AppBootstrap::arm_security()
{
    factory::clear_ros_security();
    fs::path key_path = opts_.dir / "key.pem";
    fs::path cert_path = opts_.dir / "cert.pem";
    std::string pair = "key.pem and cert.pem";
    if (fs::exists(opts_.dir / "factory.key") && fs::exists(opts_.dir / "factory.crt"))
    {
        key_path = opts_.dir / "factory.key";
        cert_path = opts_.dir / "factory.crt";
        pair = "factory.key and factory.crt";
    }
    std::string missing;
    auto miss = [&](const std::string& why) {
        if (!missing.empty()) missing += ", ";
        missing += why;
    };
    if (opts_.factory.empty()) miss("no factory endpoint");
    if (opts_.factory_root.empty()) miss("no factory root");
    if (opts_.manager_cert.empty()) miss("no manager certificate");
    if (!fs::exists(key_path)) miss("no key");
    if (!fs::exists(cert_path)) miss("no certificate");
    if (!missing.empty())
    {
        trace("SROS2 stays off: " + missing);
        return;
    }
    trace("SROS2 identity " + pair);
    const auto root = read_file(opts_.factory_root);
    const auto cert = read_file(cert_path);
    const auto manager = read_file(opts_.manager_cert);
    if (!pki::self_signed_root(manager, pki::kManagerCommonName))
    {
        trace("manager certificate is not its own root; SROS2 stays off");
        return;
    }
    opts_.manager_cert_ok = true;
    pki::CertVerifier anchors({root});
    try
    {
        anchors.verify(cert, opts_.name, {});
    }
    catch (const pki::PkiError& e)
    {
        if (e.code == "CERT_CHAIN" || e.code == "CERT_SUBJECT")
        {
            trace("certificate is not from the factory root; SROS2 stays off");
            return;
        }
        throw;
    }
    try
    {
        const auto [host, port] = split_endpoint(opts_.factory);
        const auto governance = factory::request_signed_document(host, port, "governance", "", factory::governance_document());
        trace("SROS2 governance signed");
        const auto permissions =
            factory::request_signed_document(host, port, "permissions", opts_.name, factory::permissions_document(opts_.name));
        trace("SROS2 permissions signed");
        const auto keystore = fs::absolute(opts_.dir.parent_path().parent_path() / "keystore");
        const auto enclave = "/" + node_name_for(opts_.name);
        const auto directory = factory::enclave_directory(keystore, enclave);
        factory::write_enclave(directory, read_file(key_path), cert, factory::identity_ca_bundle(root, manager), root, governance,
                           permissions);
        factory::enable_ros_security(keystore, enclave);
        trace("SROS2 enclave " + directory.string());
    }
    catch (const std::exception& e)
    {
        factory::clear_ros_security();
        log::warn("device " + opts_.name, std::string("SROS2 stays off: ") + e.what());
    }
}

} // namespace pgdemo::device
